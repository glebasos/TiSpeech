namespace TiSpeech;

/// <summary>A speech backend that can also render directly to PCM for WAV export.</summary>
public interface ITiPcmRenderer
{
    bool CanRender { get; }
    Task<TiSynthesisResult> RenderAsync(string text, CancellationToken cancellationToken = default);
}

internal interface INativePcmSynthesizer
{
    TiEngineCapabilities Capabilities { get; }
    TiLanguageFlags Languages { get; }
    string? UnavailableReason { get; }
    TiSynthesisResult Render(TiLanguage language, string text, TiVoiceOptions options, TiUserDictionary? dictionary);
}

internal sealed class NativePcmSynthesizer : INativePcmSynthesizer
{
    public TiEngineCapabilities Capabilities => TiSpeechNative.Capabilities;
    public TiLanguageFlags Languages => TiSpeechNative.Languages;
    public string? UnavailableReason => !TiSpeechNative.IsAvailable ? TiSpeechNative.UnavailableReason
        : !Capabilities.HasFlag(TiEngineCapabilities.Synthesis)
            ? "English synthesis data is unavailable (TISPEECH_E_NOTIMPL). Rebuild with TIBASE32.DLL and TIENG32.DLL."
            : null;
    public TiSynthesisResult Render(TiLanguage language, string text, TiVoiceOptions options, TiUserDictionary? dictionary) =>
        TiSpeechNative.SynthesizeText(language, text, options, dictionary);
}

/// <summary>
/// Native English text-to-speech with asynchronous playback. Each utterance
/// snapshots its voice settings; cancelled work can never start or complete a
/// replacement utterance. Phoneme previews and PCM export need no audio device.
/// </summary>
public sealed class NativeTiSpeechBackend : ITiSpeechBackend, ITiPhonemeProvider, ITiPcmRenderer
{
    private readonly Lock _sync = new();
    private readonly SemaphoreSlim _playbackGate = new(1);
    private readonly IPcmPlayer _player;
    private readonly INativePcmSynthesizer _synthesizer;
    private TiVoiceOptions _voice = new();
    private TiLanguage _language = TiLanguage.English;
    private TiSpeakingMode _speakingMode = TiSpeakingMode.Natural;
    private bool _open, _disposed, _paused;
    private CancellationTokenSource? _active;
    private string? _openError;
    private TiUserDictionary? _dictionary;

    public NativeTiSpeechBackend() : this(new SystemPcmPlayer()) { }
    public NativeTiSpeechBackend(IPcmPlayer player) : this(player, new NativePcmSynthesizer()) { }
    internal NativeTiSpeechBackend(IPcmPlayer player, INativePcmSynthesizer synthesizer)
    {
        _player = player;
        _synthesizer = synthesizer;
    }

    public string Name => "native reconstruction";
    public event EventHandler? SpeakStarted;
    public event EventHandler? SpeakCompleted;
    public event EventHandler<string>? Error;
    public TiEngineCapabilities Capabilities => IsOpen ? _synthesizer.Capabilities
        : _synthesizer.Capabilities & ~TiEngineCapabilities.Synthesis;
    public TiLanguageFlags SupportedLanguages => TiSpeechNative.Languages;
    public string? BuildInfo => TiSpeechNative.BuildInfo;
    public bool IsOpen { get { lock (_sync) return _open; } }
    public bool IsSpeaking { get { lock (_sync) return _active is not null; } }
    public bool CanRender => _synthesizer.UnavailableReason is null;
    public string? UnavailableReason => IsOpen ? null
        : _openError ?? _synthesizer.UnavailableReason ?? _player.UnavailableReason;

    bool ITiPhonemeProvider.IsAvailable => TiSpeechNative.IsAvailable
        && TiSpeechNative.Capabilities.HasFlag(TiEngineCapabilities.TextToPhonemes);
    string? ITiPhonemeProvider.UnavailableReason => !TiSpeechNative.IsAvailable
        ? TiSpeechNative.UnavailableReason : TiSpeechNative.Languages == 0
            ? "The native library was built without language data. Rebuild with TIENG32.DLL or TISPAN32.DLL."
            : null;
    string? ITiPhonemeProvider.UnavailableDetail => TiSpeechNative.UnavailableDetail;
    public TiPhonemeResult TextToPhonemes(TiLanguage language, string text)
    {
        TiUserDictionary? dictionary;
        lock (_sync) dictionary = _dictionary;
        return TiSpeechNative.TextToPhonemes(language, text, dictionary);
    }

    /// <summary>The loaded user dictionary, or null.</summary>
    public TiUserDictionary? UserDictionary { get { lock (_sync) return _dictionary; } }

    /// <summary>
    /// SVLoadUserDictionary: one dictionary per engine, replacing any loaded
    /// one. Applies to phoneme previews and speech started afterwards.
    /// </summary>
    /// <exception cref="InvalidDataException">The file is not a valid user dictionary.</exception>
    public void LoadUserDictionary(string path)
    {
        var loaded = TiUserDictionary.Load(path);
        TiUserDictionary? previous;
        lock (_sync)
        {
            if (_disposed)
            {
                loaded.Dispose();
                throw new ObjectDisposedException(nameof(NativeTiSpeechBackend));
            }
            previous = _dictionary;
            _dictionary = loaded;
        }
        previous?.Dispose();
    }

