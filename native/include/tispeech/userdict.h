/*
 * userdict.h — the SoftVoice user dictionary: SVLoadUserDictionary /
 * SVUnloadUserDictionary and how SVTextToPhon consults it.
 *
 * PROVENANCE
 * ----------
 * TIBASE32!_SVLoadUserDictionary@8 (0x1c0101a0) and
 * TIBASE32!_SVUnloadUserDictionary@8 (0x1c010450) parse the file and manage
 * the one dictionary slot a speech handle has. TIENG32!FUN_1c2097a0, called
 * from the per-word driver (0x1c2067c0) before the built-in exception
 * dictionary (0x1c207230), does the lookup. Every detail below was pinned
 * down empirically against the real DLLs under tools/sv_emu.py — see
 * REVERSING.md's "User dictionary" section for the verification log — because
 * the disassembly alone left several bit meanings ambiguous (which register
 * held what after which register-preserving call) in a way only running the
 * original resolved.
 *
 * FILE FORMAT (parsed from a caller-supplied buffer; this module does no
 * file I/O — SVLoadUserDictionary's fopen(path,"rb")/fread/fclose sequence is
 * the managed/path-aware layer's job, matching the project rule that
 * proprietary bytes and file access stay out of the reconstructed library)
 * -----------------------------------------------------------------------
 * All multi-byte fields are little-endian (native i386 byte order).
 *
 *   offset   size  field
 *   0        4     magic "SVXF"                          (0x1c014094)
 *   4        20    five reserved 4-byte fields, unvalidated by the original
 *                  beyond "this many bytes exist" — read and discarded
 *   24       4     enable flag: the dictionary is consulted only when this
 *                  is nonzero; zero makes SVLoadUserDictionary succeed but
 *                  every lookup miss (confirmed: a well-formed table with
 *                  this field zero behaves exactly like no dictionary)
 *   28       4     table_len: byte length of the table blob that follows
 *   32       112   28 x uint32 bucket offsets, each relative to the start of
 *                  the table blob:
 *                    index 0..25   first letter 'A'..'Z' (of the NORMALISED,
 *                                  i.e. upper-cased, word — original VA
 *                                  handle+0x30..handle+0x9c, indexed as
 *                                  handle+0x28 + ch*4 - 0xfc, so only valid
 *                                  for plain ASCII 'A'-'Z')
 *                    index 26      first character is a digit '0'-'9'
 *                                  (original handle+0x98)
 *                    index 27      everything else — punctuation, space,
 *                                  and the accented/Latin-1 letters that
 *                                  also carry the alphabetic class bit but
 *                                  fall outside 'A'-'Z' (original
 *                                  handle+0x9c; see "Divergences" below)
 *   144      table_len   the table blob itself, a sequence of entries per
 *                  bucket (see below), sorted ascending within buckets
 *                  0..25 (required — see "Bucket scan" below); buckets 26
 *                  and 27 need not be sorted, since the original never
 *                  applies the early-exit that would require it.
 *
 * ENTRY FORMAT (inside the table blob)
 * -------------------------------------
 *   byte 0   H0   byte offset, from the start of this entry's word field
 *                 (byte 3), to the next entry's header. Entry size is
 *                 therefore 3 + H0.
 *   byte 1   H1   length in bytes of the word field. H1 == 0 marks the end
 *                 of a bucket: scanning stops there unconditionally.
 *   byte 2   H2   flags:
 *                   bit 0 (0x01)  which text the word field is compared
 *                                 against: 0 = the NORMALISED (upper-cased)
 *                                 text, 1 = the ORIGINAL, case-preserved
 *                                 text. Confirmed both ways: an entry
 *                                 written in mixed case only matches when
 *                                 this bit is set and the input's original
 *                                 spelling matches exactly.
 *                   bit 1 (0x02)  entry kind: 1 = an ordinary word entry
 *                                 (below); 0 = an inert/reserved slot with
 *                                 no comparable word — see "Reserved slots".
 *   bytes [3, 3+H1)      word: H1 bytes, compared case-sensitively.
 *   bytes [3+H1, 3+H0)   phonemes: H0-H1 bytes, appended verbatim to the
 *                        output on a match. Conventionally starts with a
 *                        space, the same convention the built-in exception
 *                        and number tables use, because nothing else
 *                        inserts a separator between two dictionary words
 *                        matched back to back (see "Lookup", below).
 *
 * LOOKUP (TIENG32!FUN_1c2097a0, called once per lang_word() iteration,
 * BEFORE the built-in exception dictionary, numbers and letter-to-sound
 * rules — this is the front of the pipe, not a fallback)
 * ---------------------------------------------------------------------
 * The dictionary is consulted once per call to lang_word(), i.e. once at the
 * position sv_text_to_phon's outer per-word loop is currently at. Internally
 * it loops: on every successful match it re-classifies the character now at
 * the cursor and keeps going, so ONE call can consume several consecutive
 * dictionary words (confirmed: "HELLO WORLD", both entries present,
 * produces both phoneme strings from a single lookup pass). It stops and
 * returns cleanly the moment a word isn't found, leaving the cursor at that
 * word so the normal pipeline (exceptions, numbers, rules) handles it.
 *
 * Per word: classify the byte at the cursor (the SAME character-class table
 * driving the letter-to-sound rules) to pick a bucket, then scan that
 * bucket's entries in order:
 *
 *   - H1 == 0 (or, for a reserved slot, its first payload byte == 0): stop.
 *     No match; the dictionary lookup for this call ends here.
 *   - a reserved slot (H2 bit 1 clear) with a nonzero first payload byte:
 *     skip it (advance by 3+H0) and keep scanning.
 *   - an ordinary word entry: compare H1 bytes case-sensitively against the
 *     chosen text (H2 bit 0). If they all match AND the character right
 *     after the compared span is not alphabetic (a real word boundary):
 *     MATCH — append the phoneme bytes, advance the cursor past the word,
 *     set status the same way the built-in exceptions do (so
 *     default_stress() is skipped — dictionary entries carry their own
 *     stress), and go around the outer loop again. If they match but the
 *     next character IS alphabetic (this entry is a strict prefix of a
 *     longer input word), or if they mismatch: keep scanning (advance by
 *     3+H0), UNLESS this is a plain letter bucket (0..25) and the mismatch
 *     shows the entry now sorts after the input (a case-insensitive
 *     ordering test) — then stop; nothing later in a correctly sorted
 *     bucket could match either. Digit and default buckets (26, 27) never
 *     take this early exit, since they interleave unrelated first
 *     characters and have no meaningful sort order to exploit — confirmed:
 *     an out-of-order pair in bucket 27 still finds the second entry, while
 *     the same pair in a letter bucket does not.
 *
 * If the phoneme bytes for a match would not fit in the caller's remaining
 * output space, the WHOLE lookup stops immediately and reports "buffer
 * full", leaving every field exactly as it was for the word that didn't fit
 * (matching the convention sv_rules_step / exceptions() / numbers() use
 * elsewhere in textphon_eng.c) — words already matched earlier in the same
 * call stay committed.
 *
 * DIVERGENCE (deliberate, documented rather than reproduced)
 * ------------------------------------------------------------
 * The original's per-letter bucket index is `handle+0x28 + ch*4 - 0xfc`,
 * valid only for ch in 'A'..'Z'; the SAME classification bit (alphabetic)
 * is also set for nine accented uppercase and six lowercase Latin-1 letters
 * the front end's normalisation preserves unchanged (0xc1 0xc4 0xc9 0xcd
 * 0xd1 0xd3 0xd6 0xda 0xdc 0xdf, 0xe1 0xe9 0xed 0xf1 0xf3 0xfa), for which
 * the same formula computes an index far outside the 28-entry table — an
 * out-of-bounds read in the original, into whatever follows the speech
 * handle. That is not part of the dictionary format and cannot be
 * meaningfully reconstructed (it depends on unrelated heap contents, not
 * anything a dictionary file controls), so sv_userdict_lookup() treats a
 * word starting with one of those letters as "no dictionary bucket" and
 * defers to the normal pipeline instead.
 *
 * LOAD / UNLOAD (TIBASE32!_SVLoadUserDictionary@8 / _SVUnloadUserDictionary@8)
 * ------------------------------------------------------------------------
 * One dictionary slot per speech handle, not a list. SVLoadUserDictionary
 * unconditionally unloads whatever is already loaded before parsing the new
 * file (confirmed: loading a second dictionary without an explicit unload
 * replaces the first). SVUnloadUserDictionary IGNORES its path argument
 * entirely — confirmed by passing it a string that names nothing loaded and
 * observing the dictionary unload anyway — it simply frees and clears
 * whatever is currently loaded, and is a harmless no-op when nothing is.
 * Neither function is reconstructed here: this header covers the file
 * FORMAT and the LOOKUP only. sv_userdict_parse() below is the format half
 * of SVLoadUserDictionary (everything after its fopen/fread), taking the
 * whole file's bytes already in memory rather than a path, per the project
 * rule that this library does no file I/O of its own.
 */

