namespace TiSpeech;

/// <summary>
/// Capability bits reported by the native reconstruction's
/// <c>tispeech_capabilities()</c> (see <c>TiSpeech/native/include/tispeech/capi.h</c>,
/// <c>TISPEECH_CAP_*</c>). Values mirror the C header exactly.
///
/// This is the flag callers are expected to gate on instead of asking which
/// operating system they are running on: a build with no language data cannot
/// convert text, and no build can synthesise audio yet. The native side never
/// sets a bit it has not earned, and neither does <see cref="TiSpeechClient"/>-style
/// managed backends that implement <see cref="ITiSpeechBackend"/>.
/// </summary>
[Flags]
public enum TiEngineCapabilities : uint
{
    /// <summary>The backend can do nothing useful right now.</summary>
    None = 0,

    /// <summary>
    /// TISPEECH_CAP_TEXT_TO_PHONEMES — letter-to-sound rules are linked in and
    /// at least one language's rule data is present.
    /// </summary>
    TextToPhonemes = 0x1,

    /// <summary>
    /// TISPEECH_CAP_SYNTHESIS — phonemes to PCM works end to end. The native
    /// reconstruction deliberately never sets this today; the phoneme-to-frame
    /// stage is not reconstructed.
    /// </summary>
    Synthesis = 0x2,
}

/// <summary>
/// Status codes shared by the SoftVoice engine and the native reconstruction's
/// C ABI. The SoftVoice-derived values keep their original numbers so a single
/// managed enum covers both backends (see <c>TISPEECH_*</c> in capi.h).
/// </summary>
public enum TiStatus
{
    Ok = 0,

    /// <summary>TISPEECH_E_OUTOFMEMORY — native allocation failed.</summary>
    OutOfMemory = 0x1b5a,

    /// <summary>TISPEECH_E_BADPARAM — an argument was out of range or malformed.</summary>
    BadParam = 0x1b62,

    /// <summary>TISPEECH_E_NOLANGUAGE — this build has no rule data for the requested language.</summary>
    NoLanguage = 0x1b6e,

    /// <summary>TISPEECH_E_NULLTEXT — null text pointer.</summary>
    NullText = 0x1b70,

    /// <summary>
    /// TISPEECH_E_NOTIMPL — the requested pipeline stage is not reconstructed.
    /// This is a real, expected answer, not an error to paper over.
    /// </summary>
    NotImplemented = 0xF001,

    /// <summary>TISPEECH_E_BUFFERFULL — the output buffer was too small.</summary>
    BufferFull = 0xF002,

    /// <summary>
    /// Not a native status code. Reported by the managed wrapper when the
    /// native library could not be loaded at all, so callers can tell
    /// "not built" apart from "built but cannot do this".
    /// </summary>
    LibraryUnavailable = -1,

    /// <summary>
    /// Not a native status code. Reported by the managed wrapper when input
    /// text falls outside the 8-bit range the rule tables are written for.
    /// </summary>
    UnsupportedCharacters = -2,
}

/// <summary>Human-readable text for a <see cref="TiStatus"/>.</summary>
public static class TiStatusExtensions
{
    public static string Describe(this TiStatus status) => status switch
    {
        TiStatus.Ok                    => "OK",
        TiStatus.OutOfMemory           => "Native allocation failed (TISPEECH_E_OUTOFMEMORY).",
        TiStatus.BadParam              => "Invalid argument (TISPEECH_E_BADPARAM).",
        TiStatus.NoLanguage            => "No letter-to-sound data for that language in this build (TISPEECH_E_NOLANGUAGE).",
        TiStatus.NullText              => "Null text (TISPEECH_E_NULLTEXT).",
        TiStatus.NotImplemented        => "Not implemented in the native reconstruction yet (TISPEECH_E_NOTIMPL).",
        TiStatus.BufferFull            => "Output buffer too small (TISPEECH_E_BUFFERFULL).",
        TiStatus.LibraryUnavailable    => "The TiSpeech native library is not loaded.",
        TiStatus.UnsupportedCharacters => "The letter-to-sound tables are 8-bit; the text contains characters outside Latin-1.",
        _                              => $"Unknown status 0x{(int)status:X4}.",
    };
}

/// <summary>
/// Outcome of a letter-to-sound conversion. <see cref="Phonemes"/> is only
/// meaningful when <see cref="IsSuccess"/> is true — a failed conversion
/// carries an empty string, never a partial or invented result.
/// </summary>
public sealed record TiPhonemeResult(TiStatus Status, string Phonemes, string? Message = null)
{
    public bool IsSuccess => Status == TiStatus.Ok;

    public static TiPhonemeResult Success(string phonemes) => new(TiStatus.Ok, phonemes);

    public static TiPhonemeResult Failure(TiStatus status, string? message = null) =>
        new(status, string.Empty, message ?? status.Describe());
}

/// <summary>
/// Outcome of a synthesis attempt. Today this is always
/// <see cref="TiStatus.NotImplemented"/> from the native reconstruction, and
/// <see cref="Samples"/> is null rather than an empty-but-plausible buffer, so
/// a caller that ignores <see cref="Status"/> cannot mistake it for silence it
/// may play.
/// </summary>
public sealed record TiSynthesisResult(TiStatus Status, byte[]? Samples, int SampleRate)
{
    public bool IsSuccess => Status == TiStatus.Ok && Samples is not null;

    public static TiSynthesisResult Failure(TiStatus status) => new(status, null, 0);
}
