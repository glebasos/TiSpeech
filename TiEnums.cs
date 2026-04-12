using System.ComponentModel;

namespace TiSpeech;

/// <summary>
/// Personality indices confirmed from TIBASE32.DLL pointer table at 0x1c012830.
/// Indices 1,5,13,15 use female formant tables inside the engine.
/// Index 0 (Male) and 16 (Nerd) confirmed as first bytes 'm' and 'n' respectively.
/// </summary>
public enum TiPersonality : ushort
{
    [Description("Man")]           Male         = 0,
    [Description("Woman")]         Female       = 1,
    [Description("Hyper Female")]  LargeMale    = 2,
    [Description("Child")]         Child        = 3,
    [Description("Strong Man")]    GiantMale    = 4,
    [Description("Mellow")]        MellowFemale = 5,
    [Description("Singing Girl")]  MellowMale   = 6,
    [Description("Strong Woman")]  CrispMale    = 7,
    [Description("Fly")]           TheFly       = 8,
    [Description("Little Robot")]  Robotoid     = 9,
    [Description("Martian")]       Martian      = 10,
    [Description("Big Robot")]     Colossus     = 11,
    [Description("Hyper Male")]    FastFred     = 12,
    [Description("Old Woman")]     OldWoman     = 13,
    [Description("Little Man")]    Munchkin     = 14,
    [Description("Imaginary Man")] Troll        = 15,
    [Description("Nerd")]          Nerd         = 16,
    [Description("Whiner")]        Milktoast    = 17,
    [Description("Wobbly")]        Tipsy        = 18,
    [Description("Singing Boy")]   Choirboy     = 19,
}

/// <summary>
/// SVSetLanguage values — confirmed from SVSetLanguage decompile.
/// Uses the SAME bitmask as SVOpenSpeech languageFlags, NOT a 0-based index.
/// param_2 == 1 → English (DAT_1c012038)
/// param_2 == 2 → Spanish (DAT_1c01203c)
/// param_2 == 4 → German  (DAT_1c012040)
/// </summary>
public enum TiLanguage : int
{
    English = 1,
    Spanish = 2,
    German  = 4,
}

/// <summary>
/// SVSetVoicingMode values — confirmed from x32dbg breakpoint on OG TalKit UI.
/// Valid range 0–2 (engine returns 0x1b62 for >= 3).
/// This is what the Vocal Effort buttons in the original UI actually call.
/// </summary>
public enum TiVoicingMode : ushort
{
    Normal    = 0,
    Breathy   = 1,
    Whispered = 2,
}

/// <summary>
/// SVSetGlottalSource values — 0-based index into pointer table at 0x1c0128c8.
/// Table: [0]="normal", [1]="breathy", [2]="whispered", [3]=unknown.
/// NOTE: The original UI does NOT call this for Vocal Effort buttons — use TiVoicingMode instead.
/// </summary>
public enum TiGlottalSource : int
{
    Normal    = 0,
    Breathy   = 1,
    Whispered = 2,
    Unknown3  = 3,
}

/// <summary>
/// SVSetF0Style values — confirmed from SVSetF0Style decompile (valid range 0–4).
/// Pointer table at 0x1c0128d8: natural(0), style2(1), monotone(2), s...(3), random(4).
/// NOTE: Index 1 ("style2") produces the audible whispered effect —
///   SVSetGlottalSource alone is NOT sufficient; this is the required lever.
///   Index 4 is the confirmed Sung mode. Index 3 ('s...') purpose unconfirmed.
/// </summary>
public enum TiF0Style : int
{
    Natural   = 0,
    Whispered = 1,  // labeled "style2" internally — produces the whispered vocal effect
    Monotone  = 2,
    Style3    = 3,  // 's...' in pointer table — purpose unconfirmed
    Sung      = 4,  // confirmed audible singing mode
}
//1 - whispered

/// <summary>
/// SVSetSpeakingMode values — confirmed from pointer table at 0x1c0128f0.
/// </summary>
public enum TiSpeakingMode : int
{
    Natural = 0,
    Word    = 1,
    Spell   = 2,
    Number  = 3,
}

/// <summary>
/// Language flags for SVOpenSpeech — bitmask, NOT the same as TiLanguage.
/// </summary>
[Flags]
public enum TiLanguageFlags : uint
{
    English = 1,
    Spanish = 2,
    German  = 4,
}
