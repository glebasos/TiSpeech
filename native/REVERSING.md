# TiSpeech native — reverse-engineering notes

Reconstruction of the SoftVoice speech engine (`TIBASE32.DLL` + language
modules) as portable C, so OpenTalkIt can run on macOS and Linux without
shipping an emulator, a Wine dependency, or a substitute text-to-speech voice.

**This is a work in progress. Nothing here fakes success.** Entry points that
are not reconstructed return `SV_E_NOTIMPL` rather than pretending to work.

## Ground rules

1. **No proprietary bytes in git.** The DLLs are SoftVoice, Inc. property. Rule
   tables and coefficient data are extracted at build time from a copy the user
   already owns (`tools/extract_lang.py`) into the build tree. The DLLs are
   parsed as files — never mapped, relocated, or executed.
2. **Every reconstructed function cites its source address.** If you cannot
   point at the disassembly, it does not go in.
3. **Bit-exact agreement with the original is the goal.** Where our output
   differs, that is a bug to be filed, not a difference to be tuned away by
   ear.

## Binaries

| File | Size | Built | Machine | ImageBase | Functions |
|---|---|---|---|---|---|
| `TIBASE32.DLL` | 79,872 | 1996-11-18 | i386 | `0x1C000000` | ~99 mapped |
| `TIENG32.DLL` | 324,608 | 1996-11-18 | i386 | `0x1C200000` | 43 |
| `TISPAN32.DLL` | 66,560 | 1996-11-18 | i386 | `0x1C400000` | 36 |

Linker version 3.0 (MSVC 4.x). All three carry `.reloc` and no TLS.

**Correction to an earlier assessment:** the language DLLs are *not* data blobs
with a trivial loader. They export one symbol but contain a complete text front
end — 43 and 36 functions respectively, several of them 2–6 KB. The single
export is an entry point, not a measure of content.

**Second correction:** an early estimate that the synthesis core is x87-heavy
came from a byte-histogram heuristic over the `D8`–`DF` opcode range, which is
not a reliable signal. Decompilation of the waveform kernel at
`TIBASE32!FUN_1c00402c` shows **fixed-point integer** formant filtering with no
x87 in the sample loop. Any argument that a recompiled native port must lose
fidelity to floating-point precision differences is therefore withdrawn — it
was built on a bad measurement.

## Reconstructed so far

### `TIENG32!LoadLanguage` @ `0x1C201030` — DONE

Confirmed against both `llvm-objdump` and Ghidra. See `include/tispeech/lang.h`
for the annotated disassembly. A language module is a 4-entry vtable plus 5
static tables plus a zeroed 40-dword scratch area:

| Offset | Kind | TIENG32 value |
|---|---|---|
| `+0x00` | code | `0x1C206780` |
| `+0x04` | code | `0x1C2010D0` |
| `+0x08` | code | `0x1C204690` |
| `+0x0C` | code | `0x1C2067C0` |
| `+0x10` | int | `1` (language id; matches `SV_LANG_ENGLISH`) |
| `+0x14` | data | `0x1C24D720` |
| `+0x18` | data | `0x1C24E250` |
| `+0x1C` | data | `0x1C24B010` (character classes) |
| `+0x20` | data | `0x1C24B318` (defaults: 100, 150, …) |
| `+0x24` | data | `0x1C24BD28` |
| `+0x28`…`+0xC4` | scratch | zeroed, 40 dwords |

`DllMain` @ `0x1C201000` publishes the static descriptor at `0x1C255370` on
`DLL_PROCESS_ATTACH`. `LoadLanguage` takes no arguments (plain `ret`) and
returns that pointer.

### `TIENG32!FUN_1c206860` — letter-to-sound matcher — DONE

