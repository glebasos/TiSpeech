using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace TiSpeech;

/// <summary>Plays PCM without changing the synthesized voice or its speed.</summary>
public interface IPcmPlayer : IDisposable
{
    string? UnavailableReason { get; }
    Task PlayAsync(byte[] samples, int sampleRate, Action started, CancellationToken cancellationToken);
    void Pause();
    void Resume();
}

/// <summary>
/// Uses macOS afplay or Linux paplay/aplay for audio output. No shell is used;
/// only a private temporary WAV path is passed to the player. The child owns
/// the audio device and is killed and reaped on cancellation.
/// </summary>
public sealed partial class SystemPcmPlayer : IPcmPlayer
{
    private readonly Lock _sync = new();
    private readonly string? _executable = FindPlayer();
    private Process? _process;
    private bool _paused, _disposed;

    public string? UnavailableReason => _executable is not null ? null
        : OperatingSystem.IsLinux()
            ? "Audio playback needs paplay (pulseaudio-utils) or aplay (alsa-utils). WAV export is still available."
            : "No supported audio player was found. WAV export is still available.";

    private static string? FindPlayer()
    {
        if (OperatingSystem.IsMacOS()) return File.Exists("/usr/bin/afplay") ? "/usr/bin/afplay" : null;
        if (!OperatingSystem.IsLinux()) return null;
        foreach (var name in new[] { "paplay", "aplay" })
            foreach (var dir in (Environment.GetEnvironmentVariable("PATH") ?? "/usr/bin:/bin").Split(Path.PathSeparator))
            {
                if (!Path.IsPathFullyQualified(dir)) continue;
                var path = Path.Combine(dir, name);
                if (File.Exists(path)) return path;
            }
        return null;
    }

    public async Task PlayAsync(byte[] samples, int sampleRate, Action started, CancellationToken cancellationToken)
    {
        if (_executable is null) throw new InvalidOperationException(UnavailableReason);
        cancellationToken.ThrowIfCancellationRequested();
        // CreateTempSubdirectory creates a private (0700 on Unix) directory.
        var directory = Directory.CreateTempSubdirectory("opentalkit-");
        var path = Path.Combine(directory.FullName, "speech.wav");
        using var process = new Process
        {
            StartInfo = new ProcessStartInfo(_executable)
            {
                UseShellExecute = false, RedirectStandardError = true, CreateNoWindow = true,
            },
        };
        process.StartInfo.ArgumentList.Add(path);
        try
        {
            using (var file = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                PcmWaveFile.Write(file, samples, sampleRate);
            lock (_sync)
            {
                ObjectDisposedException.ThrowIf(_disposed, this);
                if (_process is not null) throw new InvalidOperationException("Audio is already playing.");
                cancellationToken.ThrowIfCancellationRequested();
                if (!process.Start()) throw new InvalidOperationException("The audio player did not start.");
                _process = process;
                if (_paused) SignalPause(process, true);
            }
            var error = process.StandardError.ReadToEndAsync();
            using var registration = cancellationToken.Register(() => Kill(process));
            cancellationToken.ThrowIfCancellationRequested();
            started();
            await process.WaitForExitAsync(CancellationToken.None).ConfigureAwait(false);
            var detail = await error.ConfigureAwait(false);
            cancellationToken.ThrowIfCancellationRequested();
            if (process.ExitCode != 0)
                throw new InvalidOperationException("Audio playback failed: " +
                    (string.IsNullOrWhiteSpace(detail) ? $"player exited with code {process.ExitCode}." : detail.Trim()));
        }
        finally
        {
            // Also reap a process if setup, pause, or the started callback failed.
            Kill(process);
            try { await process.WaitForExitAsync().ConfigureAwait(false); }
            catch (InvalidOperationException) { /* not started */ }
            lock (_sync) { if (ReferenceEquals(_process, process)) _process = null; }
            try { directory.Delete(recursive: true); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
    }

    private static void Kill(Process process)
    {
        try { if (!process.HasExited) process.Kill(); }
        catch (InvalidOperationException) { }
        catch (Win32Exception) { }
    }

    // SIGSTOP/SIGCONT values are platform-specific; neither signal terminates
    // the process, so its current sample position is retained when paused.
    [LibraryImport("libc", EntryPoint = "kill", SetLastError = true)]
    private static partial int SendSignal(int pid, int signal);

    private static void SignalPause(Process process, bool pause)
    {
        if (process.HasExited) return;
        int signal = OperatingSystem.IsMacOS() ? (pause ? 17 : 19) : (pause ? 19 : 18);
        if (SendSignal(process.Id, signal) != 0 && Marshal.GetLastPInvokeError() != 3) // ESRCH: already exited
            throw new Win32Exception(Marshal.GetLastPInvokeError());
    }

    private void SetPaused(bool paused)
    {
        lock (_sync)
        {
            _paused = paused;
            if (_process is not null) SignalPause(_process, paused);
        }
    }
    public void Pause() => SetPaused(true);
    public void Resume() => SetPaused(false);
    public void Dispose()
    {
        lock (_sync)
        {
            _disposed = true;
            if (_process is not null) Kill(_process);
        }
    }
}