#ifndef TISPEECH_USERDICT_H
#define TISPEECH_USERDICT_H

#include <stddef.h>
#include <stdint.h>

#include "tispeech/narrate.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sv_userdict sv_userdict_t;

typedef enum {
    SV_USERDICT_OK       = 0,
    SV_USERDICT_E_BADARG = -1,  /* NULL args, or out==NULL */
    SV_USERDICT_E_SHORT  = -2,  /* truncated header or table; matches the
                                  * original's 0x1b6b (short fread) */
    SV_USERDICT_E_MAGIC  = -3,  /* not an "SVXF" dictionary; matches the
                                  * original's 0x1b6c (bad magic) */
    SV_USERDICT_E_NOMEM  = -4,  /* allocation failure; matches 0x1b5a */
} sv_userdict_status_t;

/*
 * Parses `bytes[0, len)` — a whole dictionary file's contents, exactly as
 * read from disk — into a heap-owned dictionary object. Never touches the
 * filesystem: the caller (a path-aware layer, e.g. the managed side) reads
 * the file and hands over its bytes. On any status other than
 * SV_USERDICT_OK, *out is left NULL.
 */
sv_userdict_status_t sv_userdict_parse(const uint8_t *bytes, size_t len,
                                        sv_userdict_t **out);

/* Releases a dictionary from sv_userdict_parse(). NULL is a no-op. */
void sv_userdict_free(sv_userdict_t *dict);