Implemented in `src/ruleset.c`; syntax and all twelve context metacharacters
documented in `include/tispeech/ruleset.h`. Rule form is
`LEFT[MATCH]RIGHT=OUTPUT` with `\` or `` ` `` as terminator. Left context is
matched walking backwards with a stride of −1, right context forwards with +1;
the shared loop is the same code in the original, which is why a single
`sv_match_context()` with a stride parameter reproduces it exactly.

Bucket dispatch is `*(char **)(0x1C24C744 + (signed char)c * 4)`, i.e. a table
indexed by *signed* char, which is how the Latin-1 letters get their own
entries. Buckets point into one shared blob and matching runs off the end of
one letter's rules into the next letter's — reproduced by storing the blob once
and indexing by offset.

Helpers `FUN_1c207180` (accented-letter test) and `FUN_1c2070b0` (accent
stripping, vowel-gated) are reconstructed inline.

Character-class bits, read off the decompiled switch:

| Bit | Meaning | Used by |
|---|---|---|
| `0x0001` | — | `>` |
| `0x0002` | — | `?`, and the outer stop test |
| `0x0004` | palatalising consonant | `@` |
| `0x0010` | sibilant | `&` |
| `0x0020` | consonant | `^`, `:` |
| `0x0028` | voiced consonant | `.` |
| `0x0040` | vowel | `#`, accent stripping |
| `0x0080` | alphabetic | bucket dispatch, `' '`, `%` |
| `0x0100` | front vowel | `+` |
| `0x0400` | is-a-metacharacter | literal-vs-switch dispatch |

**Status: 13/13 deterministic tests pass** against rules extracted from the
real `TIENG32.DLL`, and the matcher is now differentially proven against the
original:

```
$ python tools/verify_ruleset.py --dll .../TIENG32.DLL --library build/librules_eng.dylib
=== dictionary cohort (20000 words) ===   mismatches: 0
=== randomized cohort (5000 strings) ===  mismatches: 0
PASS: 25000 inputs produce byte-identical output, rule flags and cursor position
      in both the original and the reconstruction
```

Output bytes, rule flags and cursor position all agree. Seeded at `0x19961118`;
the dictionary cohort is drawn from `/usr/share/dict/words`.

### `TIBASE32!FUN_1c004120` — waveform sample kernel — DONE, VERIFIED

Implemented in `src/dsp.c` as `sv_dsp_sample()`; state layout in
`include/tispeech/dsp.h`. One call consumes prepared coefficients plus two
noise samples and produces one unsigned 8-bit PCM sample, advancing seven
two-pole filters and three interpolation ramps.

Fixed-point integer throughout, as the decompile of `FUN_1c00402c` predicted:
no x87 in the sample loop. Two details that a "clean" rewrite gets wrong and
that the differential caught the project reasoning about up front —

- the loop wraps at every 16-bit add and subtract, so the narrowing is part of
  the algorithm, not an artefact;
- the frication shaping ends in two successive `SHLD`s that reuse the same
  unshifted low product word. Writing that as `product >> 12` changes the
  rounding.

**Status: verified bit-exact.** `tools/verify_dsp.py` maps the real
`TIBASE32.DLL` under Unicorn, runs VA `1c004120..1c004493` directly, and
compares against the C reconstruction called through ctypes — both the output
sample and all translated state, since clipping to ±200 before the output
curve can otherwise hide an error:

```
$ python tools/verify_dsp.py --dll .../TIBASE32.DLL --library build/libdsp.dylib --cases 20000
PASS: 20000 randomized native samples and filter states match the original x86 loop
```

Seeded at `0x19961118`, so the run is reproducible. This settles the fidelity
question for the kernel: the port loses nothing to the original here. It says
nothing about the stage that *feeds* the kernel, which is not reconstructed.

`--audit-state` widens the comparison from the modelled spans to all 0x400
bytes, which is how you find state the original writes and we silently drop.
Run over 4000 cases it reports exactly three such places, all since explained:

| Offset | What it is |
|---|---|
| `0x280` | the sign-selected aspiration level for this sample |
| `0x288` | the sign-selected frication level for this sample |
| `0x2e4` | the noise table pointer |

