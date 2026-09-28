# TiSpeech

A .NET 10 library for the SoftVoice speech engine originally shipped with Microsoft Talk It!: Windows bindings for the original **TIBASE32.DLL**, plus an in-progress portable C reconstruction.

## Portable reconstruction

`native/` reconstructs the English text front end and the full phoneme-to-PCM
pipeline. `TiSpeechNative.TextToPhonemes` includes exception pronunciations,
number expansion and default stress for English; Spanish currently has only
letter-to-sound conversion. English input is limited to 514 Latin-1 characters
per call. Longer passages return an explicit error.

With both `TIENG32.DLL` and `TIBASE32.DLL` supplied at build time,
`TiSpeechNative.Synthesize` returns original-voice 8-bit mono PCM at 11025 Hz.
The complete text-to-audio path has been compared sample for sample with the
original engine. The `svsay -t "hello world" out.wav` native tool converts plain
text directly to a WAV file. Inline synthesis commands and user dictionaries
remain unsupported.

**Application playback is not wired up yet.** `NativeTiSpeechBackend.Open`
still returns false and Talk/Export remain disabled; library callers and
`svsay` can already generate audio. CMake uses `TISPEECH_ENG_DLL`,
`TISPEECH_SPAN_DLL`, and `TISPEECH_BASE_DLL`; MSBuild accepts `TiSpeechEngDll`,
`TiSpeechSpanDll`, and `TiSpeechBaseDll` and detects copies in OpenTalkIt/DLLs.

Language tables are extracted at build time from original DLLs you supply; no
proprietary table data is stored in source control. The runtime neither loads
these Windows DLLs nor uses an emulator. See [native/REVERSING.md](native/REVERSING.md)
for reconstructed addresses, build options, and differential verification.

The managed library targets plain `net10.0` and builds on macOS/Linux/Windows.
`dotnet build` attempts an optional CMake build when the language DLLs are
available beside OpenTalkIt, and copies the resulting library next to the app.
Without native tooling or data, managed builds still work and the backend
reports the unavailable features. Use `-p:TiSpeechBuildNative=false` to skip the
native build, or `TISPEECH_NATIVE_LIB` to select an existing native library.

## Original Windows engine

Because `TIBASE32.DLL` is a 32-bit native DLL, any process that loads it directly must also be 32-bit (x86). There are two ways to consume this library depending on your application's architecture:

| Scenario | What to use |
|---|---|
| Your app is x86 | Reference **TiSpeech** directly and use `TiSpeechEngine` |
| Your app is AnyCPU or x64 | Reference **TiSpeech.Client** and use `TiSpeechClient` (drop-in replacement) |

`TiSpeechClient` is a proxy that spawns **TiSpeech.Host** (an x86 subprocess) and communicates with it over a named pipe. The API is identical to `TiSpeechEngine`, so switching between them requires no code changes beyond the constructor.

## Requirements

| Requirement | Detail |
|---|---|
| Platform | Windows only |
| Architecture | **x86** — required only for direct use; TiSpeech.Client handles this transparently |
| Framework | .NET 10, `net10.0` (the original engine's runtime calls remain Windows-only) |
| Native DLLs | `TIBASE32.DLL`, `TIENG32.DLL` (English), `TISPAN32.DLL` (Spanish), `TIGERM32.DLL` (German) |

The native DLLs are **not** included. Place them in a directory and pass that path to `Open()`.

## Quick Start

```csharp
using TiSpeech;

using var engine = new TiSpeechEngine();

engine.Error += (_, msg) => Console.WriteLine($"Error: {msg}");
engine.SpeakCompleted += (_, _) => Console.WriteLine("Done.");

if (engine.Open(@"C:\path\to\dlls", TiLanguageFlags.English))
{
    engine.SetPersonality(TiPersonality.Female);
    engine.SetVoicingMode(TiVoicingMode.Normal);
    engine.SetF0Style(TiF0Style.Natural);
    engine.SetRate(150);
    engine.Speak("Hello, world!");
}
```

## Project Setup

For x86 projects, reference TiSpeech directly:

```xml
<PropertyGroup>
  <PlatformTarget>x86</PlatformTarget>
</PropertyGroup>

<ItemGroup>
  <ProjectReference Include="..\TiSpeech\TiSpeech.csproj" />
</ItemGroup>
```

For AnyCPU or x64 projects, reference TiSpeech.Client instead — no `PlatformTarget` change required:

```xml
<ItemGroup>
  <ProjectReference Include="..\TiSpeech.Client\TiSpeech.Client.csproj" />
</ItemGroup>
```

See [`TiSpeech.md`](TiSpeech.md) for the full API reference.
