/*
 * userdict.c — SoftVoice user dictionaries.
 *
 * PROVENANCE
 * ----------
 * The file format is the part of TIBASE32!_SVLoadUserDictionary@8
 * (0x1c0101a0) after its fopen/fread/fclose calls (this module does no file
 * I/O of its own — see userdict.h). The lookup is
 * TIENG32!FUN_1c2097a0, called from the per-word driver (0x1c2067c0) at the
 * point textphon_eng.c's lang_word() now calls sv_userdict_lookup(), before
 * the built-in exception dictionary (0x1c207230). Both are transliterated
 * from the disassembly and then pinned down empirically under
 * tools/sv_emu.py, which is how the entry format's H0/H1/H2 fields, the
 * sorted-bucket early exit, and the digit/default fallback buckets'
 * always-continue behaviour were actually determined — the disassembly
 * alone left which register survived which register-preserving call
 * ambiguous in places static reading did not resolve cleanly. See
 * userdict.h for the format itself and REVERSING.md for the verification
 * log.
 */

#include "tispeech/userdict.h"

#include <stdlib.h>
#include <string.h>

/* File layout (userdict.h): magic(4) + reserved(20) + enable(4) +
 * table_len(4) + 28 bucket offsets(4 each) = 144 bytes before the table. */
#define SV_UD_HEADER_LEN (4 + 20 + 4 + 4 + 28 * 4)
#define SV_UD_NBUCKETS   28

/* The character-class bit this module reads off the SAME table the letter-
 * to-sound rules use (ruleset.h's SV_CC_ALPHA / SV_CC_BIT1 — not included
 * here to keep this file's only dependency its own header, matching how
 * generator.c and smoothing.c stay independent of ruleset.h). */
#define SV_UD_CLASS_ALPHA 0x0080u
#define SV_UD_CLASS_DIGIT 0x0002u
#define SV_UD_CLASS_MASK  0x0083u

struct sv_userdict {
    uint32_t enabled;                 /* file offset 24: 0 == always miss */
    uint8_t *table;                   /* owned copy of the table blob */
    uint32_t table_len;               /* file offset 28 */
    uint32_t bucket[SV_UD_NBUCKETS];  /* offsets into table[] */
};

static uint32_t ud_rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

sv_userdict_status_t sv_userdict_parse(const uint8_t *bytes, size_t len,
                                        sv_userdict_t **out)
{
    sv_userdict_t *d;
    uint32_t table_len;
    size_t i;

    if (!out)
        return SV_USERDICT_E_BADARG;
    *out = NULL;
    if (!bytes)
        return SV_USERDICT_E_BADARG;
    if (len < SV_UD_HEADER_LEN)
        return SV_USERDICT_E_SHORT;
    if (memcmp(bytes, "SVXF", 4) != 0)
        return SV_USERDICT_E_MAGIC;

    table_len = ud_rd32le(bytes + 28);
    if (len - SV_UD_HEADER_LEN < table_len)
        return SV_USERDICT_E_SHORT;

    d = malloc(sizeof *d);
    if (!d)
        return SV_USERDICT_E_NOMEM;
    d->enabled = ud_rd32le(bytes + 24);
    d->table_len = table_len;
    d->table = NULL;
    if (table_len) {
        d->table = malloc(table_len);
        if (!d->table) {
            free(d);
            return SV_USERDICT_E_NOMEM;
        }
        memcpy(d->table, bytes + SV_UD_HEADER_LEN, table_len);
    }
    for (i = 0; i < SV_UD_NBUCKETS; i++)
        d->bucket[i] = ud_rd32le(bytes + 32 + 4 * i);

    *out = d;
    return SV_USERDICT_OK;
}

void sv_userdict_free(sv_userdict_t *dict)
{
    if (!dict)
        return;
    free(dict->table);
    free(dict);
}

static unsigned char ud_upper(unsigned char c)
{
    return (c >= 'a' && c <= 'z') ? (unsigned char)(c - 0x20) : c;
}

enum ud_result { UD_NOMATCH, UD_MATCH, UD_FULL };

/*
 * One bucket, starting at `bucket`. `*pp` is the cursor in the normalised
 * text (advanced past the word on a match); `src`/`in0` let a bit-0-set
 * entry (userdict.h) compare against the ORIGINAL text at the same relative
 * position instead, without threading a second cursor through the whole
 * scan. `fallback` is true for buckets 26/27 (digit-initial and default),
 * which never take the sorted-bucket early exit (userdict.h, "Lookup").
 */
