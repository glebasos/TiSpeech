/*
 * textphon_span.c — the Spanish text front end: TIBASE32's SVTextToPhon and
 * TISPAN32's word translator (module vtable +0x0C, 0x1C406250).
 *
 * PROVENANCE
 * ----------
 * Written by disassembling TISPAN32.DLL and comparing every function against
 * its TIENG32 counterpart, capstone mnemonic-plus-operand-shape with
 * immediates normalised (the same technique REVERSING.md's vtable comparison
 * uses). The entry point at 0x1C406250 is 100% instruction-identical to
 * TIENG32 0x1c2067c0 (already ported in src/textphon_eng.c as `lang_word()`);
 * so are the functions it calls for everything BUT numbers and default
 * stress:
 *
 *   TISPAN32 0x1C406250  vtable +0x0C           per-word driver
 *   TISPAN32 0x1C406cc0  exception dictionary matcher      (== TIENG32 0x1c207230)
 *   TISPAN32 0x1C407e20  number/token normaliser            (== TIENG32 0x1c208150,
 *                        up to its own pattern-dispatch table, then diverges)
 *   TISPAN32 0x1C409400  digit spelling                     (== TIENG32 0x1c209580)
 *   TISPAN32 0x1C408520  "h:mm" / "hh:mm"                   (== TIENG32 0x1c208850)
 *   TISPAN32 0x1C408750  "m/d/yy" dates                     (== TIENG32 0x1c208a80)
 *   TISPAN32 0x1C408960  "h:mm:ss" durations                (== TIENG32 0x1c208c90)
 *   TISPAN32 0x1C408bf0  "(###)" area code                  (== TIENG32 0x1c208f20)
 *   TISPAN32 0x1C408cc0  "###-####" local numbers           (== TIENG32 0x1c208ff0)
 *   TISPAN32 0x1C408df0  "#-###-###-####" long distance      (== TIENG32 0x1c209120)
 *   TISPAN32 0x1C408ec0  "###-###-####" / "###/###-####"     (== TIENG32 0x1c2091f0)
 *   TISPAN32 0x1C409620  user dictionary lookup (== TIENG32 0x1c2097a0,
 *                        both call src/userdict.c's sv_userdict_lookup())
 *   TISPAN32 0x1C409880  spell mode                          (== TIENG32 0x1c209a00)
 *   TISPAN32 0x1C4062F0  letter-to-sound rules: src/ruleset.c, sv_lang_data_span
 *
 * Confirmed instruction-identical up to and including their own pattern-
 * dispatch/handler tables; what differs between the two languages is only
 * the embedded literal addresses (data), which this file cites at TISPAN32's
 * OWN real virtual addresses -- see tools/extract_span_frontend.py for how
 * those addresses end up inside the SAME mutable .data copy the frame
 * generator (src/langgen.c) uses (one sv_langgen instance per module).
 *
 * Two functions genuinely differ and are NOT ported here:
 *
 *   TISPAN32 0x1C4073D0  cardinal number -> Spanish words (== TIENG32 0x1c207890,
 *                        n2w; diverges at TISPAN32's own instruction 4 --
 *                        Spanish cardinal-number grammar -- gender agreement,
 *                        irregular "cien"/"ciento", apocope, "y" insertion --
 *                        is a different algorithm, not a table swap)
 *   TISPAN32 0x1C408f60  three-digit group reading (== TIENG32 0x1c209290;
 *                        diverges at instruction 1 -- Spanish's irregular
 *                        hundreds "cien"/"doscientos".../"novecientos")
 *   TISPAN32 0x1C4090F0  "####" as a year (== TIENG32 0x1c209380; the leading
 *                        zero-digit case matches TIENG32's spell_digits() path
 *                        instruction for instruction, but the cardinal-number
 *                        construction after it needs n2w's machinery)
 *
 * Since almost every remaining number handler (h_time, h_date, h_duration,
 * h_local, h_long_distance, h_area_code) calls the shared three-digit reader
 * and/or n2w internally, NOTHING that depends on cardinal-number formation
 * is reconstructed. `numbers_span()` below reproduces the ORIGINAL's own
 * "is this token numeric-shaped" gate (itself proven shared code -- the
 * TISPAN32/TIENG32 diff matches for the first ~15 instructions of
 * TIENG32 0x1c208150 before it even looks at the pattern table) and returns
 * SV_NAR_E_NOTIMPL rather than emitting anything for a numeric token: no
 * plausible-looking wrong output, an honest "not yet".
 *
 * default_stress_span() (TISPAN32 0x1C407240, == TIENG32 0x1c2077b0 in
 * shape only -- diverges at instruction 0) IS reconstructed: Spanish default
 * stress follows the actual language rule -- a word whose last letter (in
 * the OUTPUT PHONEME STRING, skipping any already-placed stress digit) is a
 * vowel-class code, 'N' or 'S' takes stress on the vowel before the last one
 * (llana/paroxytone); anything else takes stress on the last vowel
 * (aguda/oxytone). See the function for the exact table-driven test the
 * disassembly performs, address-cited at each step.
 *
 * TABLE ADDRESSES (TISPAN32, confirmed by diff against TIENG32)
 * ---------------------------------------------------------------
 *   exception dictionary buckets   0x1C40E2C4  (== TIENG32 0x1c24c7cc)
 *   exception fallback (vowel-ish) 0x1C40E430  (== TIENG32 0x1c24c938)
 *   exception fallback (general)   0x1C40E434  (== TIENG32 0x1c24c93c)
 *   default-stress ending table    0x1C40E439  (8 x 2-byte entries)
 *   three_digits' "cien"           0x1C40E784  (not ported: see above)
 *   three_digits' "-cientos"       0x1C40E774  (not ported: see above)
 *   spell-mode per-char table      0x1C40EF58  (8-byte stride x256)
 *   spell '.', digit follows       0x1C40F360
 *   spell '.', otherwise           0x1C40F358
 *   spell '?', otherwise           0x1C40EE84
 *   char classes                   0x1C40E23C bucket base / 0x1C409AA0 classes
 *                                  (== sv_lang_data_span, already extracted
 *                                  and verified by tools/extract_lang.py)
 *
 * These, and the exception-dictionary rule text they point into, live inside
 * the SAME merged .data image src/langgen.c uses (tools/extract_span_frontend.py):
 * TISPAN32's own .text/.data are copied verbatim at their OWN real addresses
 * into that image, so a pointer TISPAN32's tables hold is already correct --
 * nothing here needs a coordinate translation the way src/langgen.c's TIENG32
 * literals do (see that file's header). rd32()/str_at() below read straight
 * out of that copy exactly as textphon_eng.c's do.
 *
 * The "{{...}}" inline-command passthrough IS reconstructed (TIBASE32-level,
 * language-independent, duplicated verbatim from textphon_eng.c's
 * `passthrough()`). The user dictionary (TISPAN32 0x1C409620) is wired the
 * same way textphon_eng.c's lang_word() calls sv_userdict_lookup() at
 * TIENG32 0x1c2097a0 -- same shared code, same call site, Spanish's own
 * character-class table (`fe->cls`, == sv_lang_data_span.charclass). With no
 * dictionary attached (`dict == NULL`) it is an unconditional no-op, exactly
 * matching the original when none is loaded.
 */

