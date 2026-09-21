namespace TiSpeech;

/// <summary>
/// Letter-to-sound conversion, independent of whether anything can speak.
///
/// It is deliberately separate from <see cref="ITiSpeechBackend"/>: the native
/// reconstruction converts text to phonemes on every platform today while
/// synthesising nothing at all, and the Windows pipe/host backend is the
/// other way round. A caller that wants phonemes should ask a provider, not a
/// speech engine.
/// </summary>
public interface ITiPhonemeProvider
{
    /// <summary>Short name of the implementation, for messages and logs.</summary>
    string Name { get; }

    /// <summary>
    /// True when this provider can convert text for at least one language right
    /// now. Gate the UI on this rather than on the operating system.
    /// </summary>
    bool IsAvailable { get; }

    /// <summary>Languages this provider actually has rule data for.</summary>
    TiLanguageFlags SupportedLanguages { get; }

    /// <summary>
    /// Why conversion is unavailable, or null when it is available. Missing
    /// library and missing language data are distinct, expected states and get
    /// distinct messages.
    /// </summary>
    string? UnavailableReason { get; }

    /// <summary>
    /// Extra diagnostic detail behind <see cref="UnavailableReason"/> — for the
    /// native provider, every path it searched for the library. Belongs in a
    /// tooltip or a log rather than on screen. Null when available.
    /// </summary>
    string? UnavailableDetail { get; }

    /// <summary>The backend's own description of itself, or null.</summary>
    string? BuildInfo { get; }

    /// <summary>
    /// Convert <paramref name="text"/> to a phoneme string. Never throws for a
    /// missing library or missing language data — it reports a
    /// <see cref="TiStatus"/> instead.
    /// </summary>
    TiPhonemeResult TextToPhonemes(TiLanguage language, string text);
}

/// <summary>
/// The speech-engine contract shared by every backend OpenTalkIt can talk to:
/// the Windows out-of-process SoftVoice host (<c>TiSpeechClient</c>) and the
/// portable native reconstruction (<see cref="NativeTiSpeechBackend"/>).
///
/// The member set is the one <c>TiSpeechClient</c> and <see cref="TiSpeechEngine"/>
/// already had; this interface names it so the UI can hold either backend
/// without knowing which. The only additions are the three diagnostic members
/// at the top, which exist so callers can gate on what a backend can really do
/// instead of on <c>OperatingSystem.IsWindows()</c>.
///
/// HONESTY CONTRACT: an implementation must not set
/// <see cref="TiEngineCapabilities.Synthesis"/>, return true from
/// <see cref="Open"/>, or raise <c>SpeakStarted</c> unless it genuinely
/// produces the original engine's audio. Reporting less than you can do is
/// harmless; reporting more is a bug.
/// </summary>
public interface ITiSpeechBackend : IDisposable
{
    /// <summary>Short name of the backend, for messages and logs.</summary>
    string Name { get; }

    /// <summary>What this backend can really do right now.</summary>
    TiEngineCapabilities Capabilities { get; }

    /// <summary>
    /// Why the backend cannot speak, or null when it can. Implementations
    /// should report the concrete cause (missing host process, unimplemented
    /// stage, missing language data) rather than a platform guess.
    /// </summary>
    string? UnavailableReason { get; }

    bool IsOpen { get; }
    bool IsSpeaking { get; }

    event EventHandler? SpeakStarted;
    event EventHandler? SpeakCompleted;
    event EventHandler<string>? Error;

    /// <summary>
    /// Bring the engine up. Returns true only when speech will actually be
    /// produced by subsequent <see cref="Speak"/> calls.
    /// </summary>
    bool Open(TiLanguageFlags languages = TiLanguageFlags.English);

    void Close();

    void Speak(string text, bool interrupt = true);
    void Stop();
    void Pause();
    void Resume();

    void SetPersonality(TiPersonality personality);
    void SetLanguage(TiLanguage language);
    void SetPitch(int value);
    void SetRate(int value);
    void SetVoicingMode(TiVoicingMode mode);
    void SetF0Style(TiF0Style style);
    void SetSpeakingMode(TiSpeakingMode mode);
    void SetF0Range(int value);
    void SetF0Perturb(int value);
    void SetVowelFactor(int value);
    void SetGlottalSource(TiGlottalSource source);
}
