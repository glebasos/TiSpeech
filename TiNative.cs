using System.Runtime.InteropServices;

namespace TiSpeech;

/// <summary>
/// Raw P/Invoke declarations for TIBASE32.DLL.
/// All functions use __stdcall (verified via @N decorated export names).
/// The DLL is 32-bit — the host process must be x86.
/// </summary>
internal static class TiNative
{
    private const string Dll = "TIBASE32.dll";

    // ── Lifecycle ────────────────────────────────────────────────────────────

    /// <summary>
    /// Opens a speech instance.
    /// Returns 0 on success; error codes: 0x1b63=no audio device,
    /// 0x1b64=device open fail, 0x1b6e=lang DLL not found, 0x1b69=window fail.
    /// Audio format: PCM 11025 Hz, mono, 8-bit.
    /// </summary>
    /// <param name="phSpeech">OUT: receives the speech context handle.</param>
    /// <param name="hwndOwner">Owner/notification window handle (can be 0 initially; overwritten per SVNarrate call).</param>
    /// <param name="deviceId">waveOut device index; use 0xFFFF_FFFF (WAVE_MAPPER) for default.</param>
    /// <param name="languageFlags">Bitmask: 1=English (TIENG32), 2=Spanish (TISPAN32), 4=German (TIGERM32).</param>
    /// <param name="reserved">Unused 5th parameter; pass 0.</param>
    [DllImport(Dll, EntryPoint = "_SVOpenSpeech@20", CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVOpenSpeech(
        out IntPtr phSpeech,
        IntPtr     hwndOwner,
        uint       deviceId,
        uint       languageFlags,
        uint       reserved);

    /// <summary>Destroys the speech context and frees all resources.</summary>
    [DllImport(Dll, EntryPoint = "_SVCloseSpeech@4", CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVCloseSpeech(IntPtr hSpeech);

    /// <summary>Immediately stops speech in progress.</summary>
    [DllImport(Dll, EntryPoint = "_SVAbort@4", CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVAbort(IntPtr hSpeech);

    /// <summary>Pauses speech.</summary>
    [DllImport(Dll, EntryPoint = "_SVPause@4", CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVPause(IntPtr hSpeech);

    /// <summary>Resumes paused speech.</summary>
    [DllImport(Dll, EntryPoint = "_SVResume@4", CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVResume(IntPtr hSpeech);

    // ── Speech ───────────────────────────────────────────────────────────────

    /// <summary>
    /// Speak a pre-converted phoneme string (or raw text if called directly).
    /// The engine posts <c>SVSyncMessages</c> to <paramref name="hwndNotify"/> on completion.
    /// </summary>
    /// <param name="hSpeech">Speech context from SVOpenSpeech.</param>
    /// <param name="text">Null-terminated text/phoneme string.</param>
    /// <param name="hwndNotify">Window to receive completion PostMessage.</param>
    /// <param name="flags">
    /// 0x20 = interrupt current speech;
    /// 0x40000000 = input is pre-phonemised;
    /// combined 0xA0000000 set internally for async.
    /// </param>
    /// <param name="reserved">5th param — pass 0.</param>
    [DllImport(Dll, EntryPoint = "_SVNarrate@20", CallingConvention = CallingConvention.StdCall,
               CharSet = CharSet.Ansi)]
    public static extern uint SVNarrate(
        IntPtr hSpeech,
        string text,
        IntPtr hwndNotify,
        uint   flags,
        uint   reserved);

    /// <summary>
    /// High-level speak: converts text to phonemes then speaks.
    /// Pipeline: text → SVTextToPhon (with retry) → phoneme string → SVNarrate.
    /// </summary>
    /// <param name="hSpeech">Speech context.</param>
    /// <param name="text">Input text (null-terminated).</param>
    /// <param name="phonemeBufferOut">
    /// If <paramref name="flags"/> has bit 0x80 set: receives pointer to allocated
    /// phoneme string (caller must free). Otherwise ignored; pass IntPtr.Zero.
    /// </param>
    /// <param name="phonemeLengthOut">Length of phoneme buffer; ignored when bit 0x80 clear.</param>
    /// <param name="hwndNotify">Window for completion PostMessage.</param>
    /// <param name="flags">0x80 = also return phoneme buffer; 0x4 = larger initial buffer.</param>
    /// <param name="narrateFlags">Flags forwarded to SVNarrate (0x20 = interrupt).</param>
    /// <param name="reserved">8th param — pass 0.</param>
    [DllImport(Dll, EntryPoint = "_SVTTS@32", CallingConvention = CallingConvention.StdCall,
               CharSet = CharSet.Ansi)]
    public static extern uint SVTTS(
        IntPtr     hSpeech,
        string     text,
        ref IntPtr phonemeBufferOut,
        ref int    phonemeLengthOut,
        IntPtr     hwndNotify,
        uint       flags,
        uint       narrateFlags,
        uint       reserved);

    // ── Voice parameters (all: hSpeech + value, return 0=ok / error) ─────────

    /// <summary>
    /// Select a voice personality (0–19).
    /// Indices 1,5,13,15 use female formant tables.
    /// Error 0x1b62 = index out of range; 0x1b61 = engine busy.
    /// </summary>
    [DllImport(Dll, EntryPoint = "_SVSetPersonality@8", CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVSetPersonality(IntPtr hSpeech, ushort personalityIndex);

    [DllImport(Dll, EntryPoint = "_SVSetPitch@8",        CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVSetPitch(IntPtr hSpeech, int value);

    [DllImport(Dll, EntryPoint = "_SVSetRate@8",         CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVSetRate(IntPtr hSpeech, int value);

    [DllImport(Dll, EntryPoint = "_SVSetGender@8",       CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVSetGender(IntPtr hSpeech, int value);

    [DllImport(Dll, EntryPoint = "_SVSetLanguage@8",     CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVSetLanguage(IntPtr hSpeech, int languageId);

    [DllImport(Dll, EntryPoint = "_SVSetF0Range@8",      CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVSetF0Range(IntPtr hSpeech, int value);

    [DllImport(Dll, EntryPoint = "_SVSetF0Style@8",      CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVSetF0Style(IntPtr hSpeech, int value);

    [DllImport(Dll, EntryPoint = "_SVSetF0Perturb@8",    CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVSetF0Perturb(IntPtr hSpeech, int value);

    [DllImport(Dll, EntryPoint = "_SVSetGlottalSource@8",CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVSetGlottalSource(IntPtr hSpeech, int value);

    [DllImport(Dll, EntryPoint = "_SVSetSpeakingMode@8", CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVSetSpeakingMode(IntPtr hSpeech, int value);

    [DllImport(Dll, EntryPoint = "_SVSetVoicingMode@8",  CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVSetVoicingMode(IntPtr hSpeech, int value);

    [DllImport(Dll, EntryPoint = "_SVSetVowelFactor@8",  CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVSetVowelFactor(IntPtr hSpeech, int value);

    [DllImport(Dll, EntryPoint = "_SVSetAFBias@8",       CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVSetAFBias(IntPtr hSpeech, int value);

    [DllImport(Dll, EntryPoint = "_SVSetAHBias@8",       CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVSetAHBias(IntPtr hSpeech, int value);

    [DllImport(Dll, EntryPoint = "_SVSetAVBias@8",       CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVSetAVBias(IntPtr hSpeech, int value);

    // ── Info / dictionary ────────────────────────────────────────────────────

    [DllImport(Dll, EntryPoint = "_SVGetVersionInfo@4",        CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVGetVersionInfo(IntPtr hSpeech);

    [DllImport(Dll, EntryPoint = "_SVGetAvailableLanguages@8", CallingConvention = CallingConvention.StdCall)]
    public static extern uint SVGetAvailableLanguages(IntPtr hSpeech, out uint langFlags);

    [DllImport(Dll, EntryPoint = "_SVGetErrorText@12",         CallingConvention = CallingConvention.StdCall,
               CharSet = CharSet.Ansi)]
    public static extern uint SVGetErrorText(uint errorCode, IntPtr buffer, int bufferSize);

    [DllImport(Dll, EntryPoint = "_SVLoadUserDictionary@8",    CallingConvention = CallingConvention.StdCall,
               CharSet = CharSet.Ansi)]
    public static extern uint SVLoadUserDictionary(IntPtr hSpeech, string path);

    [DllImport(Dll, EntryPoint = "_SVUnloadUserDictionary@8",  CallingConvention = CallingConvention.StdCall,
               CharSet = CharSet.Ansi)]
    public static extern uint SVUnloadUserDictionary(IntPtr hSpeech, string path);

    // ── Win32 helpers ────────────────────────────────────────────────────────

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
    public static extern uint RegisterWindowMessageA(string lpString);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool SetDllDirectoryW(string lpPathName);

    // ── Win32 message-window support ─────────────────────────────────────────

    public delegate IntPtr WndProcDelegate(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct WNDCLASSEX
    {
        public uint    cbSize;
        public uint    style;
        public IntPtr  lpfnWndProc;
        public int     cbClsExtra;
        public int     cbWndExtra;
        public IntPtr  hInstance;
        public IntPtr  hIcon;
        public IntPtr  hCursor;
        public IntPtr  hbrBackground;
        public string? lpszMenuName;
        public string  lpszClassName;
        public IntPtr  hIconSm;
    }

    /// <summary>Special parent handle that creates a message-only window (no screen presence).</summary>
    public static readonly IntPtr HWND_MESSAGE = new(-3);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern ushort RegisterClassExW(ref WNDCLASSEX lpwcx);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateWindowExW(
        uint   dwExStyle,
        string lpClassName,
        string lpWindowName,
        uint   dwStyle,
        int X, int Y, int nWidth, int nHeight,
        IntPtr hWndParent,
        IntPtr hMenu,
        IntPtr hInstance,
        IntPtr lpParam);

    [DllImport("user32.dll")]
    public static extern IntPtr DefWindowProcW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool DestroyWindow(IntPtr hWnd);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr GetModuleHandleW(string? lpModuleName);
}