`0x280` and `0x288` are **per-sample scratch, not frame parameters.** The
original spills the value it picked from the `aspiration_negative` /
`aspiration_positive` pair — and likewise for frication — back into the state
block; our reconstruction keeps the same value in a local. Verified over 6000
unambiguous emulated calls: each slot always holds one of its own two input
levels, and both always come from the same branch. Frame-generation code
should not try to set these.

`0x2e4` advances by exactly 4 bytes per output sample, in 6000 of 6000 cases —
two int16 noise samples consumed per sample produced. That is a hard constraint
on whatever drives the kernel.

Finding these took widening the audit. The narrower check had passed 20,000
cases while all three were invisible, which is the argument for auditing whole
structures rather than the fields you already believe in.

A second instance of that same lesson, found later while reconstructing the
frame stage. The sample loop at `0x1c0041d0` treats `previous_voice` as a full
**dword** at `0x2aa`:

```
ror  edx,16
xor  dx,dx
mov  ecx,edx
xchg [ebx+0x2aa],edx
sub  ecx,edx
```

`src/dsp.c` keeps it as an `int16_t`, which is correct **only because** the loop
zeroes the low half before every store and the reset zeroes the whole dword. So
the fraction is unobservable — but only for a caller that reset the block.
`verify_dsp.py` could not see this at all: its state image is zero-filled
outside the modelled spans, so the fraction was 0 in all 20,000 cases. No code
change is needed, and none was made. The invariant is the point: **a caller that
hands the kernel a state block it did not reset diverges on its first sample.**
`sv_frame_reset()` clears the whole dword, matching `0x1c0040b0`.

Two more notes from that reconstruction. `0x280`/`0x288` are confirmed
per-sample scratch from the other direction as well — the sample loop reseeds
both every sample, at `0x1c0041c2` and `0x1c0041c9` — so the render differential
excludes exactly those two words while the apply differential still compares
them. And the frame scan at `0x1c0040e8` inside the reset is dead code:
`0x1c0040e6` is an unconditional `jmp` over it and nothing else targets it.

### `TIBASE32!FUN_1c0044cb` — frame renderer — DONE, VERIFIED

Implemented in `src/frames.c`; frame layout and table shapes in
`include/tispeech/frames.h`. This is the stage that *drives* the sample kernel:
it consumes an array of parameter frames, interpolates between them, and calls
`sv_dsp_sample()` to fill a PCM buffer, resuming mid-frame across calls.

Verified bit-exact against the real `TIBASE32.DLL`:

```
$ python tools/verify_frames.py --dll .../TIBASE32.DLL \
        --library build/libframes.dylib --cases 20000 --check-extract
PASS apply:   20000 randomized frames — every byte of the original's 0x400 state block matches VA 0x1c0044cb..0x1c004969
PASS render:  2500 randomized utterances — PCM and full state match VA 0x1c00402c..0x1c00498e, restart and resume paths both covered
PASS phoneme: 20000 randomized names — code and consumed count match VA 0x1c004c85..0x1c004d29
PASS extract: tools/extract_base.py emits the same bytes
```

Seeded at `0x19961118`. The verifier reads its tables from the emulator image
rather than from `extract_base.py`'s output, so a matching mistake in both
cannot cancel out; `--check-extract` compares them separately.

Four fidelity bugs in the first draft of this file were caught only by the
whole-state differential, and each is worth recording:

1. **The amplitude table is indexed by BYTE offset, not by element.**
   `0x1c004857` is `movzx edi, byte [esi+0x10]` then `mov ax, word
   [edi+0x1c001308]` with **no `shl edi,1`** — unlike every other table lookup
   in the function, which does scale. Index 1 yields 5888 under the original
   and 23 under element indexing.
