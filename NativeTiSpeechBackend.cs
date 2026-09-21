namespace TiSpeech;

/// <summary>
/// <see cref="ITiSpeechBackend"/> / <see cref="ITiPhonemeProvider"/> over the
/// from-scratch native reconstruction (<see cref="TiSpeechNative"/>).
///
/// What it really does today, and nothing more:
///   * letter-to-sound conversion, natively, on macOS/Linux/Windows alike;
///   * reports <see cref="TiEngineCapabilities"/> straight from
///     <c>tispeech_capabilities()</c>.
///
/// What it deliberately does NOT do: speak. <see cref="Open"/> returns false and
/// <see cref="Speak"/> reports the native <c>TISPEECH_E_NOTIMPL</c> through
/// <see cref="Error"/>. There is no fallback to a system voice, no silence
/// passed off as output, and no Wine/emulation shim. When the phoneme-to-frame
/// stage lands in the native library, whoever wires up playback changes this
/// class — until then it must keep saying no.
/// </summary>
public sealed class NativeTiSpeechBackend : ITiSpeechBackend, ITiPhonemeProvider
{
    private TiLanguage _language = TiLanguage.English;
    private bool _disposed;

    public string Name => "native reconstruction";

    // CS0067: SpeakStarted is intentionally never raised. It is part of
    // ITiSpeechBackend and this backend never starts speaking, so the absence of
    // an invocation is the point, not an oversight.
#pragma warning disable CS0067
    public event EventHandler? SpeakStarted;
#pragma warning restore CS0067
    public event EventHandler? SpeakCompleted;
    public event EventHandler<string>? Error;

    public TiEngineCapabilities Capabilities => TiSpeechNative.Capabilities;

    public TiLanguageFlags SupportedLanguages => TiSpeechNative.Languages;

    public string? BuildInfo => TiSpeechNative.BuildInfo;

    /// <summary>
    /// Always false: this backend has no playback path. See
    /// <see cref="UnavailableReason"/> for the specific reason, which
    /// distinguishes "library not built" from "built, but synthesis is not
    /// reconstructed".
    /// </summary>
    public bool IsOpen => false;

    /// <summary>Always false: nothing here ever starts speaking.</summary>
    public bool IsSpeaking => false;

    // ── ITiSpeechBackend: diagnostics ─────────────────────────────────────────

    public string? UnavailableReason
    {
        get
        {
            if (!TiSpeechNative.IsAvailable)
                return TiSpeechNative.UnavailableReason;

            if (Capabilities.HasFlag(TiEngineCapabilities.Synthesis))
            {
                // Not reachable with today's native library, which never sets
                // this bit. Kept as a real branch so that if a future build does
                // set it, this class says "not wired up yet" instead of silently
                // implying it works.
                return "The native library reports synthesis support, but this backend has no audio " +
                       "playback path wired up yet.";
            }

            var phonemeStatus = Capabilities.HasFlag(TiEngineCapabilities.TextToPhonemes)
                                && SupportedLanguages != 0
                ? "Letter-to-sound conversion does work — see the Phonemes button."
                : "This build also has no letter-to-sound language data.";
            return "Speech synthesis is not implemented in the native engine reconstruction yet: " +
                   "tispeech_synthesize() returns TISPEECH_E_NOTIMPL, so Talk and WAV export stay disabled. " +
                   phonemeStatus;
        }
    }

    // ── ITiPhonemeProvider ────────────────────────────────────────────────────

    /// <summary>
    /// True only when the native library loaded, reports
    /// <see cref="TiEngineCapabilities.TextToPhonemes"/>, and actually has rule
    /// data for at least one language.
    /// </summary>
    bool ITiPhonemeProvider.IsAvailable =>
        TiSpeechNative.IsAvailable
        && Capabilities.HasFlag(TiEngineCapabilities.TextToPhonemes)
        && SupportedLanguages != 0;

    string? ITiPhonemeProvider.UnavailableReason
    {
        get
        {
            if (!TiSpeechNative.IsAvailable)
                return TiSpeechNative.UnavailableReason;

            if (SupportedLanguages == 0 || !Capabilities.HasFlag(TiEngineCapabilities.TextToPhonemes))
                return "The TiSpeech native library is loaded but was built without language data, so it " +
                       "cannot convert text to phonemes. The rule tables belong to SoftVoice and are not in " +
                       "this repository: configure the native build with " +
                       "-DTISPEECH_ENG_DLL=<path to your own TIENG32.DLL> and rebuild.";

            return null;
        }
    }

    string? ITiPhonemeProvider.UnavailableDetail =>
        TiSpeechNative.IsAvailable ? null : TiSpeechNative.UnavailableDetail;

    public TiPhonemeResult TextToPhonemes(TiLanguage language, string text) =>
        TiSpeechNative.TextToPhonemes(language, text);

    // ── ITiSpeechBackend: lifecycle ───────────────────────────────────────────

    /// <summary>
    /// Always returns false and raises <see cref="Error"/> with
    /// <see cref="UnavailableReason"/>. Returning true here would be the single
    /// most damaging lie this project could tell, because every Talk/Export gate
    /// in the UI keys off it.
    /// </summary>
    public bool Open(TiLanguageFlags languages = TiLanguageFlags.English)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        Error?.Invoke(this, UnavailableReason ?? "The native engine reconstruction cannot synthesise speech yet.");
        return false;
    }

    public void Close() { }

    /// <summary>
    /// Calls the real <c>tispeech_synthesize()</c> so the reported status is the
    /// engine's own answer rather than a hardcoded string, reports it through
    /// <see cref="Error"/>, and produces no audio.
    ///
    /// <see cref="SpeakCompleted"/> is raised immediately afterwards purely so a
    /// caller awaiting it is released rather than deadlocking; <see cref="Error"/>
    /// has already fired and <see cref="SpeakStarted"/> never does, so nothing
    /// can read this as speech having happened.
    /// </summary>
    public void Speak(string text, bool interrupt = true)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        var result = TiSpeechNative.Synthesize(_language, text ?? string.Empty);
        Error?.Invoke(this,
            $"The native engine reconstruction produced no audio: {result.Status.Describe()} " +
            "No substitute or system voice is used by design.");
        SpeakCompleted?.Invoke(this, EventArgs.Empty);
    }

    public void Stop() { }
    public void Pause() { }
    public void Resume() { }

    // ── ITiSpeechBackend: voice parameters ────────────────────────────────────
    //
    // Accepted and discarded. There is no synthesis state to apply them to, and
    // storing them would only create the impression that they took effect.
    // Speak() reports TISPEECH_E_NOTIMPL regardless of what was set here, so no
    // caller can mistake a "set" parameter for an applied one. SetLanguage is
    // the exception: it selects the rule table used by TextToPhonemes.

    public void SetLanguage(TiLanguage language) => _language = language;

    public void SetPersonality(TiPersonality personality) { }
    public void SetPitch(int value) { }
    public void SetRate(int value) { }
    public void SetVoicingMode(TiVoicingMode mode) { }
    public void SetF0Style(TiF0Style style) { }
    public void SetSpeakingMode(TiSpeakingMode mode) { }
    public void SetF0Range(int value) { }
    public void SetF0Perturb(int value) { }
    public void SetVowelFactor(int value) { }
    public void SetGlottalSource(TiGlottalSource source) { }

    public void Dispose()
    {
        _disposed = true;
        // The native library holds no per-instance state: capi.c's entry points
        // are pure functions over static rule tables, so there is nothing to
        // release and no handle to close.
    }
}
