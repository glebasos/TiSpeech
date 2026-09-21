/*
 * ruleset.h — the SoftVoice letter-to-sound rule engine.
 *
 * PROVENANCE
 * ----------
 * Reconstructed from TIENG32.DLL!FUN_1c206860 (Ghidra decompilation, cross-
 * checked against `llvm-objdump` disassembly). This is one of ~43 functions in
 * TIENG32; the language module is a complete text front end, not a data blob.
 * This header covers the rule-matching stage only.
 *
 * RULE SYNTAX (confirmed from the matcher, not guessed)
 * -----------------------------------------------------
 *   LEFT [ MATCH ] RIGHT = OUTPUT <terminator>
 *
 * The literal text between '[' and ']' is compared against the input at the
 * cursor. LEFT is then verified walking BACKWARDS from the cursor (the
 * original uses a stride of -1), and RIGHT walking forwards (stride +1).
 * A rule ends with '\' (0x5C) or '`' (0x60); both terminate, and which one it
 * is becomes a flag on the emitted output (see sv_rule_flags below).
 *
 * Buckets are selected by the first input character via a table indexed by
 * SIGNED char, so the Latin-1 letters land in their own entries and everything
 * else falls through to a shared fallback bucket.
 *
 * CONTEXT METACHARACTERS (all confirmed against the decompiled switch)
 * --------------------------------------------------------------------
 *   ' '  word boundary: current char is not alphabetic, then skip whitespace
 *   '#'  exactly one vowel                              (class & 0x0040)
 *   '%'  a suffix: ER / E / ELY / ES / ED / ING (+ optional trailing S),
 *        and the character after it must not be alphabetic
 *   '&'  a sibilant: 'H' preceded by 'C' or 'S' (consumes two), else
 *                                                        (class & 0x0010)
 *   '+'  a front vowel (E, I, Y)                    (class_hi & 0x0001)
 *   '.'  a voiced consonant                  ((class & 0x0028) == 0x0028)
 *   ':'  zero or more consonants                        (class & 0x0020)
 *   '<'  not whitespace AND (class & 0x0003) != 0
 *   '>'  (class & 0x0001) == 0
 *   '?'  (class & 0x0002) != 0
 *   '@'  'H' preceded by 'T', 'C' or 'S' (consumes two), else
 *                                                        (class & 0x0004)
 *   '^'  exactly one consonant                          (class & 0x0020)
 *   'b'  one or more whitespace characters
 *
 * A character participates in the switch above only when its class table entry
 * has (class_hi & 0x0004); otherwise it is compared literally. That bit is what
 * lets a rule match a literal '#' or ':' where the language needs one.
 *
 * Character-class bit assignments, read off the same switch:
 *   0x0001 see '>'            0x0002 see '?'         0x0004 '@' consonant set
 *   0x0010 sibilant           0x0020 consonant       0x0040 vowel
 *   0x0080 alphabetic         0x0028 voiced consonant (0x08 | 0x20)
 *   0x0100 front vowel        0x0400 is-a-metacharacter
 */

#ifndef TISPEECH_RULESET_H
#define TISPEECH_RULESET_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Class-table bits. Names are ours; values are the engine's. */
#define SV_CC_BIT0      0x0001u
#define SV_CC_BIT1      0x0002u
#define SV_CC_ATSET     0x0004u
#define SV_CC_VOICED_LO 0x0008u
#define SV_CC_SIBILANT  0x0010u
#define SV_CC_CONSONANT 0x0020u
#define SV_CC_VOWEL     0x0040u
#define SV_CC_ALPHA     0x0080u
#define SV_CC_VOICED    (SV_CC_VOICED_LO | SV_CC_CONSONANT) /* 0x0028 */
#define SV_CC_FRONTVOW  0x0100u
#define SV_CC_METACHAR  0x0400u