static enum ud_result ud_scan_bucket(const sv_userdict_t *dict,
                                      const uint16_t cls[256],
                                      const uint8_t *bucket, int fallback,
                                      const char *src, const char *in0,
                                      const char **pp, char **out,
                                      int32_t *out_left, uint32_t *status)
{
    const uint8_t *e = bucket;

    for (;;) {
        uint32_t avail = dict->table_len - (uint32_t)(e - dict->table);
        uint8_t h0, h1, h2;
        const uint8_t *word;

        if (avail < 3)
            return UD_NOMATCH;
        h0 = e[0];
        h1 = e[1];
        h2 = e[2];
        word = e + 3;

        if (!(h2 & 0x02)) {
            /* Reserved slot (userdict.h): no comparable word. A zero first
             * payload byte stops the scan; anything else just skips it. */
            if (avail < 4 || word[0] == 0)
                return UD_NOMATCH;
            if (avail < 3u + h0)
                return UD_NOMATCH;
            e += 3u + h0;
            continue;
        }

        if (h1 == 0)
            return UD_NOMATCH;   /* bucket terminator */
        if (h0 < h1 || avail < 3u + h0)
            return UD_NOMATCH;   /* malformed entry: refuse, don't read OOB */

        {
            const char *base = (h2 & 0x01) ? src + (*pp - in0) : *pp;
            uint32_t k;
            int mismatch = -1;

            for (k = 0; k < h1; k++)
                if (word[k] != (uint8_t)base[k]) {
                    mismatch = (int)k;
                    break;
                }

            if (mismatch < 0) {
                /* All H1 bytes matched. A real match needs a word boundary
                 * right after — otherwise this entry is a strict prefix of
                 * a longer input word and does not count. */
                unsigned char nx = (unsigned char)base[h1];
                if (cls[nx] & SV_UD_CLASS_ALPHA) {
                    e += 3u + h0;
                    continue;
                }
                {
                    uint32_t phon_len = (uint32_t)h0 - h1;
                    if ((int32_t)phon_len > *out_left)
                        return UD_FULL;
                    if (phon_len)
                        memcpy(*out, word + h1, phon_len);
                    *out += phon_len;
                    **out = '\0';
                    *out_left -= (int32_t)phon_len;
                    *status = 2;   /* matches exceptions(): skip default_stress */
                    *pp += h1;
                    return UD_MATCH;
                }
            }

            if (fallback) {
                /* Digit/default buckets mix unrelated first characters, so
                 * there is no sort order to exploit: always keep scanning. */
                e += 3u + h0;
                continue;
            }
            /* Per-letter bucket: entries must be sorted ascending. Keep
             * scanning only while this entry still sorts at or before the
             * input (case-insensitively); once it sorts after, nothing
             * later in a well-formed bucket can match either. */
            if (ud_upper(word[mismatch]) <= ud_upper((unsigned char)base[mismatch])) {
                e += 3u + h0;
                continue;
            }
            return UD_NOMATCH;
        }
    }
}

/*
 * The lookup itself (userdict.h, "Lookup"): classify the character at *in,
 * pick a bucket, scan it, and on a match go around again from the new
 * cursor — one call can consume several consecutive dictionary words.
 * Returns 1 (output full, nothing changed for the word that didn't fit) or
 * 0 (zero or more words matched and committed; *in is at the first word
 * that wasn't found, or unchanged if none was).
 */
int sv_userdict_lookup(const sv_userdict_t *dict, const uint16_t cls[256],
                        const char *src, const char **in, char **out,
                        int32_t *out_left, uint32_t *status)
{
    const char *in0 = *in;
    const char *p = *in;

    if (!dict || !dict->enabled)
        return 0;

    for (;;) {
        unsigned char ch = (unsigned char)*p;
        uint32_t masked = cls[ch] & SV_UD_CLASS_MASK;
        uint32_t bucket_idx;
        int fallback;
        uint32_t table_off;
        enum ud_result r;

        if (masked == SV_UD_CLASS_ALPHA) {
            if (ch < 'A' || ch > 'Z') {
                /* userdict.h "Divergence": the original indexes its
                 * per-letter table out of bounds for the accented and
                 * lowercase-preserved Latin-1 letters that also carry this
                 * class bit. Not reproducible from the dictionary format
                 * alone; treated as "no bucket" instead. */
                *in = p;
                return 0;
            }
            bucket_idx = (uint32_t)(ch - 'A');
            fallback = 0;
        } else if (masked == SV_UD_CLASS_DIGIT) {
            bucket_idx = 26;
            fallback = 1;
        } else {
            bucket_idx = 27;
            fallback = 1;
        }

        table_off = dict->bucket[bucket_idx];
        if (table_off > dict->table_len) {
            *in = p;
            return 0;
        }

        r = ud_scan_bucket(dict, cls, dict->table + table_off, fallback,
                            src, in0, &p, out, out_left, status);
        if (r == UD_FULL) {
            *in = p;
            return 1;
        }
        if (r == UD_NOMATCH) {
            *in = p;
            return 0;
        }
        /* UD_MATCH: p was advanced past the matched word; try the next. */
    }
}
