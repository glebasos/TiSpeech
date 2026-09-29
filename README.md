# TiSpeech

A .NET 10 library for the SoftVoice speech engine originally shipped with Microsoft Talk It!: Windows bindings for the original **TIBASE32.DLL**, plus an in-progress portable C reconstruction.

## Portable reconstruction

[TalkIt_OSS](https://github.com/glebasos/TalkIt_OSS), checked out as a sibling
(`../TalkIt_OSS`, or set `-p:TiSpeechNativeDir`), reconstructs the English and Spanish text front ends and the full
phoneme-to-PCM pipeline. `TiSpeechNative.TextToPhonemes` includes exception
pronunciations, number expansion and default stress in both languages. Input is
limited to 514 Latin-1 characters per call. Longer passages return an explicit
error. Both language datasets are included in the native repo.

`TiSpeechNative.Synthesize` returns original-voice 8-bit mono PCM at 11025 Hz
for English and Spanish (`TiSpeechNative.SynthesisLanguages`).
The complete text-to-audio path has been compared sample for sample with the
original engine in both languages, and so have the engine's word, syllable,
phoneme and mouth-shape events: `NativeTiSpeechBackend` raises them as
`SpeechEvent` while it plays (`ITiSpeechEventSource`). The `svsay -t "hello world" out.wav` native
tool converts plain text directly to a WAV file (`-s` for Spanish). SoftVoice
user dictionaries (`SVXF` files) are supported via `TiUserDictionary` and
`NativeTiSpeechBackend.LoadUserDictionary`.

`NativeTiSpeechBackend` plays synthesized speech through the system player
(`afplay` on macOS, `paplay`/`aplay` on Linux), so OpenTalkIt's Talk and Export
work without the Windows host on macOS/Linux.

The native repo compiles the extracted C sources in `TalkIt_OSS/data` directly.
No original DLLs or Python are required for the portable engine's normal build
or synthesis path. `dotnet build` attempts an optional CMake build and copies
the library next to the app. Without native tooling, managed builds still work
and the backend reports unavailable features. Use `-p:TiSpeechBuildNative=false`
to skip the native build, or `TISPEECH_NATIVE_LIB` to select an existing library.

The optional runtime DLL loader remains available for development and custom
builds: `TiSpeechNative.LoadDlls(dir)` / `LoadDllFiles(...)`. A library configured
without compiled data searches `TISPEECH_DLL_DIR`, the app's folder and its
`DLLs`/`x86` subfolders, beside a macOS `.app`, then the per-user `TiSpeech/DLLs`
folder. `DllDirectory` and `DllLoadError` report runtime loading. DLLs are parsed
as files, never executed by the portable engine. See
[TalkIt_OSS data](https://github.com/glebasos/TalkIt_OSS/tree/main/data) for
regeneration and verification. The original Windows backend below still uses
its DLLs and x86 host.

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