/* Flags the matcher attaches to each emitted output run. Confirmed from the
 * tail of FUN_1c206860: the accumulator starts at 4, gains 1 for a '\'
 * terminator, 2 for a '`' terminator, and 8 when the output contains '.' or
 * '?'. The meaning of bit 2 (the initial 4) is not yet established. */
#define SV_RF_BACKSLASH 0x1u
#define SV_RF_BACKTICK  0x2u
#define SV_RF_INITIAL   0x4u
#define SV_RF_SENTENCE  0x8u

typedef struct {
    const unsigned short       *charclass;   /* 256 entries */
    const unsigned char *const *buckets;     /* 256 entries, may be NULL */
    const unsigned char        *fallback;    /* bucket for everything else */
    unsigned char               qmark_sub;   /* emitted for a bare '?' */
} sv_ruleset_t;

typedef struct {
    const sv_ruleset_t *rules;
    const char         *in;        /* cursor into the input text */
    char               *out;       /* cursor into the output buffer */
    size_t              out_left;  /* remaining output capacity */
    unsigned            flags;     /* SV_RF_* from the last rule applied */
    unsigned            opts;      /* bit 0x40 suppresses the '?' substitution */
} sv_rule_ctx_t;

/*
 * Translate `in` into phoneme text. Returns 0 on success, 1 if the output
 * buffer filled up. Faithful port of TIENG32!FUN_1c206860.
 *
 * NOTE: this is the grapheme-to-phoneme stage ONLY. It does not do the text
 * normalisation that runs ahead of it (numbers, abbreviations, the user
 * dictionary) nor anything downstream of it. Those live in other TIENG32
 * functions that are not reconstructed yet.
 */
int sv_rules_apply(const sv_ruleset_t *rules, const char *in,
                   char *out, size_t out_size, unsigned opts);

/*
 * As sv_rules_apply(), but also reports the two side-channels the original
 * writes back into its context block:
 *
 *   out_flags     SV_RF_* of the LAST rule that fired, i.e. the original's
 *                 ctx+0x1C. Zero when no rule fired at all (the original
 *                 leaves the field untouched in that case; we model an
 *                 incoming zero, which is what the callers set up). The
 *                 original also zeroes it on the buffer-full path.
 *   out_consumed  number of input characters consumed, i.e. the original's
 *                 final ctx+0x08 minus the starting cursor.
 *
 * Both may be NULL. This exists so tools/verify_ruleset.py can compare more
 * than the phoneme text against the emulated original — two runs can agree on
 * output while disagreeing on where the cursor stopped.
 */
int sv_rules_apply_ex(const sv_ruleset_t *rules, const char *in,
                      char *out, size_t out_size, unsigned opts,
                      unsigned *out_flags, size_t *out_consumed);

/* Provided by the generated translation unit (tools/extract_lang.py). */
extern const sv_ruleset_t sv_lang_data_eng;

/*
 * Provided by tools/extract_lang.py --language span, from an original
 * TISPAN32.DLL. The matcher in this file drives it unchanged: TISPAN32's own
 * matcher function is a separately-compiled copy of the same source (see
 * REVERSING.md, "the language modules are one code base"), disassembled and
 * differentially verified against sv_rules_apply_ex via Unicorn the same way
 * TIENG32's was -- see tools/verify_ruleset.py --language span. As of that
 * verification: 0 mismatches over 60,000+ real Spanish words and tens of
 * thousands of randomized stress strings (full run results in the
 * conversation/PR that added this, not restated here since it is a build-time
 * result, not a standing guarantee -- re-run the harness against your own
 * TISPAN32.DLL copy to confirm it still holds).
 *
 * Declared unconditionally like sv_lang_data_eng above: a build without
 * -DTISPEECH_SPAN_DLL simply never defines this symbol, so referencing it
 * without linking tispeech_lang_span is a link error, not a silent stub.
 */
extern const sv_ruleset_t sv_lang_data_span;

#ifdef __cplusplus
}
#endif
#endif /* TISPEECH_RULESET_H */