2. **The resume path returns to `0x1c0044a0`, not to the sample loop.**
   `0x1c004053` jumps there when `restart == 0`. A call returns out of
   `0x1c00449a` one instruction *before* decrementing the noise and frame
   counters it owes; the next call pays them on entry. Resuming at the sample
   loop instead stretches every buffer boundary by one sample. Externally
   visible as `noise_index == 2*(0x400 - noise_left)`, not `2*(0x3ff - noise_left)`.
3. **`frame_index` is committed at `0x1c0044f2`, before** the length and range
   checks, not after them.
4. **The low half of the previous-voice dword at `0x2aa` was unmodelled**, so
   the reset did not clear it although `0x1c0040b0` clears the whole dword.

Deliberate divergences from the original, all commented at their site in
`src/frames.c`: out-of-range table indices return `SV_FRAMES_E_RANGE` where the
original reads its own `.text`; `count == 0` is refused (`SV_FRAMES_E_COUNT`)
rather than reproducing the original's 65536-sample buffer overrun; a rejected
frame leaves state untouched where the original would have written
`frame_length`/`interp_scale`/`elapsed` before faulting; `'['` (phoneme list
`0x3b`) returns invalid where the original dereferences NULL — the verifier
asserts that fault actually happens rather than skipping the case. The six
coefficient tables are one contiguous blob, so an over-range formant row reads
into the next table exactly as the original does.

**Not covered.** The event-reporting block `0x1c004543..0x1c004605` is not
reconstructed, so the whole differential runs with events suppressed;
`sv_frame_apply()` refuses without `SV_FRAME_FLAG_NO_EVENTS` rather than
dropping events silently. Nothing is known about `FUN_1c00498f`. Frame fields
`+0x05`, `+0x0c`, `+0x15`, `+0x16`, `+0x1e`, `+0x1f` are never read by the
renderer and may matter to the unreconstructed generator or to the smoothing
passes at `0x1c00be40`, `0x1c00cd40`, `0x1c00de60`. The differential covers
durations 1..31 at 11025 Hz; other sample rates are exercised only through the
arithmetic test, not against the DLL.

## The language modules are one code base

`TISPAN32` was examined to see what Spanish would cost, since the application
already offers the language and the engine advertises it. It is the same
program as `TIENG32` with different tables.

`LoadLanguage` is exported at module base `+0x1030` in both, with the same
instruction sequence, and publishes a descriptor with the same field layout:

| Field | TIENG32 | TISPAN32 | Note |
|---|---|---|---|
| `+0x00` | `0x1C206780` | `0x1C406210` | code |
| `+0x04` | `0x1C2010D0` | `0x1C4010D0` | code, both at module `+0x10D0` |
| `+0x08` | `0x1C204690` | `0x1C404020` | code |
| `+0x0C` | `0x1C2067C0` | `0x1C406250` | code |
| `+0x10` | `1` | `2` | language id; `2` == `SV_LANG_SPANISH` |
| `+0x14` | `0x1C24D720` | `0x1C40F368` | data |
| `+0x18` | `0x1C24E250` | `0x1C40FE98` | data |
| `+0x1C` | `0x1C24B010` | `0x1C40D010` | both `.data+0x010` |
| `+0x20` | `0x1C24B318` | `0x1C40D318` | both `.data+0x318` |
| `+0x24` | `0x1C24BD28` | `0x1C40DA50` | data, size-dependent |

`+0x1C` and `+0x20` sit at identical offsets from each module's `.data` base,
and the `+0x04` entry is at an identical offset from each `.text` base.

Comparing the vtable functions themselves settles it. Byte equality is
impossible — every absolute address differs — so the comparison is over
capstone's mnemonic plus operand shape with immediates normalised, across a
120-instruction window from each entry point:

| Entry | Identical instruction forms |
|---|---|
| `+0x00` | 120/120 |
| `+0x04` | diverges at instruction 0 (English opens `sub esp,4`, Spanish `push ebx`) |
| `+0x08` | 120/120 |
| `+0x0C` | 120/120 |

