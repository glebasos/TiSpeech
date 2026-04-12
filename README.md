# TiSpeech

A .NET 10 managed wrapper library for **TIBASE32.DLL** — the 32-bit SoftVoice speech synthesis engine originally shipped with Microsoft Talk It!

## Architecture Overview

Because `TIBASE32.DLL` is a 32-bit native DLL, any process that loads it directly must also be 32-bit (x86). There are two ways to consume this library depending on your application's architecture:

| Scenario | What to use |
|---|---|
| Your app is x86 | Reference **TiSpeech** directly and use `TiSpeechEngine` |
| Your app is AnyCPU or x64 | Reference **TiSpeech.Client** and use `TiSpeechClient` (drop-in replacement) |

`TiSpeechClient` is a proxy that spawns **TiSpeech.Host** (an x86 subprocess) and communicates with it over stdin/stdout. The API is identical to `TiSpeechEngine`, so switching between them requires no code changes beyond the constructor.

## Requirements

| Requirement | Detail |
|---|---|
| Platform | Windows only |
| Architecture | **x86** — required only for direct use; TiSpeech.Client handles this transparently |
| Framework | .NET 10, `net10.0-windows` |
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
