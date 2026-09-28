namespace TiSpeech;

/// <summary>Native voice settings. A value of -1 keeps the personality's default.</summary>
public sealed record TiVoiceOptions(
    TiPersonality Personality = TiPersonality.Male,
    int Pitch = -1,
    int Rate = -1,
    int Voicing = -1,
    int F0Style = -1,
    int F0Range = -1,
    int F0Perturb = -1,
    int VowelFactor = -1,
    int GlottalSource = -1);