Three of the four are the same function compiled twice. Only `+0x04` genuinely
differs, and English `.text` is `0x48F40` against Spanish's `0xA590`, so the
English module carries a great deal of code the Spanish one does not —
consistent with a larger dictionary or normaliser rather than a different
front end.

**What this means for the port:** the reconstructed matcher in `src/ruleset.c`
should drive Spanish unchanged. What is missing is purely locational —
`tools/extract_lang.py` hardcodes TIENG32 addresses for `charclass`,
`bucket_base`, `fallback` and `qmark_sub`, and none of those four are
descriptor fields, so the Spanish equivalents still have to be found. The
descriptor itself is the reliable anchor and should be followed rather than
hardcoded.

**Caveat.** This is a structural argument, not a differential test. Nothing
here has been run against Spanish input, and the rule *data* format is assumed
rather than shown to be identical. Treat Spanish as cheap-looking, not proven.

## Open questions

1. ~~**Differential verification.**~~ **SETTLED.** `tools/verify_ruleset.py`
   runs the matcher against the original over 20,000 dictionary words and 5,000
   randomized strings: 0 mismatches on output bytes, rule flags and cursor
   position. The matcher is proven, not merely plausible.
   - `BEFORE` is settled too, and the dead-data reading was right. The original
     yields `BIXFOHR`, not `BIXFOH3R`: `[BE]^#=BIX` sits 126 bytes earlier and
     fires first under first-match-wins, so the `[BEFORE]=BIXFOH3R` entry is
     unreachable in the original table. Confirmed by direct probe against
     `TIENG32.DLL` — original and reconstruction agree byte for byte.
   - Still open in the same area: the Spanish module (`TISPAN32`) has not been
     put through the same differential.
2. **`sv_language` vtable semantics.** Field kinds (code vs. data) are certain;
   what the four functions *do* is inferred from call-site context only. Left
   deliberately typed as opaque rather than guessed into a wrong signature.
   Partially narrowed: three of the four are shared verbatim with `TISPAN32`
   (see above), so they are language-independent machinery, and only `+0x04`
   carries per-language code. That constrains what they can be without yet
   saying what they are.
3. **Rule flags.** The matcher tags each emitted run: starts at `4`, `|1` for a
   `\` terminator, `|2` for `` ` ``, `|8` when the output contains `.` or `?`.
   Bit 2 (the initial `4`) has no established meaning yet. Currently computed
   and discarded.
4. **Tables `+0x14`, `+0x18`, `+0x24`** are unidentified.
5. **Coverage.** `TIBASE32`'s Ghidra map has large unmapped gaps
   (`0x1C0010A2`–`0x1C0036F0`, `0x1C005DE4`–`0x1C00BE40`). Those are data or
   hand-written assembly; do not assume the decompilation is complete.

## Not started

Text normalisation (numbers, abbreviations), the user dictionary, phoneme →
parameter-frame generation, and the whole `TIBASE32` public API beyond its
declared surface.

The pipeline now has working ends and a missing middle:

| Stage | Status |
|---|---|
| text → phonemes | reconstructed and verified bit-exact (`src/ruleset.c`), English only |
| phonemes → parameter frames | **not started** |
| parameter frames → PCM | reconstructed and verified bit-exact (`src/frames.c`, `src/dsp.c`) |
| PCM → audio device | not started |

The missing middle has a name: it is the language module's vtable slot `+0x08`,
driven by `TIBASE32!FUN_1c005840`. The frame stage below it *consumes* frames;
nothing yet *produces* them.

Until the middle stage lands, `tispeech_synthesize()` in `src/capi.c` returns
`TISPEECH_E_NOTIMPL` and the application keeps Talk disabled off Windows.

## Tooling

Earlier notes in this file cite Ghidra decompilation. Ghidra is not required to
work on this, and is not installed on the current development machine. What is
actually in use:

