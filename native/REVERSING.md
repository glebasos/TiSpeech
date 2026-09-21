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
real `TIENG32.DLL`.

## Open questions

1. **Differential verification.** Every expectation in `tests/test_ruleset.c`
   is quoted from the rule table, so it is ground truth about the *data* — but
   nothing yet proves our *matcher* agrees with TIENG32 on arbitrary input.
   The decisive test is a differential run over a large word list against the
   original. Until that exists, treat the matcher as plausible, not proven.
   - First case to settle: `BEFORE`. The table holds `[BEFORE]=BIXFOH3R`, but
     `[BE]^#=BIX` sits 126 bytes earlier and fires first under first-match-wins,
     yielding `BIXFOHR`. We believe `[BEFORE]` is unreachable dead data in the
     original table. Confirm against the real DLL.
2. **`sv_language` vtable semantics.** Field kinds (code vs. data) are certain;
   what the four functions *do* is inferred from call-site context only. Left
   deliberately typed as opaque rather than guessed into a wrong signature.
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
parameter-frame generation, the DSP kernel, and the whole `TIBASE32` public
API beyond its declared surface. `sv_text_to_phon` is the only pipeline stage
with a real implementation behind it.

## Build

```sh
cmake -B build -DTISPEECH_ENG_DLL=/path/to/TIENG32.DLL
cmake --build build
./build/test_ruleset
./build/svphon "hello world"
```

Without `-DTISPEECH_ENG_DLL` only the rule engine builds; the CLI and tests
need language data.
