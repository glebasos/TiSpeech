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

    /// <summary>TISPEECH_E_DICTSHORT — the user dictionary file is truncated.</summary>
    DictionaryTruncated = 0x1b6b,

    /// <summary>TISPEECH_E_DICTFORMAT — not a SoftVoice ("SVXF") user dictionary.</summary>
    DictionaryFormat = 0x1b6c,

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
        TiStatus.DictionaryTruncated   => "The user dictionary file is truncated (TISPEECH_E_DICTSHORT).",
        TiStatus.DictionaryFormat      => "Not a SoftVoice user dictionary (TISPEECH_E_DICTFORMAT).",
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
/// Outcome of a synthesis attempt. On failure <see cref="Samples"/> is null
/// rather than an empty-but-plausible buffer, so a caller that ignores
/// <see cref="Status"/> cannot mistake it for silence it may play.
/// </summary>
public sealed record TiSynthesisResult(TiStatus Status, byte[]? Samples, int SampleRate, string? Message = null)
{
    public bool IsSuccess => Status == TiStatus.Ok && Samples is not null;

    /// <summary>What the engine reported while rendering <see cref="Samples"/>, in order.</summary>
    public IReadOnlyList<TiSpeechEvent> Events { get; init; } = [];

    public static TiSynthesisResult Failure(TiStatus status, string? message = null) =>
        new(status, null, 0, message ?? status.Describe());
}

/// <summary>
/// What the original engine's renderer reports while it speaks (TIBASE32
/// 0x1c004543..0x1c004605). The values are the window-message codes the
/// Windows engine posts.
/// </summary>
public enum TiSpeechEventKind : ushort
{
    /// <summary>A word starts: <see cref="TiSpeechEvent.Value"/> is its index in the
    /// spoken text (a UTF-16 index, since only Latin-1 is spoken), or the n of an
    /// inline <c>{wordsync n}</c>.</summary>
    Word = 0x3EB,
    /// <summary>The first phoneme after a pause.</summary>
    Sentence = 0x3EC,
    /// <summary>A syllable starts.</summary>
    Syllable = 0x3ED,
    /// <summary>A phoneme starts: the value is its phoneme code.</summary>
    Phoneme = 0x3EE,
    /// <summary>An inline <c>{usync n}</c>: the value is n &amp; 0xFF.</summary>
    UserSync = 0x3EF,
    /// <summary>The mouth shape changed: the value is the new shape, 1 (closed:
    /// M, B, silence) to 10 (F, V); 3/4 rounded, 5 open, 7/8 spread, 9 tongue.</summary>
    Mouth = 0x3F0,
}

/// <summary>Which events synthesis reports. Word and user-sync events need no
/// flag of their own beyond <see cref="Words"/>.</summary>
[Flags]
public enum TiSpeechEventMask : uint
{
    None = 0,
    Sentence = 0x1,
    Syllable = 0x2,
    Phoneme = 0x4,
    Mouth = 0x8,
    /// <summary>Word events for converted text (SVTextToPhon's word marks). The
    /// original couples this to <see cref="Mouth"/> through SVTTS's single flags
    /// argument; the audio is identical either way.</summary>
    Words = 0x100,
}

/// <summary>One engine event, positioned by the PCM sample it belongs to.</summary>
/// <param name="Sample">Index in <see cref="TiSynthesisResult.Samples"/> where it takes effect.</param>
/// <param name="TimeMs">The original's own timestamp: milliseconds since its sentence began.</param>
public readonly record struct TiSpeechEvent(TiSpeechEventKind Kind, int Value, int Sample, int TimeMs);