#include "langmod_priv.h"
#include "tispeech/ruleset.h"
#include "tispeech/textphon_span.h"
#include "tispeech/userdict.h"

#include <stdlib.h>
#include <string.h>

/* The original's context block, TIBASE32 0x1c0141e0 -- language independent,
 * identical layout to textphon_eng.c's tp_ctx. */
typedef struct {
    const char *src;
    char *out;
    char *in;
    char *base;
    char *word;
    int32_t out_left;
    uint32_t opts;
    uint32_t status;
} tp_ctx;

typedef struct {
    sv_langgen *g;
    tp_ctx c;
    const uint16_t *cls;   /* sv_lang_data_span.charclass, 256 entries */
    sv_ruleset_t rules;    /* == sv_lang_data_span (LTS matcher tables) */
    const sv_userdict_t *dict;  /* optional; NULL = none loaded */
} fe_t;

#undef G
#define G (fe->g)

static unsigned ct(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return 0x101 | (c <= 'F' ? 0x80 : 0);
    if (c >= 'a' && c <= 'z')
        return 0x102 | (c <= 'f' ? 0x80 : 0);
    if (c >= '0' && c <= '9')
        return 0x84;
    if (c == ' ')
        return 0x48;
    if (c == '\t')
        return 0x68;
    if (c >= '\n' && c <= '\r')
        return 0x28;
    if (c < 0x20 || c == 0x7f)
        return 0x20;
    if (c < 0x7f)
        return 0x10;
    return 0;
}