- **Disassembly.** Apple's `objdump` reads these PE/COFF-i386 images directly
  and resolves the export names, so no import step is needed:
  ```sh
  objdump -d --start-address=0x1c206860 --stop-address=0x1c206a00 TIENG32.DLL
  ```
  It is a disassembler, not a decompiler — reading a 6 KB function this way is
  slow, which is a real constraint on how fast the remaining stages can go.
- **Differential verification.** `unicorn` 2.1.4 emulates the original i386 code
  in process, `pefile`/`capstone` for parsing and scripted disassembly. This is
  the decisive tool: `tools/verify_dsp.py` is the pattern every future
  reconstruction should copy — map the real DLL, run the original function,
  call the C reconstruction through `ctypes`, compare output *and* state.
  That pattern has since been copied twice, and both times the whole-state
  comparison caught fidelity bugs an output-only check had missed:
  `tools/verify_frames.py` for the frame renderer and `tools/verify_ruleset.py`
  for the letter-to-sound matcher. Write the differential before believing a
  reconstruction, not after.

Nothing in this list is a runtime dependency. The shipped library links no
emulator and parses no DLL at run time.

## Build

```sh
cmake -B build -DTISPEECH_ENG_DLL=/path/to/TIENG32.DLL \
               -DTISPEECH_BASE_DLL=/path/to/TIBASE32.DLL
cmake --build build
ctest --test-dir build
./build/svphon "hello world"
```

Both DLL paths are optional and independent, and neither is committed — each is
parsed as a file at build time, never loaded or executed. What you pass decides
what gets built:

| Set | Adds |
|---|---|
| neither | rule engine, DSP, frame renderer, shared library; `capi` test only |
| `TISPEECH_ENG_DLL` | English rule data, `svphon`, `ruleset` test |
| `TISPEECH_BASE_DLL` | base coefficient tables, `frames` test |

The library code always compiles without either: callers supply the tables at
the C boundary. Only the generated data libraries and the tests that need them
are gated. Python is required only when at least one DLL is set.

`libtispeech` (`src/capi.c`, `include/tispeech/capi.h`) is the ABI the .NET side
P/Invokes. It is deliberately separate from `svapi.h`: `svapi.h` reconstructs
SoftVoice's interface and keeps its shapes, while `capi.h` is ours and exposes
only what the reconstruction has actually finished. `tispeech_capabilities()`
reports that, so the application gates on real capability rather than on the
host operating system.

The C ABI takes UTF-8 restricted to Latin-1. It decodes that UTF-8 to the
matcher's single-byte character set and upper-cases Latin-1 letters without
using the process locale. Malformed UTF-8 and characters outside Latin-1 are
rejected, not passed through as unrelated rule-table indices. Managed callers
also reject embedded NULs rather than silently converting only a prefix.
`test_capi` covers this boundary against the raw matcher and runs even without
language data (capability, argument, and not-implemented checks).

Verifying the reconstruction against the original (development only, needs
`unicorn`):

```sh
cc -shared -fPIC -std=c11 -Iinclude src/dsp.c -o build/libdsp.dylib
python tools/verify_dsp.py --dll /path/to/TIBASE32.DLL --library build/libdsp.dylib

cc -shared -fPIC -std=c11 -Wall -Wextra -Wconversion -Iinclude \
   src/dsp.c src/frames.c -o build/libframes.dylib
python tools/verify_frames.py --dll /path/to/TIBASE32.DLL \
       --library build/libframes.dylib --cases 20000 --check-extract

cc -shared -fPIC -std=c11 -Wall -Wextra -Iinclude \
   src/ruleset.c build/eng_lang_data.c -o build/librules_eng.dylib
python tools/verify_ruleset.py --dll /path/to/TIENG32.DLL \
       --library build/librules_eng.dylib
```

`verify_ruleset.py` also takes `--probe WORD` (repeatable) to compare a single
input against the original and print both results side by side, which is the
fastest way to settle a specific rule-table question.
