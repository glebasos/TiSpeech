/*
 * ruleset.c — faithful reconstruction of TIENG32.DLL!FUN_1c206860,
 *             the SoftVoice letter-to-sound rule matcher.
 *
 * PROVENANCE
 * ----------
 * Ghidra decompilation of TIENG32.DLL at VA 0x1C206860, cross-checked against
 * `llvm-objdump -d`. Helpers FUN_1c207180 (accent test) and FUN_1c2070b0
 * (accent stripping) at 0x1C207180 / 0x1C2070B0 are reconstructed inline as
 * sv_is_accented() / sv_strip_accent().
 *
 * Control flow has been restructured from the decompiler's goto soup into
 * ordinary loops. The restructuring is behaviour-preserving; every branch is
 * accounted for. Deviations, if any are ever found, are bugs — file them
 * against REVERSING.md rather than "fixing" them to taste, because bit-exact
 * agreement with the original is the whole point.
 *
 * NOT reconstructed here: text normalisation (numbers, abbreviations), the
 * user dictionary, and the phoneme-to-parameter stages. See REVERSING.md.
 */

#include "tispeech/ruleset.h"

#include <string.h>

#define CH_LBRACKET 0x5bu
#define CH_RBRACKET 0x5du
#define CH_BACKSLASH 0x5cu
#define CH_BACKTICK 0x60u
#define CH_EQUALS 0x3du

/* FUN_1c207180 — true for the accented Latin-1 letters the ruleset spells
 * out literally. When a RULE character is one of these, the input character is
 * compared verbatim instead of being accent-stripped first. */
static int sv_is_accented(unsigned char c)
{
    switch (c) {
    case 0xc1: case 0xc9: case 0xcd: case 0xd1: case 0xd3: case 0xda:
    case 0xe1: case 0xe9: case 0xed: case 0xf1: case 0xf3: case 0xfa:
    case 0xfc:
        return 1;
    default:
        return 0;
    }
}

/* FUN_1c2070b0 — strip the accent off a vowel. Guarded by the vowel bit, so a
 * non-vowel is returned untouched even if it happens to be one of the cases. */
static unsigned char sv_strip_accent(const sv_ruleset_t *rs, unsigned char c)
{
    if ((rs->charclass[c] & SV_CC_VOWEL) == 0)
        return c;
    switch (c) {
    case 0xc1: case 0xe1: return 0x41; /* A */
    case 0xc9: case 0xe9: return 0x45; /* E */
    case 0xcd: case 0xed: return 0x49; /* I */
    case 0xd1: case 0xf1: return 0x4e; /* N */
    case 0xd3: case 0xf3: return 0x4f; /* O */
    case 0xda: case 0xfa: case 0xfc: return 0x55; /* U */
    default: return c;
    }
}

