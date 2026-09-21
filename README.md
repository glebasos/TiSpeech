# TiSpeech

A .NET 10 library for the SoftVoice speech engine originally shipped with Microsoft Talk It!: Windows bindings for the original **TIBASE32.DLL**, plus an in-progress portable C reconstruction.

## Portable reconstruction

`native/` contains reconstructed letter-to-sound rules, the fixed-point waveform
kernel, and a frame renderer. `TiSpeechNative` exposes the available stages
through a C ABI; `NativeTiSpeechBackend` implements the application's shared
backend interfaces. Phoneme previews work when language data is built in.
**Speech synthesis is not available yet:** the phoneme-to-frame generator and
native playback path are still missing. `Synthesize` returns `NotImplemented`,
with no audio buffer or substitute voice.

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
