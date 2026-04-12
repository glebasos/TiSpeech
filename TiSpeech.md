# TiSpeech — API Reference

A .NET 10 managed wrapper library for **TIBASE32.DLL** — the 32-bit SoftVoice speech synthesis engine originally shipped with Microsoft Talk It!

> **Note:** If your consuming project is AnyCPU or x64, use **TiSpeech.Client** (`TiSpeechClient`) instead of this library directly. `TiSpeechClient` is a drop-in replacement that routes calls through an x86 subprocess, so your app doesn't need to target x86. See the [TiSpeech.Client README](../TiSpeech.Client/README.md).

## Requirements

| Requirement | Detail |
|---|---|
| Platform | Windows only |
| Architecture | **x86** — TIBASE32.DLL is a 32-bit native DLL; the consuming process must be compiled as x86 |
| Framework | .NET 10, `net10.0-windows` |
| Native DLLs | `TIBASE32.DLL`, `TIENG32.DLL` (English), `TISPAN32.DLL` (Spanish), `TIGERM32.DLL` (German) |

The native DLLs are **not** included in the library. Place them in a directory and pass that path to `Open()`.

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

Add a reference to `TiSpeech.csproj` and ensure your consuming project targets x86:

```xml
<PropertyGroup>
  <PlatformTarget>x86</PlatformTarget>
</PropertyGroup>

<ItemGroup>
  <ProjectReference Include="..\TiSpeech\TiSpeech.csproj" />
</ItemGroup>
```

---

## API Reference

### `TiSpeechEngine`

The main entry point. Implements `IDisposable` — use `using` or call `Dispose()` when done.

#### Properties

| Property | Type | Description |
|---|---|---|
| `IsOpen` | `bool` | Whether the engine has been successfully opened |
| `IsSpeaking` | `bool` | Whether speech is currently in progress |

#### Events

| Event | Signature | Raised when |
|---|---|---|
| `SpeakStarted` | `EventHandler` | `Speak()` was accepted by the engine |
| `SpeakCompleted` | `EventHandler` | The engine finished speaking (native async callback) |
| `Error` | `EventHandler<string>` | Any engine call returns a non-zero error code |

#### Lifecycle

```csharp
bool Open(string dllDirectory, TiLanguageFlags languages = TiLanguageFlags.English)
```
Loads the native DLLs from `dllDirectory` and opens a speech context. The `languages` bitmask controls which language DLLs are loaded — only loaded languages can be switched to at runtime. Returns `false` and fires `Error` on failure.

```csharp
void Close()
```
Stops any current speech and releases the native context. Safe to call multiple times.

```csharp
void Dispose()
```
Calls `Close()` and destroys the internal notification window.

#### Speech Control

```csharp
void Speak(string text, bool interrupt = true)
```
Converts `text` to phonemes and speaks asynchronously. When `interrupt` is `true` (default), any currently playing speech is cut off immediately. `SpeakStarted` fires synchronously before this returns; `SpeakCompleted` fires later from the Windows message loop.

```csharp
void Stop()
```
Immediately aborts speech in progress.

```csharp
void Pause()
void Resume()
```
Pause and resume speech. Both throw `InvalidOperationException` if the engine is not open.

#### Voice Parameters

All setters silently no-op if the engine is not open.

| Method | Default | Range | Description |
|---|---|---|---|
| `SetPersonality(TiPersonality)` | — | 0–19 | Voice character (see table below) |
| `SetLanguage(TiLanguage)` | — | — | Switch active language; must be loaded in `Open()` |
| `SetPitch(int)` | 100 | 50–200 | Pitch as a percentage of the voice's natural pitch |
| `SetRate(int)` | 150 | 50–300 | Speaking rate as a percentage |
| `SetVoicingMode(TiVoicingMode)` | Normal | 0–2 | Vocal effort / voicing quality (Normal / Breathy / Whispered) |
| `SetF0Style(TiF0Style)` | Natural | 0–4 | Fundamental frequency contour shape |
| `SetSpeakingMode(TiSpeakingMode)` | Natural | — | How text tokens are interpreted |
| `SetF0Range(int)` | — | — | Pitch variation range |
| `SetF0Perturb(int)` | — | — | Random pitch perturbation amount |
| `SetVowelFactor(int)` | — | — | Vowel quality scaling |

> `SetGlottalSource(TiGlottalSource)` is also available but is **not** called by the original Talk It! UI. Use `SetVoicingMode` for vocal effort control.

#### Error Formatting

```csharp
static string FormatError(uint code)
```
Converts a native error code to a human-readable string. Also called internally before firing the `Error` event.

| Code | Meaning |
|---|---|
| `0x0000` | OK |
| `0x1b5a` | Out of memory |
| `0x1b61` | Engine busy |
| `0x1b62` | Invalid parameter (out of range) |
| `0x1b63` | No audio output device found |
| `0x1b64` | Cannot open audio device |
| `0x1b66` | Already speaking — use interrupt flag |
| `0x1b69` | Internal window creation failed |
| `0x1b6e` | Language DLL not found (TIENG32.DLL / TISPAN32.DLL) |
| `0x1b70` | Null text pointer |

---

## Enumerations

### `TiPersonality`

20 voice personalities built into TIBASE32.DLL. Indices confirmed from the engine's internal pointer table.