/*
 * The lookup itself: TIENG32!FUN_1c2097a0, as called from lang_word() in
 * textphon_eng.c. `cls` is the same 256-entry character-class table the
 * letter-to-sound rules use. `src` is ctx->src as of the start of the
 * current word; `*in` is ctx->in, advanced past every word matched (zero or
 * more — a single call can consume several consecutive dictionary words,
 * see userdict.h "Lookup"); `*out`/`*out_left`/`*status` are ctx->out,
 * ctx->out_left and ctx->status, updated the same way exceptions() and
 * sv_rules_step() do. `dict` may be NULL (immediate no-op, returns 0 with
 * nothing changed). Returns 1 if the output buffer filled up partway
 * through a match, with every field exactly as it was for the word that
 * didn't fit (nothing from before that word is undone); 0 otherwise.
 */
int sv_userdict_lookup(const sv_userdict_t *dict, const uint16_t cls[256],
                       const char *src, const char **in, char **out,
                       int32_t *out_left, uint32_t *status);

/*
 * As sv_text_to_phon() (narrate.h), but consults `dict` first at the exact
 * point TIENG32's word driver calls FUN_1c2097a0 — before the built-in
 * exception dictionary, numbers and letter-to-sound rules. `dict` may be
 * NULL, in which case this is byte-identical to sv_text_to_phon(), which is
 * implemented as sv_text_to_phon_ex(m, text, out, out_size, flags, NULL).
 */
int32_t sv_text_to_phon_ex(const sv_langmod *m, const char *text, char *out,
                           int32_t out_size, uint32_t flags,
                           const sv_userdict_t *dict);

/* As sv_tts_phonemes() (narrate.h): SVTTS's buffer-retry loop, with every
 * SVTextToPhon call consulting `dict`. NULL gives sv_tts_phonemes(). */
int32_t sv_tts_phonemes_ex(const sv_langmod *m, const char *text,
                           uint32_t flags, const sv_userdict_t *dict,
                           char **phonemes);

#ifdef __cplusplus
}
#endif
#endif /* TISPEECH_USERDICT_H */