static int c_toupper(int c) { return c >= 'a' && c <= 'z' ? c - 0x20 : c; }
static int c_tolower(int c) { return c >= 'A' && c <= 'Z' ? c + 0x20 : c; }

/* A 32-bit word of the merged .data image, or 0 outside it. */
static uint32_t rd32(const fe_t *fe, uint32_t va)
{
    if (va - G->dva > G->dsize - 4)
        return 0;
    return sv_rd32(G->d + (va - G->dva));
}

/* The string or rule text at an original VA, inside the same merged image
 * (see the file header: TISPAN32's own .text/.data are copied into it
 * verbatim at their real addresses, so this alone is enough -- no separate
 * .text-segment lookup is needed the way textphon_eng.c's str_at() falls
 * back to one). */
static const char *str_at(const fe_t *fe, uint32_t va)
{
    if (va - G->dva < G->dsize)
        return (const char *)(G->d + (va - G->dva));
    return "";
}
#define ST(va) str_at(fe, (uint32_t)(va))

/* ------------------------------------------------------------------------ */
/* Numbers -- gate only. See the file header: n2w and three_digits genuinely */
/* differ and are not reconstructed, so a numeric-shaped token is refused   */
/* rather than mistranslated.                                              */
/* ------------------------------------------------------------------------ */

/* TISPAN32 0x1C407e20 opening gate, instruction-identical with TIENG32
 * 0x1c208150's (see textphon_eng.c's numbers()): is the token at the cursor
 * numeric-shaped ("123", "$5", "(1", ".5", ...)? */