static int sv_isspace(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/* Compare one rule character against one input character, applying the
 * accent-stripping rule above. */
static int sv_chreq(const sv_ruleset_t *rs, unsigned char rule_ch, unsigned char in_ch)
{
    if (!sv_is_accented(rule_ch))
        in_ch = sv_strip_accent(rs, in_ch);
    return rule_ch == in_ch;
}

/* Advance past the end of the current rule, leaving the cursor just after its
 * terminator. Mirrors the decompiler's repeated
 * `while (*p != '\\') { if (*p == '`') break; p++; }` tails. */
static const unsigned char *sv_skip_rule(const unsigned char *p)
{
    while (*p != CH_BACKSLASH && *p != CH_BACKTICK) {
        if (*p == 0)
            return p;
        p++;
    }
    return p;
}

/*
 * Lowest address any bucket points at, i.e. the first byte of the rule blob.
 *
 * In the original the blob is not the start of anything: it sits inside
 * TIENG32's data section at 0x1C209E22 (which is also bucket 'A', the lowest
 * of the 26 bucket pointers), and the two bytes below it, 0x1C209E20 and
 * 0x1C209E21, are both '\'. Those are sentinels. They exist because a LEFT
 * context is matched walking backwards and is bounded by the previous rule's
 * terminator — the blob's very first rule has no previous rule, so the table
 * author put a terminator there by hand.
 *
 * tools/extract_lang.py starts its blob at min(buckets, fallback), so the
 * sentinel is not in our copy and a naive backwards walk would read
 * rule_blob[-1]. Treating "below the floor" as a terminator reproduces the
 * sentinel exactly: hitting '\' makes sv_match_context succeed, and so does
 * this. Without it, ` [A.] =EY5` ` — the first rule in the English blob —
 * cannot fire, and "A." comes out as " EY5." instead of the original's "EY5".
 * Found by tools/verify_ruleset.py.
 */
static const unsigned char *sv_blob_floor(const sv_ruleset_t *rs)
{
    const unsigned char *lo = rs->fallback;
    int i;

    if (rs->buckets != NULL) {
        for (i = 0; i < 256; i++) {
            const unsigned char *b = rs->buckets[i];
            if (b != NULL && (lo == NULL || b < lo))
                lo = b;
        }
    }
    return lo;
}

/*
 * Read one input byte without ever dereferencing outside [floor, ceil].
 *
 * The original always operates on a full sentence buffer, so walking off
 * either end of the current word still lands on real memory: whitespace the
 * text normaliser left between words on the left, and more sentence text (or
 * at minimum a zeroed, generously sized working buffer) on the right. Our
 * callers can hand us a bare word with no slack on either side -- ASan
 * caught both directions: a left-context walk reading in_floor[-1] for a
 * buffer that starts exactly at the word, and the word-boundary loops below
 * (' ' and 'b') reading one byte past the word's own NUL terminator, because
 * they always step once before checking for more whitespace.
 *
 * We cannot manufacture the original's memory, so we manufacture the SHAPE
 * every caller who does pad correctly already presents, on both ends:
 *
 *   - below floor: exactly one boundary space at floor - 1 (this word has no
 *     known predecessor, indistinguishable from "this word starts a
 *     sentence"), then NUL beyond that. Returning ' ' for every position
 *     below floor -- not just the first -- would make the whitespace-skip
 *     loops spin forever, since sv_isspace(' ') is always true and there is
 *     no real byte out there to eventually break on.
 *   - above ceil (the input's own NUL terminator, computed once as
 *     in_floor + strlen(in)): NUL. The terminator itself is always safe to
 *     read; only what would come after it is synthesised.
 *
 * This is exactly the shape tools/verify_ruleset.py's frame() already
 * builds (one real space, then NUL padding, on both sides), so it changes
 * nothing for a caller who already provides it.
 */
static unsigned char sv_peek(const unsigned char *floor, const unsigned char *ceil,
                             const unsigned char *ip)
{
    if (floor != NULL && ip < floor)
        return (ip == floor - 1) ? (unsigned char)' ' : (unsigned char)'\0';
    if (ceil != NULL && ip > ceil)
        return (unsigned char)'\0';
    return *ip;
}

/*
 * Verify one context side.
 *   rp        rule cursor, already positioned on the first context character
 *   ip        input cursor
 *   stride    -1 for the left context, +1 for the right context
 *   rule_floor first byte of the rule blob; one below it stands in for the
 *             original's '\' sentinel (see sv_blob_floor). Only the
 *             backwards walk over the RULE side can reach it.
 *   in_floor  first byte of the caller's input buffer (sv_rules_apply_ex's
 *             `in`). Only the backwards walk over the INPUT side can reach
 *             it; see sv_peek.
 *   in_ceil   address of the caller's input NUL terminator. Only the
 *             forwards walk over the INPUT side can read past it; see
 *             sv_peek.
 * Returns 1 on match (with *rp_end left on the terminator that stopped it),
 * 0 on failure.
 */
static int sv_match_context(const sv_ruleset_t *rs,
                            const unsigned char *rp, const unsigned char *ip,
                            int stride, const unsigned char *rule_floor,
                            const unsigned char *in_floor,
                            const unsigned char *in_ceil,
                            const unsigned char **rp_end)
{
    const unsigned short *cc = rs->charclass;

    for (;;) {
        unsigned char rc;

        if (stride < 0 && rule_floor != NULL && rp < rule_floor) {
            *rp_end = rp; /* the sentinel below the blob; never dereferenced */
            return 1;
        }
        rc = *rp;
        if (rc == CH_BACKSLASH || rc == CH_BACKTICK || rc == CH_EQUALS) {
            *rp_end = rp;
            return 1;
        }

        if ((cc[rc] & SV_CC_METACHAR) == 0) {
            /* Literal character. */
            if (!sv_chreq(rs, rc, sv_peek(in_floor, in_ceil, ip)))
                return 0;
            ip += stride;
        } else {
            unsigned char c = sv_peek(in_floor, in_ceil, ip);
            switch (rc) {
            case ' ': /* word boundary, then skip whitespace */
                if ((cc[c] & SV_CC_ALPHA) != 0)
                    return 0;
                do {
                    ip += stride;
                } while (sv_isspace(sv_peek(in_floor, in_ceil, ip)));
                break;

            case '#': /* one vowel */
                if ((cc[c] & SV_CC_VOWEL) == 0)
                    return 0;
                ip += stride;
                break;

            case '%': { /* a suffix: ER / E / ELY / ES / ED / ING (+ opt. S) */
                /* Always reads forward of ip regardless of stride: for the
                 * left-context walk (ip decreasing) that is back toward
                 * positions already established as inside the buffer, so no
                 * in_floor guard is needed here. */
                int n;
                if (c == 'E') {
                    unsigned char c1 = ip[1];
                    if (c1 == 'R') {
                        n = 3 - 1 + (ip[2] == 'S');
                    } else if (c1 == 'L' && ip[2] == 'Y') {
                        n = 3;
                    } else if (c1 == 'S' || c1 == 'D') {
                        n = 2;
                    } else {
                        n = 1;
                    }
                } else if (c == 'I' && ip[1] == 'N' && ip[2] == 'G') {
                    n = 4 - 1 + (ip[3] == 'S');
                } else {
                    return 0;
                }
                ip += n;
                if ((cc[sv_peek(in_floor, in_ceil, ip)] & SV_CC_ALPHA) != 0)
                    return 0;
                break;
            }

            case '&': /* sibilant */
                if (c == 'H') {
                    unsigned char prev = sv_peek(in_floor, in_ceil, ip - 1);
                    if (prev != 'C' && prev != 'S')
                        return 0;
                    ip += stride * 2;
                } else {
                    if ((cc[c] & SV_CC_SIBILANT) == 0)
                        return 0;
                    ip += stride;
                }
                break;

            case '+': /* front vowel */
                if ((cc[c] & SV_CC_FRONTVOW) == 0)
                    return 0;
                ip += stride;
                break;

            case '.': /* voiced consonant */
                if ((cc[c] & SV_CC_VOICED) != SV_CC_VOICED)
                    return 0;
                ip += stride;
                break;

            case ':': /* zero or more consonants */
                while ((cc[sv_peek(in_floor, in_ceil, ip)] & SV_CC_CONSONANT) != 0)
                    ip += stride;
                break;

            case '<':
                if (sv_isspace(c) || (cc[c] & (SV_CC_BIT0 | SV_CC_BIT1)) == 0)
                    return 0;
                ip += stride;
                break;

            case '>':
                if ((cc[c] & SV_CC_BIT0) != 0)
                    return 0;
                ip += stride;
                break;

            case '?':
                if ((cc[c] & SV_CC_BIT1) == 0)
                    return 0;
                ip += stride;
                break;

            case '@': /* consonant that palatalises a following U */
                if (c == 'H') {
                    unsigned char prev = sv_peek(in_floor, in_ceil, ip - 1);
                    if (prev != 'T' && prev != 'C' && prev != 'S')
                        return 0;
                    ip += stride * 2;
                } else {
                    if ((cc[c] & SV_CC_ATSET) == 0)
                        return 0;
                    ip += stride;
                }
                break;

            case '^': /* one consonant */
                if ((cc[c] & SV_CC_CONSONANT) == 0)
                    return 0;
                ip += stride;
                break;

            case 'b': /* one or more whitespace */
                if (!sv_isspace(c))
                    return 0;
                do {
                    ip += stride;
                } while (sv_isspace(sv_peek(in_floor, in_ceil, ip)));
                break;

            default:
                return 0;
            }
        }
        rp += stride;
    }
}

int sv_rules_apply_ex(const sv_ruleset_t *rs, const char *in,
                      char *out, size_t out_size, unsigned opts,
                      unsigned *out_flags, size_t *out_consumed)
{
    const unsigned short *cc    = rs->charclass;
    const unsigned char  *p     = (const unsigned char *)in;
    const unsigned char  *floor = sv_blob_floor(rs);
    const unsigned char  *in_floor = (const unsigned char *)in;
    const unsigned char  *in_ceil  = in_floor + strlen(in);
    char                 *op    = out;
    size_t                out_left;
    int                   used_fallback = 0;
    unsigned              last_flags = 0;

    if (out_flags != NULL)
        *out_flags = 0;
    if (out_consumed != NULL)
        *out_consumed = 0;
    if (out_size == 0)
        return 1;
    *op = '\0';
    out_left = out_size - 1;

    for (;;) {
        unsigned char c = *p;
        const unsigned char *bucket;

        if (c == ' ' || c == '\0')
            break;
        if ((cc[c] & SV_CC_BIT1) != 0)
            break;
        /* The original latches a flag the first time it falls back to the
         * catch-all bucket and stops on the following iteration. */
        if (used_fallback)
            break;

        if ((cc[c] & SV_CC_ALPHA) == 0) {
            bucket = rs->fallback;
            used_fallback = 1;
        } else if (c < 0x7b && rs->buckets[c] != NULL) {
            bucket = rs->buckets[c];
        } else if (rs->buckets[c] != NULL) {
            bucket = rs->buckets[c]; /* 0xC4 / 0xD6 / 0xDC / 0xDF */
        } else {
            bucket = rs->fallback;
            used_fallback = 1;
        }

        /* Walk the bucket looking for a rule whose literal, left context and
         * right context all match at the cursor. */
        {
            const unsigned char *rp = bucket;
            int matched = 0;

            while (*rp != '\0' && !matched) {
                const unsigned char *lb, *lit, *ip, *rend;
                const unsigned char *q;

                while (*rp != CH_LBRACKET) {
                    if (*rp == '\0')
                        goto bucket_done;
                    rp++;
                }
                lb = rp;          /* on '['            */
                lit = rp + 1;     /* first literal char */
                q = p;

                /* Literal run. */
                while (*lit != CH_RBRACKET) {
                    if (*lit == '\0')
                        goto bucket_done;
                    if (!sv_chreq(rs, *lit, *q))
                        break;
                    lit++;
                    q++;
                }
                if (*lit != CH_RBRACKET) {
                    /* Mismatch: resume the '[' scan after the failure point,
                     * which lands on the next rule. */
                    rp = lit + 1;
                    continue;
                }
                if (lit[-1] == CH_LBRACKET)
                    q++; /* empty [] consumes one input character */

                /* Left context, walking backwards from just before '['. */
                ip = p - 1;
                if (!sv_match_context(rs, lb - 1, ip, -1, floor, in_floor, in_ceil, &rend)) {
                    rp = sv_skip_rule(lit);
                    if (*rp == '\0')
                        goto bucket_done;
                    rp++;
                    continue;
                }
                /* Right context, walking forwards from just after ']'. */
                if (!sv_match_context(rs, lit + 1, q, +1, floor, in_floor, in_ceil, &rend)) {
                    rp = sv_skip_rule(lit);
                    if (*rp == '\0')
                        goto bucket_done;
                    rp++;
                    continue;
                }
                /* rend is on the '=' that ends the right context. */
                {
                    const unsigned char *o = rend + 1;
                    const unsigned char *scan = o;
                    unsigned flags = SV_RF_INITIAL;
                    size_t n = 0;

                    for (;;) {
                        unsigned char oc = *scan;
                        if (oc == CH_BACKSLASH) { flags |= SV_RF_BACKSLASH; break; }
                        if (oc == CH_BACKTICK)  { flags |= SV_RF_BACKTICK;  break; }
                        if (oc == '\0') break;
                        if (oc == '.' || oc == '?')
                            flags |= SV_RF_SENTENCE;
                        n++;
                        scan++;
                    }

                    if (n > out_left)
                        return 1;

                    if ((opts & 0x40u) == 0 && *p == '?' && n == 1) {
                        *op++ = (char)rs->qmark_sub;
                        out_left -= 1;
                    } else {
                        memcpy(op, o, n);
                        op += n;
                        out_left -= n;
                    }
                    *op = '\0';
                    last_flags = flags;
                }

                p = q; /* consume the literal */
                matched = 1;
            }
        bucket_done:
            if (!matched)
                break; /* no rule fired; the original also stops here */
        }
    }

    *op = '\0';
    if (out_flags != NULL)
        *out_flags = last_flags;
    if (out_consumed != NULL)
        *out_consumed = (size_t)((const char *)p - in);
    return 0;
}

int sv_rules_apply(const sv_ruleset_t *rs, const char *in,
                   char *out, size_t out_size, unsigned opts)
{
    return sv_rules_apply_ex(rs, in, out, out_size, opts, NULL, NULL);
}