| Value | Index | UI Name | Notes |
|---|---|---|---|
| `Male` | 0 | Man | |
| `Female` | 1 | Woman | Female formant table |
| `LargeMale` | 2 | Hyper Female | |
| `Child` | 3 | Child | |
| `GiantMale` | 4 | Strong Man | |
| `MellowFemale` | 5 | Mellow | Female formant table |
| `MellowMale` | 6 | Singing Girl | |
| `CrispMale` | 7 | Strong Woman | |
| `TheFly` | 8 | Fly | |
| `Robotoid` | 9 | Little Robot | |
| `Martian` | 10 | Martian | |
| `Colossus` | 11 | Big Robot | |
| `FastFred` | 12 | Hyper Male | |
| `OldWoman` | 13 | Old Woman | Female formant table |
| `Munchkin` | 14 | Little Man | |
| `Troll` | 15 | Imaginary Man | Female formant table (acoustic effect) |
| `Nerd` | 16 | Nerd | |
| `Milktoast` | 17 | Whiner | |
| `Tipsy` | 18 | Wobbly | |
| `Choirboy` | 19 | Singing Boy | |

"UI Name" refers to the names used in the Talk It! application.

### `TiLanguage`

Used with `SetLanguage()` to switch the active language at runtime. The language must have been loaded during `Open()`.

| Value | Meaning |
|---|---|
| `English` | English (TIENG32.DLL) |
| `Spanish` | Spanish (TISPAN32.DLL) |
| `German` | German (TIGERM32.DLL) |

### `TiLanguageFlags`

Bitmask used with `Open()` to specify which language DLLs to load. Multiple flags can be combined.

```csharp
engine.Open(dir, TiLanguageFlags.English | TiLanguageFlags.Spanish);
```

| Flag | Value |
|---|---|
| `English` | 1 |
| `Spanish` | 2 |
| `German` | 4 |

> `TiLanguageFlags` and `TiLanguage` use the same numeric values but are separate types. `TiLanguageFlags` is used at open time; `TiLanguage` is used when switching languages on an already-open engine.

### `TiVoicingMode`

Controls vocal effort / voicing quality. Confirmed by breakpoint on `_SVSetVoicingMode@8` in the original Talk It! process — these are the exact values the OG UI passes for each Vocal Effort button.

| Value | Index | Description |
|---|---|---|
| `Normal` | 0 | Standard modal voice |
| `Breathy` | 1 | Airy, breathy quality |
| `Whispered` | 2 | Whispered speech |

Valid range enforced by the engine: values >= 3 return error `0x1b62`.

### `TiGlottalSource`

Low-level glottal source model parameter. Confirmed from the engine's internal pointer table at `0x1c0128c8` (`[0]`=normal, `[1]`=breathy, `[2]`=whispered, `[3]`=unknown).

> The original Talk It! UI does **not** call `SVSetGlottalSource` for its Vocal Effort buttons — it uses `SVSetVoicingMode` instead. This function may have an effect in combination with other parameters but is not required for normal use.

| Value | Index |
|---|---|
| `Normal` | 0 |
| `Breathy` | 1 |
| `Whispered` | 2 |
| `Unknown3` | 3 |

### `TiF0Style`

Controls the shape of the fundamental frequency (pitch) contour over time. Confirmed from `_SVSetF0Style@8` breakpoint — these are the values the original Talk It! Pitch Quality buttons pass.

| Value | Index | Description |
|---|---|---|
| `Natural` | 0 | Natural intonation patterns |
| `Whispered` | 1 | Internal "style2" — produces a whispered-like pitch contour |
| `Monotone` | 2 | Flat pitch throughout |
| `Style3` | 3 | Purpose unconfirmed |
| `Sung` | 4 | Smooth singing pitch contour |

> Note: `TiF0Style.Whispered` (index 1) is distinct from `TiVoicingMode.Whispered` (index 2). The original UI's Whispered button calls `SVSetVoicingMode(2)`. `TiF0Style` index 1 is labeled "style2" internally and was not exposed in the original UI.

### `TiSpeakingMode`

Controls how the engine interprets input text tokens.

| Value | Description |
|---|---|
| `Natural` | Normal text-to-speech |
| `Word` | Speak each word discretely |
| `Spell` | Spell out each character |
| `Number` | Read tokens as numbers |

---

## Native DLL Notes

The library wraps the following exports from `TIBASE32.DLL` (stdcall, decorated names):

| Managed method | Native export |
|---|---|
| `Open` | `_SVOpenSpeech@20` |
| `Close` | `_SVCloseSpeech@4` |
| `Stop` | `_SVAbort@4` |
| `Pause` | `_SVPause@4` |
| `Resume` | `_SVResume@4` |
| `Speak` | `_SVTTS@32` |
| `SetPersonality` | `_SVSetPersonality@8` |
| `SetPitch` | `_SVSetPitch@8` |
| `SetRate` | `_SVSetRate@8` |
| `SetLanguage` | `_SVSetLanguage@8` |
| `SetVoicingMode` | `_SVSetVoicingMode@8` |
| `SetGlottalSource` | `_SVSetGlottalSource@8` |
| `SetF0Style` | `_SVSetF0Style@8` |
| `SetF0Range` | `_SVSetF0Range@8` |
| `SetF0Perturb` | `_SVSetF0Perturb@8` |
| `SetSpeakingMode` | `_SVSetSpeakingMode@8` |
| `SetVowelFactor` | `_SVSetVowelFactor@8` |

Audio output is PCM, 11025 Hz, mono, 8-bit, sent to the default waveOut device (WAVE_MAPPER).

Speech completion is delivered via a hidden message-only window created with raw Win32 P/Invoke (`RegisterClassExW` / `CreateWindowExW` with `HWND_MESSAGE`). No Windows Forms dependency is required. The window listens for the `SVSyncMessages` registered window message; WParam `0x3E9` (1001) indicates speech has ended.
