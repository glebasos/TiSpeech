/*
 * lang.h — the SoftVoice language-module interface.
 *
 * PROVENANCE
 * ----------
 * Recovered by disassembling TIENG32.DLL (324,608 bytes, 1996-11-18, i386,
 * ImageBase 0x1C200000) with `llvm-objdump -d`. Both TIENG32.DLL and
 * TISPAN32.DLL export exactly one symbol, `LoadLanguage`, and TIBASE32.DLL
 * reaches it through LoadLibraryA + GetProcAddress.
 *
 * The reconstruction below is a direct transcription of TIENG32!LoadLanguage
 * at RVA 0x1030. Annotated disassembly:
 *
 *   ; DllMain at RVA 0x1000 runs first and publishes the static descriptor:
 *   ;   if (reason == DLL_PROCESS_ATTACH) {
 *   ;       g_descriptor = (lang*)0x1C255370;   // static, in .data
 *   ;       g_hinstance  = hinst;
 *   ;   }
 *   ;   return 1;
 *   ;
 *   ; LoadLanguage() takes NO arguments (plain `retl`, not `retl imm16`)
 *   ; and returns the descriptor pointer in EAX:
 *   ;
 *   ;   a1 3c 54 25 1c    mov eax, [g_descriptor]
 *   ;   c7 40 04 d0 10..  mov [eax+0x04], 0x1C2010D0   ; code
 *   ;   c7 40 08 90 46..  mov [eax+0x08], 0x1C204690   ; code
 *   ;   c7 00    80 67..  mov [eax+0x00], 0x1C206780   ; code
 *   ;   c7 40 0c c0 67..  mov [eax+0x0C], 0x1C2067C0   ; code
 *   ;   c7 40 14 20 d7..  mov [eax+0x14], 0x1C24D720   ; data
 *   ;   c7 40 18 50 e2..  mov [eax+0x18], 0x1C24E250   ; data
 *   ;   c7 40 1c 10 b0..  mov [eax+0x1C], 0x1C24B010   ; data
 *   ;   c7 40 20 18 b3..  mov [eax+0x20], 0x1C24B318   ; data
 *   ;   c7 40 24 28 bd..  mov [eax+0x24], 0x1C24BD28   ; data
 *   ;   c7 40 10 01..     mov [eax+0x10], 1            ; language id
 *   ;   ; then zero [eax+0x28] .. [eax+0xC4] inclusive, step 4:
 *   ;   xor ecx, ecx / xor eax, eax
 *   ; L: mov edx, [g_descriptor]
 *   ;    add eax, 4
 *   ;    cmp eax, 0xA0
 *   ;    mov [edx+eax+0x24], ecx      ; NB: store precedes the branch, so the
 *   ;    jl  L                        ;     eax==0xA0 iteration DOES store.
 *   ;    mov eax, [g_descriptor]
 *   ;    ret
 *
 * So a language module is a 4-entry vtable plus 5 static tables plus a
 * zero-initialised runtime scratch area. The engine owns no language logic
 * itself; swapping TIENG32 for TISPAN32 swaps all of it.
 *
 * CONFIRMED: field offsets, field kinds (code vs. data), the language id at
 *            +0x10, and the exact extent of the zeroed scratch region.
 * UNCONFIRMED: the semantics of the four function pointers. Naming below is a
 *            hypothesis from call-site context only and is marked as such.
 *            Do not rely on it until the callers in TIBASE32 are decompiled.
 */

#ifndef TISPEECH_LANG_H
#define TISPEECH_LANG_H

#include <stdint.h>
#include "tispeech/svapi.h"

#ifdef __cplusplus
extern "C" {
#endif

struct sv_language;

/* Size of the scratch tail that LoadLanguage zeroes: offsets 0x28..0xC4
 * inclusive with a stride of 4 => 40 dwords. */
#define SV_LANG_SCRATCH_DWORDS 40

typedef struct sv_language sv_language_t;

/* HYPOTHESIS (unconfirmed) — see header comment. The original slots hold
 * stdcall thunks; arities are not yet known, so these are deliberately typed
 * as opaque rather than guessed into a wrong signature. */
typedef void *sv_lang_fn;

struct sv_language {
    /* +0x00 */ sv_lang_fn   fn0;          /* TIENG32 RVA 0x6780 */
    /* +0x04 */ sv_lang_fn   fn_textproc;  /* TIENG32 RVA 0x10D0 — the large
                                            * routine immediately following
                                            * LoadLanguage; the rule matcher's
                                            * most likely caller. */
    /* +0x08 */ sv_lang_fn   fn2;          /* TIENG32 RVA 0x4690 */
    /* +0x0C */ sv_lang_fn   fn3;          /* TIENG32 RVA 0x67C0 */
    /* +0x10 */ int32_t      language_id;  /* 1 in TIENG32; matches SV_LANG_ENGLISH */
    /* +0x14 */ const void  *table_14;     /* TIENG32 RVA 0x4D720 */
    /* +0x18 */ const void  *table_18;     /* TIENG32 RVA 0x4E250 */
    /* +0x1C */ const void  *charclass;    /* TIENG32 RVA 0x4B010 — 4-byte
                                            * entries, character-indexed. */
    /* +0x20 */ const void  *defaults;     /* TIENG32 RVA 0x4B318 — begins with
                                            * a back-pointer then 100, 150,
                                            * 1, 2, ... i.e. the default pitch
                                            * and rate percentages. */
    /* +0x24 */ const void  *table_24;     /* TIENG32 RVA 0x4BD28 */
    /* +0x28 */ uint32_t     scratch[SV_LANG_SCRATCH_DWORDS];
};

/* The rule index: 31 pointers, one per rule bucket, at TIENG32 RVA 0x4C848.
 * Buckets 0..25 are 'A'..'Z'; buckets 26..30 cover the accented Latin-1
 * letters that trail the blob. Reached from the descriptor's static data, not
 * from a descriptor field directly. */
#define SV_RULE_BUCKETS 31

/* Loads the language module. The native reimplementation has no DLL to load;
 * rule data is supplied by the generated translation unit (see
 * tools/extract_lang.py). Returns NULL if this build has no data for `id`. */
const sv_language_t *sv_language_load(int32_t id);

#ifdef __cplusplus
}
#endif
#endif /* TISPEECH_LANG_H */
