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
 * so are the functions it calls for everything but three number functions
 * and default stress:
 *
 *   TISPAN32 0x1C406250  vtable +0x0C           per-word driver
 *   TISPAN32 0x1C406cc0  exception dictionary matcher      (== TIENG32 0x1c207230)
 *   TISPAN32 0x1C407e20  number/token normaliser            (== TIENG32 0x1c208150)
 *   TISPAN32 0x1C407da0  scale word follows                 (== TIENG32 0x1c2080d0,
 *                        Spanish words and lengths)
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
 * Confirmed instruction-identical, the pattern-dispatch table at 0x1C40E628
 * included (same seventeen patterns, same handler slots); what differs
 * between the two languages is only the embedded literal addresses (data),
 * which this file cites at TISPAN32's OWN real virtual addresses -- see
 * tools/extract_span_frontend.py for how those addresses end up inside the
 * SAME mutable .data copy the frame generator (src/langgen.c) uses (one
 * sv_langgen instance per module). The literal map was read off the aligned
 * instructions, not guessed from string content.
 *
 * Three number functions are genuinely Spanish code, transcribed from
 * TISPAN32's own disassembly:
 *
 *   TISPAN32 0x1C4073D0  n2w, cardinal number -> words (TIENG32 0x1c207890's
 *                        slot). Validation, currency, percent, scale words
 *                        and the zero-group cut keep English's shape; the
 *                        group reading is Spanish: "UW4NOH" dropped before a
 *                        scale word, irregular hundreds, "-Y" tens before a
 *                        unit, "IY" + digits + "DEH5SIYMAAL" for decimals.
 *   TISPAN32 0x1C408f60  three digits of a phone number (TIENG32 0x1c209290's
 *                        slot): 100 / N00 as one word, else digit by digit.
 *   TISPAN32 0x1C4090F0  "####" as a year (TIENG32 0x1c209380's slot): a
 *                        cardinal, "MIY5L" and n2w's hundreds and tens.
 *
 * tools/verify_spanish.py --numbers compares all of it with TISPAN32.
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
 *   number word tables             0x1C40EB98 ones, 0x1C40EBE8 / 0x1C40EC10
 *                                  tens, 0x1C40EB38 scale, 0x1C40E628 patterns
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
    /* 0x1C407e20's token pointers, 0x1c411d6c..0x1c411d78 (TIENG32's
     * 0x1c25011c..0x1c250128). */
    char *tok;   /* 0x1c411d78 */
    char *next;  /* 0x1c411d6c: the token's end, then the next word */
    char *wend;  /* 0x1c411d70 */
    char *tend;  /* 0x1c411d74 */
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
/* Numbers.                                                                  */
/* ------------------------------------------------------------------------ */

/* Working buffers in TISPAN32 BSS, == TIENG32's 0x1c250130 / 0x1c24ed90 /
 * 0x1c24ed88 (textphon_eng.c's PAT / OUTB / TMP), at their own addresses. */
#define PAT  ((char *)DP(0x1c411d80u))  /* token pattern, "#,###.##" */
#define OUTB ((char *)DP(0x1c4109e0u))  /* phonemes being built */
#define TMP  ((char *)DP(0x1c4109d0u))  /* one number of h:m:s */

/* *(char **)va, i.e. a vocabulary table entry. */
#define SP(va) str_at(fe, rd32(fe, (uint32_t)(va)))

/* The word tables. ONES is 0..19 ("SEH5ROH ".."DIYEHSIY NUWEY5VEH ") and
 * runs straight on into TENS0, the tens said alone ("VEY5NTEY "); TENSY is
 * the tens a unit follows ("VEY5NTEHIY ", "TREY5NTAAIY "). DIGIT is ONES
 * indexed by character code. SCALE(1) is "MIY5L ", SCALE(2) "MIYYOH5N "... */
#define ONES(i)  SP(0x1c40eb98u + 4 * (int32_t)(i))
#define TENS0(i) SP(0x1c40ebe8u + 4 * (int32_t)(i))
#define TENSY(i) SP(0x1c40ec10u + 4 * (int32_t)(i))
#define DIGIT(c) SP(0x1c40ead8u + 4 * (int32_t)(c))
#define SCALE(i) SP(0x1c40eb38u + 4 * (int32_t)(i))

/* strcat into one of the .data buffers, bounded by the end of the .data
 * copy where the original has no bound (textphon_eng.c's cat()). */
static void cat(fe_t *fe, char *dst, const char *src)
{
    char *end = (char *)G->d + G->dsize - 1;
    char *d = dst + strlen(dst);
    if ((uint8_t *)dst < G->d || dst > end) {
        strcat(dst, src);
        return;
    }
    while (*src && d < end)
        *d++ = *src++;
    *d = '\0';
}

/* TISPAN32 0x1C407da0: does the next word scale the number? Same shape as
 * TIENG32 0x1c2080d0 with the Spanish words and their own lengths. */
static int scale_follows(fe_t *fe, const char *next)
{
    if (!next)
        return 0;
    if (!strncmp(next, ST(0x1c40e568u), 6) || !strncmp(next, ST(0x1c40e560u), 6)
        || !strncmp(next, ST(0x1c40e558u), 7))
        return 1;
    return !strncmp(next, ST(0x1c40e54cu), 10);   /* "QUADRILLON" */
}

/* The hundreds of a group, the jump table at 0x1c407d48 (n2w) and its twin
 * at 0x1c4093d4 (year): 1 is "SIYEH4N" alone, 5/7/9 are irregular, the rest
 * are the digit followed by "SIYEH4N". Then "TOHS " for the plural, or for
 * 100 "TOH " when tens or units follow and " " when not. The two copies
 * differ only in the stress digit of the irregular 7 and 9. */
static void hundreds(fe_t *fe, int32_t h, int32_t rest, int year, char *out)
{
    switch (h) {
    case 1:
        cat(fe, out, ST(0x1c40e534u));   /* "SIYEH4N" */
        break;
    case 5:
        cat(fe, out, ST(0x1c40e528u));   /* "KIYNIYEH4N" */
        break;
    case 7:
        cat(fe, out, ST(year ? 0x1c40e7a0u : 0x1c40e518u));   /* "SEH{4,3}TEHSIYEH4N" */
        break;
    case 9:
        cat(fe, out, ST(year ? 0x1c40e790u : 0x1c40e508u));   /* "NOH{4,3}VEHSIYEH4N" */
        break;
    case 2: case 3: case 4: case 6: case 8:
        cat(fe, out, ONES(h));
        cat(fe, out, ST(0x1c40e534u));
        break;
    default:
        break;   /* 0x1c40764b: `ja` past the table, nothing said */
    }
    if (h > 1)
        cat(fe, out, ST(0x1c40e500u));   /* "TOHS " */
    else
        cat(fe, out, ST(rest ? 0x1c40e4f8u : 0x1c40e4f4u));   /* "TOH " / " " */
}

/* Tens and units, 0x1c407826..0x1c4078b6 (n2w) and 0x1c409336..0x1c4093c0
 * (year): 20 and up as a tens word, the "-Y" form when a unit follows; below
 * 20 one word, nothing for zero. */
static void tens_units(fe_t *fe, int32_t t, int32_t u, char *out)
{
    if (t >= 2) {
        cat(fe, out, u ? TENSY(t) : TENS0(t));
        if (u)
            cat(fe, out, ONES(u));
    } else if (t * 10 + u) {
        cat(fe, out, ONES(t * 10 + u));
    }
}

/* TISPAN32 0x1C4073D0: a number as words ("1.250,5" is not Spanish input
 * here -- the pattern is the English "#,###.##" -- "$3.20", "12%"). The
 * validation, the dollar/percent/scale bookkeeping and the zero-group cut
 * are TIENG32 0x1c207890's; the group reading is Spanish: a lone "1" group
 * before a scale word is dropped ("MIY5L", not "UW4NOH MIY5L"), hundreds go
 * through hundreds(), decimals are "IY" + digits + "DEH5SIYMAAL", and cents
 * have no "and". Returns 0 when the token is not a well-formed number, 1
 * when it was said, 2 when the scale word that follows was said with it. */
static int n2w(fe_t *fe, char *tok, const char *next, char *out)
{
    int32_t len = (int32_t)strlen(tok);
    int dollar = *tok == '$';
    int percent, scale, one_dollar, cents_ok, valid, run, last_zero;
    int32_t int_len, frac, rem, commas, k;
    char *lastp, *p;
    const char *pp;

    if (dollar) {
        tok++;
        len--;
    }
    lastp = tok + len - 1;
    percent = *lastp == '%';
    if (percent) {
        len--;
        *lastp = '\0';
    }
    int_len = (int32_t)strcspn(tok, ST(0x1c40e548u));   /* "." */
    if (int_len > 0x52) {
        if (percent)
            *lastp = '%';
        return 0;
    }
    frac = len - int_len - 1;
    if (frac <= 0)
        frac = 0;
    scale = scale_follows(fe, next);
    one_dollar = dollar && int_len == 1 && *tok == '1';
    cents_ok = dollar && (frac == 2 || frac == 0) && !scale;

    /* 0x1c4074d5: every comma closes exactly three digits, the leading group
     * has at most three. */
    valid = 1;
    run = 0;
    pp = PAT + int_len - 1 + (dollar ? 1 : 0);
    for (k = int_len; k != 0; k--) {
        char ch = *pp--;
        if (ch == '#')
            run++;
        else if (ch == ',' && run == 3)
            run = 0;
        else
            valid = 0;
    }
    if (run > 3)
        valid = 0;
    if (frac && strchr(tok + int_len + 1, '.'))
        valid = 0;
    if (!valid) {
        if (percent)
            *lastp = '%';
        return 0;
    }

    commas = 0;
    for (k = 0; k < int_len; k++)
        commas += tok[k] == ',';

    /* With no integer part, 0x1c40799c tests a stack slot only the group
     * loop writes (the strcat length scratch at [esp+0x10]). It only decides
     * whether the last ',' in the output is cut, and none of n2w's callers
     * can reach it with a ',' in the output and no integer part: '(' needs a
     * digit after it, and OUTB starts as " ". 0 stands in for it, as in
     * textphon_eng.c. */
    last_zero = 0;
    p = tok;
    rem = int_len;
    if (rem > 0) {
        do {
            int32_t n = (int32_t)strcspn(p, ST(0x1c40e544u));   /* ",." */
            int32_t g = n >= 3 ? 3 : n;
            int32_t h = 0, t = 0, u = 0;
            switch (g) {
            case 3: h = (signed char)*p++ - '0'; /* fall through */
            case 2: t = (signed char)*p++ - '0'; /* fall through */
            case 1: u = (signed char)*p++ - '0'; break;
            }
            if (h + t == 0 && u == 1) {
                /* 0x1c40761e: "UW4NOH ", unless a scale word follows. */
                if (commas == 0)
                    cat(fe, out, ST(0x1c40e53cu));
            } else {
                if (h)
                    hundreds(fe, h, t + u, 0, out);
                tens_units(fe, t, u, out);
            }
            last_zero = (uint32_t)(h + t + u) < 1;
            if (last_zero) {
                if (rem <= 3)
                    while (rem-- != 0)
                        cat(fe, out, ST(0x1c40e4e8u));   /* "SEH5ROH " */
                commas--;
            } else if (commas) {
                cat(fe, out, SCALE(commas));
                commas--;
                cat(fe, out, ST(0x1c40e4e4u));   /* ", " */
            }
            p++;
            rem -= g + 1;
        } while (rem > 0);
    }
    if (last_zero) {
        char *c = strrchr(out, ',');
        if (c)
            *c = '\0';
    }
    if (int_len == 0)
        p++;

    if (dollar && cents_ok) {
        if (one_dollar)
            cat(fe, out, ST(0x1c40e4d8u));   /* "DOH5LAAR " */
        else if (int_len)
            cat(fe, out, ST(0x1c40e4c8u));   /* "DOH5LAAREHS " */
        if (frac == 2) {
            int32_t d1 = (signed char)p[0] - '0', d2 = (signed char)p[1] - '0';
            int32_t cents = d1 * 10 + d2;
            p += 2;
            if (cents) {
                if (d1 < 2) {
                    cat(fe, out, ONES(cents));
                } else {
                    cat(fe, out, TENS0(d1));   /* sic: never the "-Y" form */
                    if (d2)
                        cat(fe, out, ONES(d2));
                }
                cat(fe, out, ST(cents == 1 ? 0x1c40e4b8u : 0x1c40e4a8u));   /* SEHNTAA5VOH(S) */
            }
        }
    } else if (frac) {
        cat(fe, out, ST(0x1c40e4a4u));   /* "IY " */
        while (frac-- != 0)
            cat(fe, out, DIGIT((signed char)*p++));
        cat(fe, out, ST(0x1c40e494u));   /* "DEH5SIYMAAL " */
    }

    {
        int ret = 1;
        if (scale) {
            switch (c_toupper((unsigned char)*next)) {
            case 'B': cat(fe, out, ST(0x1c40e478u)); break;   /* "MIY3L MIYYOH5N " */
            case 'M': cat(fe, out, ST(0x1c40e488u)); break;   /* "MIYYOH5N " */
            case 'Q': cat(fe, out, ST(0x1c40e45cu)); break;   /* "KWAADRIYYOH5N " */
            case 'T': cat(fe, out, ST(0x1c40e46cu)); break;   /* "TRIYYOH5N " */
            }
            ret = 2;
        }
        if (dollar && (scale || !cents_ok))
            cat(fe, out, ST(0x1c40e4c8u));   /* "DOH5LAAREHS " */
        if (scale && strchr(next, ')'))
            cat(fe, out, ST(0x1c40e4e4u));
        if (percent) {
            cat(fe, out, ST(0x1c40e448u));   /* "POHR SIYEH5NTOH " */
            *lastp = '%';
        }
        return ret;
    }
}

/* TISPAN32 0x1C409400, == TIENG32 0x1c209580: digit strings. Parentheses
 * are said as pauses; a number is tried first; otherwise each character is
 * spelled. On a character it cannot spell it stores that position through
 * `pnext` and returns 3, so the caller resumes there. */
static int spell_digits(fe_t *fe, char *tok, char **pnext, char *out)
{
    int32_t len = (int32_t)strlen(tok), k;
    char *last = tok + len - 1;
    int save = (signed char)*last;
    int rc;

    if (save == ')') {
        *last = '\0';
        PAT[len - 1] = '\0';
    }
    if (*tok == '(') {
        tok++;
        for (k = 0; k < len; k++)
            PAT[k] = PAT[k + 1];
        cat(fe, out, ST(0x1c40e4e4u));
    }
    rc = n2w(fe, tok, NULL, out);
    *last = (char)save;
    if (rc) {
        if (save == ')')
            cat(fe, out, ST(0x1c40e4e4u));
        return rc;
    }
    for (k = len; k != 0; k--) {
        unsigned char x = (unsigned char)*tok++;
        const char *s;
        if (ct(x) & 4) {
            s = DIGIT(x);
        } else {
            switch (x) {
            case '$': s = ST(0x1c40e4d8u); break;   /* DOH5LAAR */
            case '+': s = ST(0x1c40e7ccu); break;   /* MAA5S */
            case ',': s = ST(0x1c40e768u); break;   /* "- " */
            case '-': s = ST(0x1c40e7d4u); break;   /* MEH5NOHS */
            case '.': s = ST(0x1c40e7ecu); break;   /* PUW5NTOH */
            case '/': s = ST(0x1c40e7e0u); break;   /* KOHRTAA5R */
            case ':': s = ST(0x1c40e7b8u); break;   /* DOH3S PUW5NTOHS */
            default:
                *pnext = tok - 1;
                return 3;
            }
        }
        cat(fe, out, s);
    }
    return 1;
}

/* TISPAN32 0x1C408f60 (TIENG32 0x1c209290's slot): three digits of a phone
 * number, "5 0 0" as one word -- "SIYEH5N " for 100, the digit and
 * "SIYEH4NTOHS " otherwise -- and anything else digit by digit. */
static int three_digits(fe_t *fe, const char *s, char *out)
{
    int32_t d0 = (signed char)s[0] - '0';
    int32_t d1 = (signed char)s[1] - '0';
    int32_t d2 = (signed char)s[2] - '0';
    if (d0 && !d1 && !d2) {
        if (d0 == 1) {
            cat(fe, out, ST(0x1c40e784u));   /* "SIYEH5N " */
        } else {
            cat(fe, out, ONES(d0));
            cat(fe, out, ST(0x1c40e774u));   /* "SIYEH4NTOHS " */
        }
        return 1;
    }
    cat(fe, out, ONES(d0));
    cat(fe, out, ONES(d1));
    cat(fe, out, ONES(d2));
    return 1;
}

/* TISPAN32 0x1C408cc0, == TIENG32 0x1c208ff0: "###-####". */
static int h_local(fe_t *fe, char *tok, char *out)
{
    cat(fe, out, DIGIT((signed char)tok[0]));
    cat(fe, out, DIGIT((signed char)tok[1]));
    cat(fe, out, DIGIT((signed char)tok[2]));
    cat(fe, out, ST(0x1c40e4e4u));
    cat(fe, out, DIGIT((signed char)tok[4]));
    return three_digits(fe, tok + 5, out);
}

/* TISPAN32 0x1C408df0, == TIENG32 0x1c209120: "#-###-###-####". */
static int h_long_distance(fe_t *fe, char *tok, char *out)
{
    cat(fe, out, DIGIT((signed char)tok[0]));
    cat(fe, out, ST(0x1c40e710u));   /* "," */
    three_digits(fe, tok + 2, out);
    cat(fe, out, ST(0x1c40e710u));
    h_local(fe, tok + 6, out);
    return 1;
}

/* TISPAN32 0x1C408ec0, == TIENG32 0x1c2091f0: "###-###-####" and
 * "###/###-####". English's "area code" string is a lone " " here. */
static int h_area_code(fe_t *fe, char *tok, char *out)
{
    cat(fe, out, ST(0x1c40e4f4u));   /* " " */
    three_digits(fe, tok, out);
    cat(fe, out, ST(0x1c40e710u));
    h_local(fe, tok + 4, out);
    return 1;
}

/* TISPAN32 0x1C408bf0, == TIENG32 0x1c208f20: "(###)", when a number
 * follows. */
static int h_paren_area(fe_t *fe, char *tok, const char *next, char *out)
{
    if (!next || !(ct((unsigned char)*next) & 4))
        return 0;
    cat(fe, out, ST(0x1c40e4f4u));
    three_digits(fe, tok + 1, out);
    cat(fe, out, ST(0x1c40e710u));
    return 1;
}

/* TISPAN32 0x1C408520, == TIENG32 0x1c208850: "h:mm" on a 12-hour clock.
 * On the hour Spanish says nothing more (" "), a leading-zero minute is
 * "OH4 " and the digit. */
static int h_time(fe_t *fe, char *tok, char *out)
{
    const char *s = tok + 1;
    int32_t a = (signed char)tok[0] - '0', h, d1, d2, m;
    if (*s == ':') {
        h = a;
    } else {
        h = a * 10 + ((signed char)*s - '0');
        s++;
    }
    s++;
    d1 = (signed char)s[0] - '0';
    d2 = (signed char)s[1] - '0';
    m = d1 * 10 + d2;
    if (h < 1 || h > 12 || m > 0x3b)
        return 0;
    cat(fe, out, ONES(h));
    if (m == 0) {
        cat(fe, out, ST(0x1c40e4f4u));   /* " " */
    } else if (d1 == 0) {
        cat(fe, out, ST(0x1c40e740u));   /* "OH4 " */
        cat(fe, out, ONES(d2));
    } else if (d1 == 1) {
        cat(fe, out, ONES(m));
    } else {
        cat(fe, out, TENSY(d1));
        if (d2)
            cat(fe, out, ONES(d2));
    }
    return 1;
}

/* One number of a date: below 20 as a word, otherwise the "-Y" tens and the
 * units, the units said even when zero. */
static void date_part(fe_t *fe, int32_t n, char *out)
{
    if (n < 0x14) {
        cat(fe, out, ONES(n));
    } else {
        cat(fe, out, TENSY(n / 10));
        cat(fe, out, ONES(n % 10));
    }
}

/* TISPAN32 0x1C408750, == TIENG32 0x1c208a80: "m/d/yy". */
static int h_date(fe_t *fe, char *tok, char *out)
{
    const char *s = tok;
    int32_t m = 0, d = 0, y;
    while (*s != '/') {
        m = m * 10 + (signed char)*s - '0';
        s++;
    }
    s++;
    while (*s != '/') {
        d = d * 10 + (signed char)*s - '0';
        s++;
    }
    y = ((signed char)s[1] - '0') * 10 + (signed char)s[2] - '0';
    date_part(fe, m, out);
    date_part(fe, d, out);
    date_part(fe, y, out);
    return 1;
}

/* TISPAN32 0x1C408960, == TIENG32 0x1c208c90: "h:mm:ss" as hours, minutes
 * and seconds. Each number goes through n2w with PAT forced to digits, and
 * its result is not checked. */
static int h_duration(fe_t *fe, char *tok, char *out)
{
    const char *s = tok + 1;
    char *t;
    int32_t v;
    char c;

    PAT[0] = PAT[1] = PAT[2] = '#';
    memset(TMP, 0, 4);
    t = TMP;
    v = 0;
    for (c = s[-1]; c != ':'; c = s[-1]) {
        *t++ = c;
        v = v * 10 + (signed char)c - '0';
        s++;
    }
    n2w(fe, TMP, NULL, out);
    cat(fe, out, ST(0x1c40e76cu));   /* "OH5RAA" */
    cat(fe, out, ST(v == 1 ? 0x1c40e768u : 0x1c40e764u));   /* "- " / "Z- " */

    s++;
    memset(TMP, 0, 4);
    t = TMP;
    v = 0;
    for (c = s[-1]; c != ':'; c = s[-1]) {
        *t++ = c;
        v = v * 10 + (signed char)c - '0';
        s++;
    }
    n2w(fe, TMP, NULL, out);
    cat(fe, out, ST(0x1c40e758u));   /* "MIYNUW5TOH" */
    cat(fe, out, ST(v == 1 ? 0x1c40e768u : 0x1c40e754u));   /* "- " / "S- " */

    s++;
    memset(TMP, 0, 4);
    c = s[-1];
    v = ((signed char)c - '0') * 10;
    t = TMP;
    if (c != '0')
        *t++ = c;
    t[0] = *s;
    t[1] = '\0';
    v += (signed char)*s - '0';
    n2w(fe, TMP, NULL, out);
    cat(fe, out, ST(0x1c40e748u));   /* "SEHGUW5NDOH" */
    cat(fe, out, ST(v == 1 ? 0x1c40e768u : 0x1c40e764u));
    return 1;
}

/* TISPAN32 0x1C4090F0 (TIENG32 0x1c209380's slot): "####" as a year, read
 * as a cardinal: "MIY5L" with the thousands digit only above 1, then the
 * hundreds and tens as n2w says them (with its own stress on 700 and 900).
 * A leading zero is spelled, exactly as in English. */
static int h_year(fe_t *fe, char *tok, char *out)
{
    int32_t a, b, c, d;
    if (tok[0] == '0') {
        char *scratch = NULL;   /* the original passes its own argument slot */
        spell_digits(fe, tok, &scratch, out);
        return 1;
    }
    a = (signed char)tok[0] - '0';
    b = (signed char)tok[1] - '0';
    c = (signed char)tok[2] - '0';
    d = (signed char)tok[3] - '0';
    if (a > 1)
        cat(fe, out, ONES(a));
    cat(fe, out, ST(0x1c40e7b0u));   /* "MIY5L " */
    if (b)
        hundreds(fe, b, c + d, 1, out);
    tens_units(fe, c, d, out);
    return 1;
}

/* The pattern table at 0x1C40E628 names its handlers by address. */
static int call_handler(fe_t *fe, uint32_t fn, char *tok, char *next, char *out)
{
    switch (fn) {
    case 0x1c408df0: return h_long_distance(fe, tok, out);
    case 0x1c408ec0: return h_area_code(fe, tok, out);
    case 0x1c408cc0: return h_local(fe, tok, out);
    case 0x1c4090f0: return h_year(fe, tok, out);
    case 0x1c408960: return h_duration(fe, tok, out);
    case 0x1c408520: return h_time(fe, tok, out);
    case 0x1c408bf0: return h_paren_area(fe, tok, next, out);
    case 0x1c408750: return h_date(fe, tok, out);
    case 0x1c409400: {
        /* As in English: spell_digits gets `next` itself where it expects a
         * pointer to it, and the two patterns routed here ("#####",
         * "#####-####") never reach the store. */
        char *unused = next;
        return spell_digits(fe, tok, &unused, out);
    }
    default:
        return 0;
    }
}

/* TISPAN32 0x1C407e20, instruction-identical to TIENG32 0x1c208150
 * (textphon_eng.c's numbers()): numbers and other digit tokens at the
 * cursor. Returns 1 when the output filled up, else 0; status 4 means the
 * token was said. */
static int numbers(fe_t *fe)
{
    tp_ctx *c = &fe->c;
    char *tok = c->in;
    unsigned char ch;
    char save_t, save_w;
    int32_t n, cnt, rc;
    int plain;
    uint32_t e;
    const char *q;

    fe->tok = tok;
    if (*tok == '\0')
        return 0;
    ch = (unsigned char)*tok;
    if (!(ct(ch) & 4)) {
        unsigned char n1 = (unsigned char)tok[1];
        if (ch == '$') {
            if (!(ct(n1) & 4) && n1 != '.')
                return 0;
        } else if (ch == '(' || ch == '.') {
            if (!(ct(n1) & 4))
                return 0;
        } else {
            return 0;
        }
    }

    /* The token, and the word after it. */
    PAT[0] = '\0';
    fe->next = tok + strspn(tok, ST(0x1c40e728u));  /* "0123456789,.$%+-()/:" */
    if (fe->next[-1] == ',' || fe->next[-1] == '.')
        fe->next--;
    save_t = *fe->next;
    fe->tend = fe->next;
    if (save_t) {
        *fe->next = '\0';
        fe->next++;
    }
    if (save_t && *fe->next == ' ')
        do
            fe->next++;
        while (*fe->next == ' ');
    fe->wend = strchr(fe->next, ' ');
    if (!fe->wend)
        fe->wend = fe->next + strlen(fe->next);
    if (fe->wend[-1] == ',' || fe->wend[-1] == '.')
        fe->wend--;
    save_w = *fe->wend;
    *fe->wend = '\0';

    /* Its pattern: '#' per digit, 'L' per letter, punctuation as itself. It
     * is "plain" when it has nothing but digits, '$', '%', ',' and '.'. */
    n = (int32_t)strlen(tok);
    plain = 1;
    cnt = n >= 0x14 ? 0x14 : n;
    q = tok;
    while (cnt-- != 0) {
        unsigned char x = (unsigned char)*q++;
        uint32_t s;
        int keep = 0;
        if (ct(x) & 0x103) {
            s = 0x1c40e724;   /* "L" */
        } else if (ct(x) & 4) {
            s = 0x1c40e720;   /* "#" */
            keep = 1;
        } else {
            switch (x) {
            case '$': s = 0x1c40e70c; keep = 1; break;
            case '%': s = 0x1c40e708; keep = 1; break;
            case ',': s = 0x1c40e710; keep = 1; break;
            case '.': s = 0x1c40e548; keep = 1; break;
            case '(': s = 0x1c40e71c; break;
            case ')': s = 0x1c40e718; break;
            case '-': s = 0x1c40e714; break;
            case '/': s = 0x1c40e704; break;
            case ':': s = 0x1c40e700; break;
            default: s = 0x1c40e724; break;
            }
        }
        cat(fe, PAT, ST(s));
        if (!keep)
            plain = 0;
    }

    OUTB[0] = '\0';
    cat(fe, OUTB, ST(0x1c40e4f4u));   /* " " */
    rc = 0;
    for (e = 0x1c40e628; rd32(fe, e + 8) != 0; e += 12)
        if (strcmp(ST(rd32(fe, e)), PAT) == 0) {
            rc = call_handler(fe, rd32(fe, e + 8), tok, fe->next, OUTB);
            break;
        }
    if (rc == 0) {
        if (plain)
            rc = n2w(fe, tok, fe->next, OUTB);
        if (!plain || rc == 0) {
            rc = spell_digits(fe, fe->tok, &fe->next, OUTB);
            if (rc == 0)
                goto restore;
        }
    }
    {
        int32_t k = (int32_t)strlen(OUTB);
        c->out_left -= k;
        if (c->out_left <= 0) {
            rc = -1;
        } else {
            strcat(c->out, OUTB);
            c->out += k;
        }
    }
restore:
    *fe->tend = save_t;
    *fe->wend = save_w;
    switch (rc) {
    case -1:
        return 1;
    case 0:
        return 0;
    case 1:
        c->src += fe->tend - c->in;
        c->status = 4;
        c->in = fe->tend;
        return 0;
    case 2:
        c->src += fe->wend - c->in;
        c->status = 4;
        c->in = fe->wend;
        return 0;
    case 3:
        c->src += fe->next - c->in;
        c->status = 4;
        c->in = fe->next;
        return 0;
    default:
        c->src += fe->tend - c->in;
        c->in = fe->tend;
        return 0;
    }
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
        while (*p != ' ') {
            for (const unsigned char *t = table; t < table + 16; t += 2) {
                if (t[0] != *p || t[-1] != p[-1])
                    continue;
                at = p;
                matches++;
                /* 0x1c407335..0x1c40735d: the first vowel pair may
                 * extend a diphthong. After a match the original advances
                 * both cursors, then decrements p again after the table. */
                if (matches >= 2 &&
                    (t != table || matches != 2 || prev - p != 2)) {
                    end = p;
                    goto insert_stress;
                }
                prev = p;
                p--;
            }
            p--;
        }
        if (matches == 0)
            return 0;
        end = at;
    } else {
        unsigned char *p = end;
        for (;;) {
            for (const unsigned char *t = table; t < table + 16; t += 2) {
                if (t[0] == *p && t[-1] == p[-1]) {
                    end = p;
                    goto insert_stress;
                }
            }
            if (*--p == ' ')
                return 0;
        }
    }

insert_stress:
    if (fe->cls[end[1]] & 2)   /* already has a stress mark right after it */
        return 0;
    if (c->out_left < 1)
        return 1;
    c->out_left--;
    {
        unsigned char *d = end + 1;
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
        s = ST(rd32(fe, 0x1c40ef58u + 8u * (uint32_t)ch));
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
        if (numbers(fe))
            return 1;
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
            return default_stress(fe);
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