static int looks_numeric(const char *tok)
{
    unsigned char ch = (unsigned char)*tok;
    if (ct(ch) & 4)
        return 1;
    if (ch == '\0')
        return 0;
    {
        unsigned char n1 = (unsigned char)tok[1];
        if (ch == '$')
            return (ct(n1) & 4) || n1 == '.';
        if (ch == '(' || ch == '.')
            return (ct(n1) & 4) != 0;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Words.                                                                    */
/* ------------------------------------------------------------------------ */

/* TISPAN32 0x1C406cc0's bucket lookup, == TIENG32 0x1c207230's exc_bucket()
 * (textphon_eng.c) with Spanish's own tables. */
static const unsigned char *exc_bucket(fe_t *fe, unsigned char ch, int *fb)
{
    uint32_t va;
    uint16_t k = fe->cls[ch];
    if ((k & 0x80) && ch <= 0x7a) {
        va = rd32(fe, 0x1c40e2c4u + 4 * (int32_t)(signed char)ch);
    } else {
        va = rd32(fe, (!(k & 0x80) && (k & 2)) ? 0x1c40e430u : 0x1c40e434u);
        *fb = 1;
    }
    return va ? (const unsigned char *)str_at(fe, va) : NULL;
}

/* TISPAN32 0x1C406cc0: the exception dictionary, " [WORD] =PHONEMES\".
 * Identical algorithm to TIENG32 0x1c207230 (textphon_eng.c's exceptions()),
 * confirmed instruction-for-instruction for its whole control-flow body (the
 * diff against TIENG32 only desyncs once the linear disassembly runs off the
 * end of the function into the embedded rule-text data, which differs in
 * length between the two dictionaries, as expected). Ported by direct
 * transcription of that shared logic with Spanish's own table addresses. */
static int exceptions(fe_t *fe)
{
    tp_ctx *c = &fe->c;
    const uint16_t *cls = fe->cls;
    char *out = c->out;
    int fb = 0;
    const unsigned char *ip = (const unsigned char *)c->in;

    for (;;) {
        const unsigned char *cur = ip;
        const unsigned char *rp = exc_bucket(fe, *cur, &fb);
        const unsigned char *lb, *after, *r, *x;
        int step, side;

        if (!rp)
            goto finish;
rule:
        ip = cur;
        while (*rp != '[')  /* rules have no embedded NULs */
            rp++;
        lb = rp;
        rp++;
        if (fb) {
            if (*rp == '*')
                goto finish;
        } else if ((signed char)cur[1] < (signed char)rp[1] || *rp == '*') {
            goto finish;
        }
        for (;;) {
            unsigned char rc = *rp, v;
            if (rc == ']')
                break;
            v = sv_rule_accented(rc) ? *ip : sv_rule_strip_accent(&fe->rules, *ip);
            ip++;
            rp++;
            if (rc != v)
                goto rule;
        }

        after = rp;
        r = lb - 1;
        x = cur - 1;
        step = -1;
        for (side = 0;;) {
            while (*r != '\\' && *r != '=') {
                unsigned char rc = *r;
                if (!(cls[rc] & 0x400)) {
                    unsigned char v = sv_rule_accented(rc) ? *x
                                                           : sv_rule_strip_accent(&fe->rules, *x);
                    if (rc != v)
                        goto fail;
                    r += step;
                    x += step;
                    continue;
                }
                switch (rc) {
                case ' ':
                    if (cls[*x] & 0x80)
                        goto fail;
                    break;
                case '#':
                case '+':
                    if (!(cls[*x] & 0x40))
                        goto fail;
                    break;
                case '%': {
                    int32_t n;
                    if (x[0] == 'E') {
                        if (x[1] == 'R')
                            n = 2 + (x[2] == 'S');
                        else if (x[1] == 'L' && x[2] == 'Y')
                            n = 3;
                        else if (x[1] == 'S' || x[1] == 'D')
                            n = 2;
                        else
                            n = 1;
                    } else if (x[0] == 'I' && x[1] == 'N' && x[2] == 'G') {
                        n = 3 + (x[3] == 'S');
                    } else {
                        goto fail;
                    }
                    x += n;
                    if (cls[*x] & 0x80)
                        goto fail;
                    r += step;
                    continue;
                }
                case '&':
                    if (*x == 'H') {
                        if (x[-1] != 'C' && x[-1] != 'S')
                            goto fail;
                        r += step;
                        x += 2 * step;
                        continue;
                    }
                    if (!(cls[*x] & 0x10))
                        goto fail;
                    break;
                case '.':
                    if ((cls[*x] & 0x28) != 0x28)
                        goto fail;
                    break;
                case '>':
                    if (cls[*x] & 1)
                        goto fail;
                    break;
                case '?':
                    if (!(cls[*x] & 2))
                        goto fail;
                    break;
                case '@':
                    if (*x == 'H') {
                        if (x[-1] != 'T' && x[-1] != 'C' && x[-1] != 'S')
                            goto fail;
                        r += step;
                        x += 2 * step;
                        continue;
                    }
                    if (!(cls[*x] & 4))
                        goto fail;
                    break;
                case '^':
                    if (!(cls[*x] & 0x20))
                        goto fail;
                    break;
                case ':':
                    while (cls[*x] & 0x20)
                        x += step;
                    r += step;
                    continue;
                default:
                    goto fail;
                }
                r += step;
                x += step;
            }
            if (++side >= 2)
                break;
            step = 1;
            r = after + 1;
            x = ip;
        }

        {
            const unsigned char *o = ++r;
            int32_t n = 0;
            c->status = 2;
            while (*r != '\\') {
                n++;
                r++;
            }
            if (c->out_left < n)
                return 1;
            strncat(out, (const char *)o, (size_t)(int16_t)n);
            out += n;
            c->src += (const char *)cur - c->in;
            c->out_left -= n;
            c->out = out;
            c->in = (char *)cur;
        }
        continue;

fail:
        rp = r;
        do
            rp++;
        while (*rp != '\\');
        goto rule;
    }

finish:
    *out = '\0';
    c->out = out;
    c->src += (const char *)ip - c->in;
    c->in = (char *)ip;
    return 0;
}

/* TISPAN32 0x1C407240 -- default stress. Genuinely different from TIENG32
 * 0x1c2077b0 (diverges at instruction 0): the disassembly implements the
 * standard Spanish stress rule applied to the PHONEME string TISPAN32's own
 * rules/exceptions just produced for this word, walking from its end:
 *
 *   1. Scan c->out backward to the space that starts this word (identical
 *      shape to TIENG32's own scan: bails out returning "no default stress"
 *      if a stress-class byte other than the digits '1'/'2' is hit first --
 *      i.e. the word already carries an explicit stress mark).
 *   2. From the ORIGINAL c->out position (the word's end, typically the NUL)
 *      scan backward for the last ALPHABETIC byte -- the last phoneme-code
 *      character actually written, skipping only that terminator.
 *   3. If that last code is 'N', 'S', or matches one of 8 two-byte "ending"
 *      table entries at 0x1c40e439 (with two of the eight excluded from
 *      this immediate test -- table indices 4 and 5 -- see step 4b), stress
 *      goes on the PENULTIMATE vowel: walk backward re-testing the SAME
 *      table for a second match, tracking adjacency (two matches exactly 2
 *      bytes apart read as one diphthong nucleus, not two).
 *   4. Otherwise stress goes on the LAST vowel: walk backward re-testing
 *      the table (this time with no exclusions) until the first match or
 *      the word's start space.
 *   5. Insert '5' immediately before the chosen vowel code, shifting
 *      whatever followed it one byte to the right (same insertion shape as
 *      TIENG32's own tail).
 *
 * The two entries excluded from step 3's immediate test (table indices 4
 * and 5, VAs 0x1c40e441 and 0x1c40e443) are followed by the SAME table scan
 * without that exclusion in the disassembly's fallback path, so both are
 * simply less certain to indicate a stressed-syllable-final vowel on their
 * own and get re-examined by the general scan.
 *
 * This reconstruction has NOT yet been differentially verified against the
 * original with tools/sv_emu.py (only worked out from static disassembly);
 * see the hand-back notes. */
static int default_stress(fe_t *fe)
{
    tp_ctx *c = &fe->c;
    const unsigned char *table = (const unsigned char *)ST(0x1c40e439u);
    unsigned char *scan = (unsigned char *)c->out;
    unsigned char *end;   /* last alphabetic phoneme-code byte */
    unsigned char last;
    int penult;

    if (*scan != ' ') {
        for (;;) {
            unsigned char al = *scan;
            if ((fe->cls[al] & 2) && al != '1' && al != '2')
                return 0;
            scan--;
            if (*scan == ' ')
                break;
        }
    }

    end = (unsigned char *)c->out;
    last = *end;
    if (!(fe->cls[last] & 0x80)) {
        do {
            end--;
            last = *end;
        } while (!(fe->cls[last] & 0x80));
    }

    penult = (last == 'N' || last == 'S');
    if (!penult) {
        const unsigned char *t = table;
        for (; t < table + 16; t += 2) {
            if (t[0] != last)
                continue;
            if (t[-1] != end[-1])
                continue;
            if (t == table + 10) /* VA 0x1c40e443: excluded here */
                continue;
            if (t != table + 8) { /* VA 0x1c40e441: excluded here too */
                penult = 1;
                break;
            }
        }
    }

    if (penult) {
        unsigned char *at = NULL, *prev = NULL;
        int matches = 0;
        unsigned char *p = end;
        for (;;) {
            const unsigned char *t = table;
            int found = 0;
            for (; t < table + 16; t += 2) {
                if (t[0] == *p && t[-1] == p[-1]) {
                    found = 1;
                    if (t == table && matches == 1 && prev && (prev - p) != 2)
                        found = 0;   /* not one diphthong: keep scanning */
                    if (found)
                        break;
                }
            }
            if (found) {
                at = p;
                matches++;
                if (matches >= 2)
                    break;
                prev = p;
                p--;
                continue;
            }
            p--;
            if (*p == ' ')
                break;
        }
        if (matches == 0)
            return 0;
        end = at;
    } else {
        unsigned char *p = end;
        for (;;) {
            const unsigned char *t = table;
            int found = 0;
            for (; t < table + 16; t += 2)
                if (t[0] == *p && t[-1] == p[-1]) { found = 1; break; }
            if (found) {
                end = p;
                break;
            }
            if (*p == ' ')
                return 0;
            p--;
        }
    }

    if (fe->cls[end[1]] & 2)   /* already has a stress mark right after it */
        return 0;
    if (c->out_left < 1)
        return 1;
    c->out_left--;
    {
        unsigned char *d = end;
        unsigned char carry = '5';
        while (*d) {
            unsigned char t = *d;
            *d++ = carry;
            carry = t;
        }
        *d++ = carry;
        *d = '\0';
        c->out = (char *)d;
    }
    return 0;
}

/* TISPAN32 0x1C409880: spell mode, one table entry per character.
 * Instruction-identical to TIENG32 0x1c209a00 (textphon_eng.c's spell()). */
static int spell(fe_t *fe)
{
    tp_ctx *c = &fe->c;
    char *p = c->in;
    char *o = c->out;
    uint32_t st = c->status;
    unsigned char ch;

    for (;;) {
        unsigned char nx;
        const char *s;
        int32_t n;

        ch = (unsigned char)*p++;
        if (ch == 0 || ch >= 0x80)
            break;
        nx = (unsigned char)*p;
        s = ST(0x1c40ef58u + 8u * (uint32_t)ch);
        if (ch == '.') {
            if (ct(nx) & 4)
                s = ST(0x1c40f360u);
            else if (!(ct(nx) & 8) && nx != 0)
                s = ST(0x1c40f358u);
        } else if (ch == '?') {
            if ((ct(nx) & 8) || nx == 0)
                s = ST(0x1c40ee84u);
        }
        n = (int32_t)strlen(s);
        if (n > c->out_left) {
            c->status = 0;
            return 1;
        }
        c->out_left -= n;
        memcpy(o, s, (size_t)n);
        o += n;
        *o = '\0';
        c->src++;
        c->in++;
        c->out = o;
        c->status = st | 6;
        if (ct(ch) & 8)
            break;
    }
    if (ch == 0)
        return 0;
    if (c->out_left == 0) {
        c->status = 0;
        return 1;
    }
    c->out_left--;
    o++;
    o[-2] = ',';
    o[-1] = ' ';
    *o = '\0';
    c->out = o;
    c->status = st | 6;
    return 0;
}

/* Vtable +0x0C, TISPAN32 0x1C406250: one word. Instruction-identical to
 * TIENG32 0x1c2067c0 (see the file header). */
static int lang_word(fe_t *fe)
{
    tp_ctx *c = &fe->c;
    uint32_t stress_all = c->opts & 2;

    if (c->opts & 4) {
        if (spell(fe))
            return 1;
        c->status &= ~4u;
        return 0;
    }
    for (;;) {
        c->status &= ~4u;
        /* TISPAN32 0x1C409620, the user dictionary (userdict.c, shared with
         * TIENG32's 0x1c2097a0 -- src/textphon_eng.c's lang_word() calls the
         * exact same sv_userdict_lookup() at the exact same point). Only a
         * no-op when fe->dict is NULL, so behaviour without one attached is
         * unchanged. */
        if (fe->dict) {
            const char *in = c->in;
            int rc = sv_userdict_lookup(fe->dict, fe->cls, c->src, &in,
                                        &c->out, &c->out_left, &c->status);
            c->src += in - c->in;
            c->in = (char *)in;
            if (rc)
                return 1;
        }
        if (exceptions(fe))
            return 1;
        if (looks_numeric(c->in))
            return -2;   /* SV_NAR_E_NOTIMPL sentinel: see numbers_span() note */
        if (c->status & 4)
            continue;
        {
            const char *in = c->in;
            int rc = sv_rules_step(&fe->rules, c->base, &in, &c->out, &c->out_left,
                                   c->opts, &c->status);
            c->src += in - c->in;
            c->in = (char *)in;
            if (rc)
                return 1;
        }
        if (c->status & 4)
            continue;
        if (stress_all || (c->status & 1))
            default_stress(fe);
        return 0;
    }
}

/* TIBASE32 0x1c0100d0: "{{text}}" goes to the output as "{text}", lower-
 * cased. Language-independent (TIBASE32-level); duplicated verbatim from
 * textphon_eng.c's passthrough() rather than sharing a symbol across the two
 * front-end files. */
static int passthrough(tp_ctx *c)
{
    char *o = c->out;
    char *p = c->in;
    char *open, a;
    int32_t n;

    c->status &= ~4u;
    if (c->opts & 0x10)
        return 0;
    if (*p++ != '{')
        return 0;
    if (*p++ != '{')
        return 0;
    open = p - 1;
    a = *p++;
    n = 0;
    while (a != '\0') {
        if (a == '}' && *p == '}')
            break;
        a = *p++;
        n++;
    }
    if (a != '}')
        return 0;
    p++;
    n += 2;
    if (c->out_left < n)
        return 1;
    strncat(o, open, (size_t)n);
    o++;
    c->out_left -= n;
    c->out += n;
    n -= 2;
    c->in = p;
    for (; n > 0; n--, o++)
        *o = (char)c_tolower((unsigned char)*o);
    *c->out = '\0';
    c->status |= 4;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Entry points.                                                            */
/* ------------------------------------------------------------------------ */

#define SLACK 16

static int fe_init(fe_t *fe, const sv_langmod *m)
{
    memset(fe, 0, sizeof *fe);
    fe->g = (sv_langgen *)m->priv;
    if (!fe->g)
        return -1;
    fe->rules = sv_lang_data_span;
    fe->cls = sv_lang_data_span.charclass;
    return 0;
}

/* Which Latin-1 letters SVTextToPhon keeps, TIBASE32 0x1c010074 -- language
 * independent, duplicated verbatim from textphon_eng.c. */
static int keep_latin1(unsigned char ch, unsigned char *to)
{
    switch (ch) {
    case 0xa2: case 0xa3: case 0xa5: case 0xa9: case 0xae: case 0xb0:
    case 0xb1: case 0xbc: case 0xbd: case 0xbe: case 0xc1: case 0xc4:
    case 0xc9: case 0xcd: case 0xd1: case 0xd3: case 0xd6: case 0xd7:
    case 0xda: case 0xdc: case 0xdf: case 0xe1: case 0xe9: case 0xed:
    case 0xf1: case 0xf3: case 0xf7: case 0xfa:
        *to = ch;
        return 1;
    case 0xe4: *to = 0xc4; return 1;
    case 0xf6: *to = 0xd6; return 1;
    case 0xfc: *to = 0xdc; return 1;
    default:
        return 0;
    }
}

int32_t sv_text_to_phon_span_ex(const sv_langmod *m, const char *text, char *out,
                                int32_t out_size, uint32_t flags,
                                const sv_userdict_t *dict)
{
    fe_t *fe;
    tp_ctx *c;
    size_t tl;
    int16_t size;
    char *block, *buf, *w, *p, *mark = NULL;
    const unsigned char *s;
    int32_t ofs = 0, ret = 0;
    int rc = 0;

    if (!m || !text || !out || out_size <= 0)
        return SV_TP_E_BADARG;
    tl = strlen(text);
    if (tl + 3 > 0x7fff)
        return SV_NAR_E_NOMEM;
    size = (int16_t)(tl + 3);
    fe = malloc(sizeof *fe);
    block = calloc((size_t)size + 2 * SLACK, 1);
    if (!fe || !block || fe_init(fe, m)) {
        int bad = fe && block;
        free(fe);
        free(block);
        return bad ? SV_NAR_E_NOTIMPL : SV_NAR_E_NOMEM;
    }
    fe->dict = dict;
    buf = block + SLACK;
    c = &fe->c;
    c->src = text;
    c->out = out;
    c->in = buf;
    c->base = buf;
    c->out_left = out_size;
    c->opts = flags;
    c->status = 0;

    buf[0] = ' ';
    w = buf + 1;
    if (tl > 0x202)
        *w++ = '\0';
    for (s = (const unsigned char *)text; *s; s++, w++) {
        unsigned char ch = *s, to;
        if (ct(ch) & 2)
            *w = (char)c_toupper(ch);
        else if (ch >= 0xa2 && ch <= 0xfc && keep_latin1(ch, &to))
            *w = (char)to;
        else if (ch == ']' && !(flags & 4))
            *w = ' ';
        else if (ct(ch) & 0x157)
            *w = (char)ch;
        else
            *w = ' ';
    }
    *w = '\0';
    out[0] = '\0';
    c->in++;
    c->out_left--;

    for (p = c->in; *p == ' '; p++)
        ;
    while (*p) {
        ofs = (int32_t)(p - c->base - 1);
        if ((c->opts & 8) && (!(p[0] == '{' || p[1] == '{') || (c->opts & 0x10))) {
            char marker[16];
            int32_t n = 0;
            char digits[12];
            int32_t v = ofs, nd = 0;
            int neg = v < 0;
            marker[n++] = '@';
            marker[n++] = 'w';
            if (neg)
                marker[n++] = '-';
            do {
                digits[nd++] = (char)('0' + (neg ? -(v % 10) : v % 10));
                v /= 10;
            } while (v);
            while (nd)
                marker[n++] = digits[--nd];
            if (n + 2 >= c->out_left) {
                rc = 1;
                break;
            }
            c->out_left -= n + 2;
            *c->out++ = ' ';
            memcpy(c->out, marker, (size_t)n);
            c->out += n;
            mark = c->out - 1;
            *c->out++ = ' ';
            *c->out = '\0';
        }
        c->word = p;
        c->src += p - c->in;
        c->in = p;
        if (c->out_left <= 1) {
            rc = 1;
            break;
        }
        mark = c->out;
        *c->out++ = ' ';
        *c->out = '\0';
        c->out_left--;
        if ((rc = passthrough(c)) != 0)
            break;
        rc = lang_word(fe);
        if (rc == -2) {
            free(block);
            free(fe);
            return SV_NAR_E_NOTIMPL;
        }
        if (rc)
            break;
        for (p = c->in; *p == ' '; p++)
            ;
    }
    if (rc == 1) {
        ret = -1 - ofs;
        if (mark)
            *mark = '\0';
    }
    free(block);
    free(fe);
    return ret;
}

int32_t sv_text_to_phon_span(const sv_langmod *m, const char *text, char *out,
                             int32_t out_size, uint32_t flags)
{
    return sv_text_to_phon_span_ex(m, text, out, out_size, flags, NULL);
}

int32_t sv_tts_phonemes_span_ex(const sv_langmod *m, const char *text,
                                uint32_t flags, const sv_userdict_t *dict,
                                char **phonemes)
{
    struct { char *p; } chunks[64];
    int nchunks = 0, i;
    int32_t mult = (flags & 4) ? 6 : 3;
    int32_t rc = 0;
    size_t total;
    char *joined;

    if (!phonemes)
        return SV_TP_E_BADARG;
    *phonemes = NULL;
    if (!m || !text)
        return SV_TP_E_BADARG;
    for (;;) {
        int32_t size;
        char *b;
        if (rc == -1)
            mult += 2;
        if (strlen(text) > (size_t)(INT32_MAX / mult) - 10) {
            rc = SV_NAR_E_NOMEM;
            break;
        }
        size = (int32_t)(strlen(text) + 10) * mult;
        b = calloc((size_t)size, 1);
        if (!b || nchunks == (int)(sizeof chunks / sizeof *chunks)) {
            free(b);
            rc = SV_NAR_E_NOMEM;
            break;
        }
        chunks[nchunks++].p = b;
        rc = sv_text_to_phon_span_ex(m, text, b, size, flags, dict);
        if (rc == 0 || rc == SV_NAR_E_NOMEM || rc == SV_NAR_E_NOTIMPL
            || rc == SV_TP_E_BADARG)
            break;
        if (rc == -1)
            free(chunks[--nchunks].p);
        else
            text -= rc + 1;
    }
    if (rc != 0) {
        for (i = 0; i < nchunks; i++)
            free(chunks[i].p);
        return rc;
    }
    total = 1;
    for (i = 0; i < nchunks; i++)
        total += strlen(chunks[i].p);
    joined = realloc(chunks[0].p, total);
    if (!joined) {
        for (i = 0; i < nchunks; i++)
            free(chunks[i].p);
        return SV_NAR_E_NOMEM;
    }
    for (i = 1; i < nchunks; i++) {
        strcat(joined, chunks[i].p);
        free(chunks[i].p);
    }
    *phonemes = joined;
    return 0;
}

int32_t sv_tts_phonemes_span(const sv_langmod *m, const char *text, uint32_t flags,
                             char **phonemes)
{
    return sv_tts_phonemes_span_ex(m, text, flags, NULL, phonemes);
}