    /// <summary>SVUnloadUserDictionary. A no-op when none is loaded.</summary>
    public void UnloadUserDictionary()
    {
        TiUserDictionary? previous;
        lock (_sync)
        {
            previous = _dictionary;
            _dictionary = null;
        }
        previous?.Dispose();
    }

    public bool Open(TiLanguageFlags languages = TiLanguageFlags.English)
    {
        lock (_sync)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            _openError = !languages.HasFlag(TiLanguageFlags.English)
                ? "Native speech currently supports English only."
                : _synthesizer.UnavailableReason ?? _player.UnavailableReason;
            if (_openError is not null)
            {
                _open = false;
                Error?.Invoke(this, _openError);
                return false;
            }
            _open = true;
            return true;
        }
    }

    public Task<TiSynthesisResult> RenderAsync(string text, CancellationToken cancellationToken = default)
    {
        TiVoiceOptions voice;
        TiLanguage language;
        TiUserDictionary? dictionary;
        lock (_sync)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            voice = _voice;
            language = _language;
            dictionary = _dictionary;
            if (_speakingMode != TiSpeakingMode.Natural)
                return Task.FromResult(TiSynthesisResult.Failure(TiStatus.NotImplemented,
                    "Native speech currently supports the natural speaking mode only."));
        }
        return Task.Run(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            var result = _synthesizer.Render(language, text, voice, dictionary);
            cancellationToken.ThrowIfCancellationRequested();
            return result;
        }, cancellationToken);
    }

    public void Speak(string text, bool interrupt = true)
    {
        lock (_sync)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (!_open)
            {
                Error?.Invoke(this, UnavailableReason ?? "Open the speech backend before speaking.");
                SpeakCompleted?.Invoke(this, EventArgs.Empty);
                return;
            }
            if (_active is not null && !interrupt)
            {
                Error?.Invoke(this, "Speech is already in progress.");
                return;
            }
            _active?.Cancel();
            var request = new CancellationTokenSource();
            _active = request;
            _paused = false;
            // RenderAsync snapshots settings here, before another utterance can change them.
            var render = RenderAsync(text, request.Token);
            _ = RunAsync(request, render);
        }
    }

    private async Task RunAsync(CancellationTokenSource request, Task<TiSynthesisResult> render)
    {
        bool entered = false;
        try
        {
            var result = await render.ConfigureAwait(false);
            request.Token.ThrowIfCancellationRequested();
            if (!result.IsSuccess)
                throw new InvalidOperationException(result.Message ?? result.Status.Describe());
            await _playbackGate.WaitAsync(request.Token).ConfigureAwait(false);
            entered = true;
            request.Token.ThrowIfCancellationRequested();
            lock (_sync)
            {
                _player.Resume();
                if (_paused) _player.Pause();
            }
            await _player.PlayAsync(result.Samples!, result.SampleRate, () =>
            {
                lock (_sync)
                    if (ReferenceEquals(_active, request)) SpeakStarted?.Invoke(this, EventArgs.Empty);
            }, request.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (request.IsCancellationRequested) { }
        catch (Exception ex)
        {
            lock (_sync)
                if (ReferenceEquals(_active, request)) Error?.Invoke(this, ex.Message);
        }
        finally
        {
            if (entered) _playbackGate.Release();
            lock (_sync)
            {
                if (ReferenceEquals(_active, request))
                {
                    _active = null;
                    SpeakCompleted?.Invoke(this, EventArgs.Empty);
                }
                request.Dispose();
            }
        }
    }

    public void Stop()
    {
        lock (_sync)
        {
            var active = _active;
            if (active is null) return;
            _active = null;
            active.Cancel();
            SpeakCompleted?.Invoke(this, EventArgs.Empty);
        }
    }
    public void Pause() { lock (_sync) { if (_active is not null) { _paused = true; _player.Pause(); } } }
    public void Resume() { lock (_sync) { if (_active is not null) { _paused = false; _player.Resume(); } } }
    public void Close() { lock (_sync) { _open = false; Stop(); } }

    public void SetLanguage(TiLanguage value) { lock (_sync) _language = value; }
    public void SetPersonality(TiPersonality value) { lock (_sync) _voice = new(value); }
    public void SetPitch(int value) { lock (_sync) _voice = _voice with { Pitch = value }; }
    public void SetRate(int value) { lock (_sync) _voice = _voice with { Rate = value }; }
    public void SetVoicingMode(TiVoicingMode value) { lock (_sync) _voice = _voice with { Voicing = (int)value }; }
    public void SetF0Style(TiF0Style value) { lock (_sync) _voice = _voice with { F0Style = (int)value }; }
    public void SetSpeakingMode(TiSpeakingMode value) { lock (_sync) _speakingMode = value; }
    public void SetF0Range(int value) { lock (_sync) _voice = _voice with { F0Range = value }; }
    public void SetF0Perturb(int value) { lock (_sync) _voice = _voice with { F0Perturb = value }; }
    public void SetVowelFactor(int value) { lock (_sync) _voice = _voice with { VowelFactor = value }; }
    public void SetGlottalSource(TiGlottalSource value) { lock (_sync) _voice = _voice with { GlottalSource = (int)value }; }
    public void Dispose()
    {
        lock (_sync)
        {
            if (_disposed) return;
            _disposed = true;
            Close();
            _player.Dispose();
            _dictionary?.Dispose();
            _dictionary = null;
        }
    }
}
