using System.Runtime.InteropServices;

namespace TiSpeech;

/// <summary>
/// Managed wrapper around TIBASE32.DLL.
/// </summary>
public sealed class TiSpeechEngine : IDisposable
{
    private IntPtr _handle;
    private bool   _disposed;
    private readonly SpeechNotifyWindow _notifyWindow;

    public bool IsOpen     => _handle != IntPtr.Zero;
    public bool IsSpeaking { get; private set; }

    public event EventHandler?         SpeakStarted;
    public event EventHandler?         SpeakCompleted;
    public event EventHandler<string>? Error;

    public TiSpeechEngine()
    {
        _notifyWindow = new SpeechNotifyWindow();
        _notifyWindow.SpeechCompleted += OnNativeCompleted;
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    public bool Open(string dllDirectory, TiLanguageFlags languages = TiLanguageFlags.English)
    {
        if (_handle != IntPtr.Zero) return true;

        TiNative.SetDllDirectoryW(dllDirectory);

        uint err = TiNative.SVOpenSpeech(
            out _handle,
            _notifyWindow.Handle,
            0xFFFFFFFF,         // WAVE_MAPPER = default audio device
            (uint)languages,
            0);

        if (err != 0 || _handle == IntPtr.Zero)
        {
            _handle = IntPtr.Zero;
            Error?.Invoke(this, FormatError(err));
            return false;
        }
        return true;
    }

    public void Close()
    {
        if (_handle == IntPtr.Zero) return;
        TiNative.SVAbort(_handle);
        TiNative.SVCloseSpeech(_handle);
        _handle = IntPtr.Zero;
    }

    // ── Speech ────────────────────────────────────────────────────────────────

    public void Speak(string text, bool interrupt = true)
    {
        if (!IsOpen) return;
        IntPtr phonBuf = IntPtr.Zero;
        int    phonLen = 0;
        uint   flags   = interrupt ? 0x20u : 0u;

        uint err = TiNative.SVTTS(
            _handle, text,
            ref phonBuf, ref phonLen,
            _notifyWindow.Handle,
            0, flags, 0);

        if (err != 0) { Error?.Invoke(this, FormatError(err)); return; }

        IsSpeaking = true;
        SpeakStarted?.Invoke(this, EventArgs.Empty);
    }

    public void Stop()
    {
        if (_handle == IntPtr.Zero) return;
        TiNative.SVAbort(_handle);
        IsSpeaking = false;
    }

    public void Pause()  { EnsureOpen(); TiNative.SVPause(_handle);  }
    public void Resume() { EnsureOpen(); TiNative.SVResume(_handle); }

    // ── Voice parameters ──────────────────────────────────────────────────────

    /// <summary>Select voice personality (0–19). See <see cref="TiPersonality"/>.</summary>
    public void SetPersonality(TiPersonality p)
        { if (!IsOpen) return; TiNative.SVSetPersonality(_handle, (ushort)p); }

    /// <summary>Set active language. Must be loaded in Open().</summary>
    public void SetLanguage(TiLanguage lang)
        { if (!IsOpen) return; TiNative.SVSetLanguage(_handle, (int)lang); }

    /// <summary>Pitch percentage. 100 = default. Typical range 50–200.</summary>
    public void SetPitch(int value)
        { if (!IsOpen) return; TiNative.SVSetPitch(_handle, value); }

    /// <summary>Speaking rate percentage. 150 = default. Typical range 50–300.</summary>
    public void SetRate(int value)
        { if (!IsOpen) return; TiNative.SVSetRate(_handle, value); }

    /// <summary>Vocal effort / breathiness. See <see cref="TiGlottalSource"/>.</summary>
    public void SetGlottalSource(TiGlottalSource source)
        { if (!IsOpen) return; TiNative.SVSetGlottalSource(_handle, (int)source); }

    /// <summary>Fundamental frequency contour style. See <see cref="TiF0Style"/>.</summary>
    public void SetF0Style(TiF0Style style)
        { if (!IsOpen) return; TiNative.SVSetF0Style(_handle, (int)style); }

    /// <summary>Speaking mode (normal, spell, etc.). See <see cref="TiSpeakingMode"/>.</summary>
    public void SetSpeakingMode(TiSpeakingMode mode)
        { if (!IsOpen) return; TiNative.SVSetSpeakingMode(_handle, (int)mode); }

    /// <summary>Vocal effort / voicing quality. See <see cref="TiVoicingMode"/>.</summary>
    public void SetVoicingMode(TiVoicingMode mode)
        { if (!IsOpen) return; TiNative.SVSetVoicingMode(_handle, (int)mode); }

    public void SetF0Range(int value)   { if (!IsOpen) return; TiNative.SVSetF0Range(_handle, value); }
    public void SetF0Perturb(int value) { if (!IsOpen) return; TiNative.SVSetF0Perturb(_handle, value); }
    public void SetVowelFactor(int v)   { if (!IsOpen) return; TiNative.SVSetVowelFactor(_handle, v); }

    // ── Error helpers ─────────────────────────────────────────────────────────

    public static string FormatError(uint code) => code switch
    {
        0       => "OK",
        0x1b5a  => "Out of memory",
        0x1b61  => "Engine busy",
        0x1b62  => "Invalid personality index (must be 0–19)",
        0x1b63  => "No audio output device found",
        0x1b64  => "Cannot open audio device",
        0x1b66  => "Already speaking — use interrupt flag",
        0x1b69  => "Internal window creation failed",
        0x1b6e  => "Language DLL not found (TIENG32.DLL / TISPAN32.DLL)",
        0x1b70  => "Null text pointer",
        _       => $"SoftVoice error 0x{code:X4}",
    };

    private void EnsureOpen()
    {
        if (_handle == IntPtr.Zero)
            throw new InvalidOperationException("Call Open() first.");
    }

    private void OnNativeCompleted(object? sender, EventArgs e)
    {
        IsSpeaking = false;
        SpeakCompleted?.Invoke(this, EventArgs.Empty);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        Close();
        _notifyWindow.Dispose();
    }

    // ── Hidden notification window ────────────────────────────────────────────

    private sealed class SpeechNotifyWindow : IDisposable
    {
        private static readonly uint _msgId =
            TiNative.RegisterWindowMessageA("SVSyncMessages");

        // Class name must be unique per process to avoid conflicts on repeated init.
        private static readonly string _className =
            $"TiSpeechNotify_{Environment.ProcessId}";

        private static bool _classRegistered;

        public event EventHandler? SpeechCompleted;
        public IntPtr Handle { get; private set; }

        // Keep the delegate alive for the lifetime of this instance so GC won't collect it.
        private readonly TiNative.WndProcDelegate _wndProcDelegate;
        private readonly GCHandle _delegatePin;

        public SpeechNotifyWindow()
        {
            _wndProcDelegate = WndProc;
            _delegatePin = GCHandle.Alloc(_wndProcDelegate);

            IntPtr hInstance = TiNative.GetModuleHandleW(null);

            if (!_classRegistered)
            {
                var wc = new TiNative.WNDCLASSEX
                {
                    cbSize       = (uint)Marshal.SizeOf<TiNative.WNDCLASSEX>(),
                    lpfnWndProc  = Marshal.GetFunctionPointerForDelegate(_wndProcDelegate),
                    hInstance    = hInstance,
                    lpszClassName = _className,
                };
                TiNative.RegisterClassExW(ref wc);
                _classRegistered = true;
            }

            Handle = TiNative.CreateWindowExW(
                0, _className, "TiSpeechNotify",
                0, 0, 0, 0, 0,
                TiNative.HWND_MESSAGE,
                IntPtr.Zero, hInstance, IntPtr.Zero);
        }

        private IntPtr WndProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam)
        {
            if (_msgId != 0 && msg == _msgId)
            {
                if (wParam == (IntPtr)0x3E9)  // 1001 = speech ended
                    SpeechCompleted?.Invoke(this, EventArgs.Empty);
                return IntPtr.Zero;
            }
            return TiNative.DefWindowProcW(hWnd, msg, wParam, lParam);
        }

        public void Dispose()
        {
            if (Handle != IntPtr.Zero)
            {
                TiNative.DestroyWindow(Handle);
                Handle = IntPtr.Zero;
            }
            if (_delegatePin.IsAllocated)
                _delegatePin.Free();
        }
    }
}
