/*
 * textphon_eng.c — the text front end: TIBASE32's SVTextToPhon and the
 * English module's word translator (TIENG32 vtable +0x0C).
 *
 * PROVENANCE
 * ----------
 * Transliterated from the disassembly, function by function:
 *
 *   TIBASE32 0x1c00fc50  _SVTextToPhon@24   normalise, then one call per word
 *   TIBASE32 0x1c0100d0  "{{...}}" passthrough
 *   TIBASE32 0x1c00f970  _SVTTS@32          the retry/chunk loop around it
 *   TIENG32  0x1c2067c0  vtable +0x0C       per-word driver
 *   TIENG32  0x1c207230  exception dictionary matcher
 *   TIENG32  0x1c208150  number/token normaliser, with its pattern handlers
 *                        0x1c208850 0x1c208a80 0x1c208c90 0x1c208f20
 *                        0x1c208ff0 0x1c209120 0x1c2091f0 0x1c209290
 *                        0x1c209380 0x1c209580
 *   TIENG32  0x1c207890  digits to words;  0x1c2080d0 "MILLION" etc. follows
 *   TIENG32  0x1c2077b0  default stress
 *   TIENG32  0x1c209a00  spell mode (flags & 4)
 *   TIENG32  0x1c206860  letter-to-sound rules: src/ruleset.c, sv_rules_step
 *
 * The word translator keeps its working buffers in module globals: the token
 * pattern (0x1c250130), the output being built (0x1c24ed90) and a scratch
 * number (0x1c24ed88). They live in the module's mutable .data copy, the one
 * the frame generator uses, so a leftover from one call is seen by the next
 * exactly as in the original. Pointer tables (vocabulary, rule buckets) hold
 * original VAs and are resolved through the extracted images.
 *
 * The C runtime calls the original makes (_isctype, toupper, strspn, ...) run
 * in the "C" locale there; ct() below is that locale's _pctype table.
 *
 * The user dictionary (TIENG32 0x1c2097a0, TIBASE32's _SVLoadUserDictionary@8
 * / _SVUnloadUserDictionary@8) is reconstructed in userdict.c / userdict.h;
 * sv_text_to_phon_ex() below takes an optional dictionary and lang_word()
 * consults it at the same point the original does, before the built-in
 * exception dictionary. sv_text_to_phon() is unchanged — it is
 * sv_text_to_phon_ex(..., NULL) — so behaviour without a dictionary stays
 * byte-identical.
 */

#include "langmod_priv.h"
#include "tispeech/ruleset.h"
#include "tispeech/userdict.h"

#include <stdlib.h>
#include <string.h>

/* The original's context block, TIBASE32 0x1c0141e0. */
typedef struct {
    const char *src;   /* +0x00 cursor in the caller's text */
    char *out;         /* +0x04 end of the phoneme string (on its NUL) */
    char *in;          /* +0x08 cursor in the normalised text */
    char *base;        /* +0x0c the normalised text */
    char *word;        /* +0x10 start of the current word */
    int32_t out_left;  /* +0x14 */
    uint32_t opts;     /* +0x18 SVTextToPhon's flags */
    uint32_t status;   /* +0x1c */
} tp_ctx;

typedef struct {
    sv_langgen *g;
    tp_ctx c;
    uint16_t cls[256];                  /* 0x1c209c20, the rule class table */
    const unsigned char *buckets[256];  /* 0x1c24c744, resolved */
    sv_ruleset_t rules;
    const sv_userdict_t *dict;          /* optional; NULL = none loaded */
    /* FUN_1c208150's token pointers, 0x1c250120..0x1c250128. */
    char *tok;   /* 0x1c250128 */
    char *next;  /* 0x1c25011c: the token's end, then the next word */
    char *wend;  /* 0x1c250120 */
    char *tend;  /* 0x1c250124 */
} fe_t;

#undef G
#define G (fe->g)

/* Working buffers in .data. */
#define PAT  ((char *)DP(0x1c250130))  /* token pattern, "#,###.##" */
#define OUTB ((char *)DP(0x1c24ed90))  /* phonemes being built */
#define TMP  ((char *)DP(0x1c24ed88))  /* one number of h:m:s */

/* The "C" locale's _pctype: _UPPER 1, _LOWER 2, _DIGIT 4, _SPACE 8,
 * _PUNCT 0x10, _CONTROL 0x20, _BLANK 0x40, _HEX 0x80, _ALPHA 0x100.
 * Nothing above 0x7f is classified. */
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

/* A 32-bit word of .data, or 0 outside it. */
static uint32_t rd32(const fe_t *fe, uint32_t va)
{
    if (va - G->dva > G->dsize - 4)
        return 0;
    return sv_rd32(G->d + (va - G->dva));
}

/* The string or rule text at an original VA. Every address the original
 * dereferences resolves; anything else (only reachable from input the
 * original itself would crash on) reads as empty. */
static const char *str_at(const fe_t *fe, uint32_t va)
{
    const uint8_t *p;
    if (va - G->dva < G->dsize)
        return (const char *)(G->d + (va - G->dva));
    p = sv_tp_ptr(G, va);
    return p ? (const char *)p : "";
}

/* *(char **)va, i.e. a vocabulary table entry. */
#define SP(va) str_at(fe, rd32(fe, (uint32_t)(va)))
#define ST(va) str_at(fe, (uint32_t)(va))

/* The word tables. ONES is 0..19 followed by the stressed tens (so
 * ONES(20 + t) == the 0x1c24d018 table); TENS is unstressed. */
#define ONES(i)  SP(0x1c24cfc8 + 4 * (int32_t)(i))
#define TENS(i)  SP(0x1c24d040 + 4 * (int32_t)(i))
#define STENS(i) SP(0x1c24d018 + 4 * (int32_t)(i))
#define DIGIT(c) SP(0x1c24cf08 + 4 * (int32_t)(c))   /* indexed by char code */
#define SCALE(i) SP(0x1c24cf68 + 4 * (int32_t)(i))   /* HUNDRED, THOUSAND, ... */

/* strcat into one of the .data buffers. The original has no bound; this
 * one stops at the end of the .data copy instead of running past it. */
static void cat(fe_t *fe, char *dst, const char *src)
{
    char *end = (char *)G->d + G->dsize - 1;
    char *d = dst + strlen(dst);
    if ((uint8_t *)dst < G->d || dst > end) {
        strcat(dst, src);  /* not a .data buffer: the caller bounded it */
        return;
    }
    while (*src && d < end)
        *d++ = *src++;
    *d = '\0';
}

/* ------------------------------------------------------------------------ */
/* Numbers.                                                                  */
/* ------------------------------------------------------------------------ */

/* 0x1c2080d0: does the next word scale the number? */
static int scale_follows(fe_t *fe, const char *next)
{
    if (!next)
        return 0;
    if (!strncmp(next, ST(0x1c24ca24), 7) || !strncmp(next, ST(0x1c24ca1c), 7)
        || !strncmp(next, ST(0x1c24ca10), 8))
        return 1;
    return !strncmp(next, ST(0x1c24ca04), 11);
}

/* 0x1c207890: a number as words ("1,250.5", "$3.20", "12%"). Validated
 * against the token's pattern in PAT. Returns 0 when the token is not a
 * well-formed number, 1 when it was said, 2 when the scale word that follows
 * ("million") was said with it. */
static int n2w(fe_t *fe, char *tok, const char *next, char *out)
{
    int32_t len = (int32_t)strlen(tok);
    int dollar = *tok == '$';
    int percent, scale, one_dollar, cents_ok, valid, run, commas, last_zero;
    int32_t int_len, frac, rem, k;
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
    int_len = (int32_t)strcspn(tok, ST(0x1c24ca00));
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

    /* Digit groups, walked backwards over the pattern: every comma must close
     * exactly three digits, and the leading group has at most three. */
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

    /* The original tests a stack slot here that only the group loop sets;
     * with no integer part it is stale. It only decides whether a ',' in the
     * output is cut, and there is none on the paths that reach it with no
     * integer part (".5", "$.50"), so 0 stands in for it. */
    last_zero = 0;
    p = tok;
    rem = int_len;
    if (rem > 0) {
        do {
            int32_t n = (int32_t)strcspn(p, ST(0x1c24c9fc));
            int32_t g = n >= 3 ? 3 : n;
            int32_t h = 0, t = 0, u = 0;
            switch (g) {
            case 3: h = (signed char)*p++ - '0'; /* fall through */
            case 2: t = (signed char)*p++ - '0'; /* fall through */
            case 1: u = (signed char)*p++ - '0'; break;
            }
            if (h) {
                cat(fe, out, ONES(h));
                cat(fe, out, ST(0x1c24c9f0));   /* "/HUN5DRIHD " */
            }
            if (t >= 2) {
                cat(fe, out, STENS(t));
                if (u)
                    cat(fe, out, ONES(u));
            } else if (t * 10 + u) {
                cat(fe, out, ONES(t * 10 + u));
            }
            last_zero = (uint32_t)(h + t + u) < 1;
            if (last_zero) {
                /* A zero group: said as "zero" per digit only when it is the
                 * last one; "1,000" says "one thousand, zero zero zero" and
                 * the cut below takes it back to "one thousand". */
                if (rem <= 3)
                    while (rem-- != 0)
                        cat(fe, out, ST(0x1c24c9e4));   /* "ZIH5ROW " */
                commas--;
            } else if (commas) {
                cat(fe, out, SCALE(commas));
                commas--;
                cat(fe, out, ST(0x1c24c9e0));   /* ", " */
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
            cat(fe, out, ST(0x1c24c9d4));   /* "DAA5LER " */
        else if (int_len)
            cat(fe, out, ST(0x1c24c9c8));   /* "DAA5LERZ " */
        if (frac == 2) {
            int32_t d1 = (signed char)p[0] - '0', d2 = (signed char)p[1] - '0';
            int32_t cents = d1 * 10 + d2;
            p += 2;
            if (cents) {
                if (int_len)
                    cat(fe, out, ST(0x1c24c9c0));   /* "AEND " */
                if (d1 < 2) {
                    cat(fe, out, ONES(cents));
                } else {
                    cat(fe, out, STENS(d1));
                    if (d2)
                        cat(fe, out, ONES(d2));
                }
                cat(fe, out, ST(cents == 1 ? 0x1c24c9b8 : 0x1c24c9ac));
            }
        }
    } else if (frac) {
        cat(fe, out, ST(0x1c24c9a4));   /* "POY5NT " */
        while (frac-- != 0)
            cat(fe, out, DIGIT((signed char)*p++));
    }

    {
        int ret = 1;
        if (scale) {
            switch (c_toupper((unsigned char)*next)) {
            case 'B': cat(fe, out, ST(0x1c24c98c)); break;
            case 'M': cat(fe, out, ST(0x1c24c998)); break;
            case 'Q': cat(fe, out, ST(0x1c24c970)); break;
            case 'T': cat(fe, out, ST(0x1c24c980)); break;
            }
            ret = 2;
        }
        if (dollar && (scale || !cents_ok))
            cat(fe, out, ST(0x1c24c9c8));   /* "DAA5LERZ " */
        if (scale && strchr(next, ')'))
            cat(fe, out, ST(0x1c24c9e0));
        if (percent) {
            cat(fe, out, ST(0x1c24c964));   /* "PERSEH5NT " */
            *lastp = '%';
        }
        return ret;
    }
}

/* 0x1c209580: digit strings. Parentheses are said as pauses; a number is
 * tried first; otherwise each character is spelled. On a character it
 * cannot spell it stores that position through `pnext` and returns 3, so the
 * caller resumes there. */
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
        cat(fe, out, ST(0x1c24c9e0));
    }
    rc = n2w(fe, tok, NULL, out);
    *last = (char)save;
    if (rc) {
        if (save == ')')
            cat(fe, out, ST(0x1c24c9e0));
        return rc;
    }
    for (k = len; k != 0; k--) {
        unsigned char x = (unsigned char)*tok++;
        const char *s;
        if (ct(x) & 4) {
            s = DIGIT(x);
        } else {
            switch (x) {
            case '$': s = ST(0x1c24c9d4); break;   /* DAA5LER */
            case '+': s = ST(0x1c24cc80); break;   /* PLAH5S */
            case ',': s = ST(0x1c24cc38); break;   /* "- " */
            case '-': s = ST(0x1c24cc88); break;   /* DAE5SH */
            case '.': s = ST(0x1c24c9a4); break;   /* POY5NT */
            case '/': s = ST(0x1c24cc90); break;   /* SLAE5SH */
            case ':': s = ST(0x1c24cc74); break;   /* KOW5LUN */
            default:
                *pnext = tok - 1;
                return 3;
            }
        }
        cat(fe, out, s);
    }
    return 1;
}

/* 0x1c209290: three digits, "5 0 0" as "five hundred". */
static int three_digits(fe_t *fe, const char *s, char *out)
{
    int32_t d0 = (signed char)s[0] - '0';
    int32_t d1 = (signed char)s[1] - '0';
    int32_t d2 = (signed char)s[2] - '0';
    cat(fe, out, ONES(d0));
    if (d0 && !d1 && !d2) {
        cat(fe, out, ST(0x1c24cc58));   /* "/HAH5NDRIHD " */
    } else {
        cat(fe, out, ONES(d1));
        cat(fe, out, ONES(d2));
    }
    return 1;
}

/* 0x1c208ff0: "###-####". */
static int h_local(fe_t *fe, char *tok, char *out)
{
    cat(fe, out, DIGIT((signed char)tok[0]));
    cat(fe, out, DIGIT((signed char)tok[1]));
    cat(fe, out, DIGIT((signed char)tok[2]));
    cat(fe, out, ST(0x1c24c9e0));
    cat(fe, out, DIGIT((signed char)tok[4]));
    return three_digits(fe, tok + 5, out);
}

/* 0x1c209120: "#-###-###-####". */
static int h_long_distance(fe_t *fe, char *tok, char *out)
{
    cat(fe, out, DIGIT((signed char)tok[0]));
    cat(fe, out, ST(0x1c24cbd4));   /* "," */
    three_digits(fe, tok + 2, out);
    cat(fe, out, ST(0x1c24cbd4));
    h_local(fe, tok + 6, out);
    return 1;
}

/* 0x1c2091f0: "###-###-####" and "###/###-####". */
static int h_area_code(fe_t *fe, char *tok, char *out)
{
    cat(fe, out, ST(0x1c24cc44));   /* "EH5RIYAH2 KOW5D " */
    three_digits(fe, tok, out);
    cat(fe, out, ST(0x1c24cbd4));
    h_local(fe, tok + 4, out);
    return 1;
}

/* 0x1c208f20: "(###)", when a number follows. */
static int h_paren_area(fe_t *fe, char *tok, const char *next, char *out)
{
    if (!next || !(ct((unsigned char)*next) & 4))
        return 0;
    cat(fe, out, ST(0x1c24cc44));
    three_digits(fe, tok + 1, out);
    cat(fe, out, ST(0x1c24cbd4));
    return 1;
}

/* 0x1c208850: "h:mm" on a 12-hour clock. */
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
        cat(fe, out, ST(0x1c24cc04));   /* "OWKLAA5K " */
    } else if (d1 == 0) {
        cat(fe, out, ST(0x1c24cc10));   /* "OW4 " */
        cat(fe, out, ONES(d2));
    } else if (d1 == 1) {
        cat(fe, out, ONES(m));
    } else {
        cat(fe, out, TENS(d1));
        if (d2)
            cat(fe, out, ONES(d2));
    }
    return 1;
}

/* One number of a date: below 20 as a word, otherwise tens and units, the
 * units said even when zero. */
static void date_part(fe_t *fe, int32_t n, char *out)
{
    if (n < 0x14) {
        cat(fe, out, ONES(n));
    } else {
        cat(fe, out, TENS(n / 10));
        cat(fe, out, ONES(n % 10));
    }
}

/* 0x1c208a80: "m/d/yy". */
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

/* 0x1c208c90: "h:mm:ss" as hours, minutes and seconds. Each number goes
 * through n2w with PAT forced to digits, and its result is not checked. */
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
    cat(fe, out, ST(0x1c24cc3c));   /* "AW5ER" */
    cat(fe, out, ST(v == 1 ? 0x1c24cc38 : 0x1c24cc34));

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
    cat(fe, out, ST(0x1c24cc28));   /* "MIH5NAXT" */
    cat(fe, out, ST(v == 1 ? 0x1c24cc38 : 0x1c24cc24));

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
    cat(fe, out, ST(0x1c24cc18));   /* "SEH5KUND" */
    cat(fe, out, ST(v == 1 ? 0x1c24cc38 : 0x1c24cc34));
    return 1;
}

/* 0x1c209380: "####" as a year. */
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
    if (b * 10 + c == 0) {
        cat(fe, out, ONES(a));
        cat(fe, out, ST(0x1c24cc68));   /* "THAW5ZIHND " */
        if (d)
            cat(fe, out, ONES(d));
        return 1;
    }
    if (a == 1) {
        cat(fe, out, ONES(a * 10 + b));
    } else {
        cat(fe, out, TENS(a));
        if (b)
            cat(fe, out, ONES(b));
    }
    if (c * 10 + d == 0) {
        cat(fe, out, ST(0x1c24cc58));   /* "/HAH5NDRIHD " */
    } else if (c == 1) {
        cat(fe, out, ONES(c * 10 + d));
    } else {
        cat(fe, out, TENS(c));
        if (d)
            cat(fe, out, ONES(d));
    }
    return 1;
}

/* The pattern table at 0x1c24cae8 names its handlers by address. */
static int call_handler(fe_t *fe, uint32_t fn, char *tok, char *next, char *out)
{
    switch (fn) {
    case 0x1c209120: return h_long_distance(fe, tok, out);
    case 0x1c2091f0: return h_area_code(fe, tok, out);
    case 0x1c208ff0: return h_local(fe, tok, out);
    case 0x1c209380: return h_year(fe, tok, out);
    case 0x1c208c90: return h_duration(fe, tok, out);
    case 0x1c208850: return h_time(fe, tok, out);
    case 0x1c208f20: return h_paren_area(fe, tok, next, out);
    case 0x1c208a80: return h_date(fe, tok, out);
    case 0x1c209580: {
        /* Called through the table, 0x1c209580 gets `next` itself where it
         * expects a pointer to it, and would store a pointer into the text
         * there. The two patterns routed to it ("#####", "#####-####") are
         * all digits and '-', which it always spells, so the store is never
         * reached. */
        char *unused = next;
        return spell_digits(fe, tok, &unused, out);
    }
    default:
        return 0;
    }
}

/* 0x1c208150: numbers and other digit tokens at the cursor. Returns 1 when
 * the output filled up, else 0; status 4 means the token was said. */
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
    fe->next = tok + strspn(tok, ST(0x1c24cbec));  /* "0123456789,.$%+-()/:" */
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
            s = 0x1c24cbe8;   /* "L" */
        } else if (ct(x) & 4) {
            s = 0x1c24cbe4;   /* "#" */
            keep = 1;
        } else {
            switch (x) {
            case '$': s = 0x1c24cbd0; keep = 1; break;
            case '%': s = 0x1c24cbcc; keep = 1; break;
            case ',': s = 0x1c24cbd4; keep = 1; break;
            case '.': s = 0x1c24ca00; keep = 1; break;
            case '(': s = 0x1c24cbe0; break;
            case ')': s = 0x1c24cbdc; break;
            case '-': s = 0x1c24cbd8; break;
            case '/': s = 0x1c24cbc8; break;
            case ':': s = 0x1c24cbc4; break;
            default: s = 0x1c24cbe8; break;
            }
        }
        cat(fe, PAT, ST(s));
        if (!keep)
            plain = 0;
    }

    OUTB[0] = '\0';
    cat(fe, OUTB, ST(0x1c24cbc0));   /* " " */
    rc = 0;
    for (e = 0x1c24cae8; rd32(fe, e + 8) != 0; e += 12)
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

/* The exception rules' bucket for the character at the cursor. */
static const unsigned char *exc_bucket(fe_t *fe, unsigned char ch, int *fb)
{
    uint32_t va;
    uint16_t k = fe->cls[ch];
    if ((k & 0x80) && ch <= 0x7a) {
        va = rd32(fe, 0x1c24c7cc + 4 * (int32_t)(signed char)ch);
    } else {
        va = rd32(fe, (!(k & 0x80) && (k & 2)) ? 0x1c24c938 : 0x1c24c93c);
        *fb = 1;
    }
    return (const unsigned char *)sv_tp_ptr(G, va);
}

/* 0x1c207230: the exception dictionary, " [WORD] =PHONEMES\". Buckets are
 * sorted on the second letter and end at a "[*" entry. Unlike the letter-to-
 * sound rules, ' ' in a context does not skip whitespace, '+' is any vowel,
 * and output is appended at the output's end without moving ctx.out past the
 * word until the matcher stops. Returns 1 when the output filled up. */
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
        while (*rp != '[') {
            if (!*rp && !sv_tp_ptr(G, 0)) { /* keep scanning: rules have no NULs */ }
            rp++;
        }
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
                    /* The original's switch has no case for the remaining
                     * metacharacters and would spin on this one forever. */
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

        /* r is on the '=' that ends the right context. */
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
        continue;   /* ip is past the literal */

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

/* 0x1c2077b0: a word that came out of the rules with no primary stress gets
 * '5' after its first vowel. */
static int default_stress(fe_t *fe)
{
    tp_ctx *c = &fe->c;
    char *d = c->out;
    const char *pairs = (const char *)DP(0x1c24c940);
    int npairs = (c->opts & 2) ? 0x12 : 0x10;
    int i;
    char carry;

    if (*d != ' ') {
        do {
            unsigned char b = (unsigned char)*d;
            if ((fe->cls[b] & 2) && b != '1' && b != '2')
                return 0;
            d--;
        } while (*d != ' ');
    }
    for (d++; *d; d++)
        for (i = 0; i < npairs; i++)
            if (pairs[2 * i] == d[0] && pairs[2 * i + 1] == d[1])
                goto found;
    return 0;
found:
    d += 2;
    if (fe->cls[(unsigned char)*d] & 2)
        return 0;
    if (c->out_left < 1)
        return 1;
    c->out_left--;
    carry = '5';
    while (*d) {
        char t = *d;
        *d++ = carry;
        carry = t;
    }
    *d++ = carry;
    *d = '\0';
    c->out = d;
    return 0;
}

/* 0x1c209a00: spell mode, one table entry per character. */
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
        s = SP(0x1c24d318 + 8 * (uint32_t)ch);
        if (ch == '.') {
            if (ct(nx) & 4)
                s = ST(0x1c24c9a4);          /* "POY5NT " */
            else if (!(ct(nx) & 8) && nx != 0)
                s = ST(0x1c24d718);
        } else if (ch == '?') {
            if ((ct(nx) & 8) || nx == 0)
                s = ST(0x1c24d260);
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

/* Vtable +0x0C, 0x1c2067c0: one word. */
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
        /* 0x1c2097a0, the user dictionary (userdict.c). Consulted first, and
         * only when one is loaded — with fe->dict NULL this is a no-op, so
         * behaviour is unchanged from before this hook existed. */
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
            default_stress(fe);
        return 0;
    }
}

/* TIBASE32 0x1c0100d0: "{{text}}" goes to the output as "{text}", lower-
 * cased. */
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
/* Entry points.                                                             */
/* ------------------------------------------------------------------------ */

/* Bytes of zeroes around the normalised text. The original's buffer is a
 * calloc block exactly one byte longer than it needs; the matchers read a
 * few bytes either side of it (context walks, the '%' suffix look-ahead),
 * which there land in heap bookkeeping. Here they land in zeroes. */
#define SLACK 16

static int fe_init(fe_t *fe, const sv_langmod *m)
{
    int i;
    memset(fe, 0, sizeof *fe);
    fe->g = (sv_langgen *)m->priv;
    if (!fe->g || !sv_tp_ptr(fe->g, 0x1c209c20) || !sv_tp_ptr(fe->g, 0x1c209e21 + 0x200))
        return -1;
    for (i = 0; i < 256; i++)
        fe->cls[i] = (uint16_t)(sv_tb(fe->g, 0x1c209c20 + 2 * i)
                                | sv_tb(fe->g, 0x1c209c21 + 2 * i) << 8);
    /* 0x1c206860 indexes 0x1c24c744 by the letter for 'A'..'z' and has
     * cases for four Latin-1 letters; everything else is the fallback. */
    for (i = 0; i <= 0x7a; i++)
        if (fe->cls[i] & 0x80)
            fe->buckets[i] = sv_tp_ptr(fe->g, rd32(fe, 0x1c24c744 + 4 * i));
    fe->buckets[0xc4] = sv_tp_ptr(fe->g, rd32(fe, 0x1c24c8b0));
    fe->buckets[0xd6] = sv_tp_ptr(fe->g, rd32(fe, 0x1c24c8b4));
    fe->buckets[0xdc] = sv_tp_ptr(fe->g, rd32(fe, 0x1c24c8bc));
    fe->buckets[0xdf] = sv_tp_ptr(fe->g, rd32(fe, 0x1c24c8b8));
    fe->rules.charclass = fe->cls;
    fe->rules.buckets = fe->buckets;
    fe->rules.fallback = sv_tp_ptr(fe->g, rd32(fe, 0x1c24c8c4));
    fe->rules.qmark_sub = B(0x1c24c8c8);
    return fe->rules.fallback ? 0 : -1;
}

/* Which Latin-1 letters SVTextToPhon keeps, 0x1c010074: the rest of
 * 0xa2..0xfc become spaces, and three lower-case umlauts are upper-cased. */
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

int32_t sv_text_to_phon_ex(const sv_langmod *m, const char *text, char *out,
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
    /* calloc(strlen + 3) with the size held in 16 bits, signed. Past that
     * the original's buffer wraps or the allocation fails. */
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

    /* Normalise: upper-case, keep what the rules can read, blank the rest. */
    buf[0] = ' ';
    w = buf + 1;
    if (tl > 0x202)
        *w++ = '\0';   /* sic: longer input is cut off entirely */
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
            /* Word index marks, "@w<offset>", from the "@w" template at
             * TIBASE32 0x1c014210. */
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
        if ((rc = lang_word(fe)) != 0)
            break;
        for (p = c->in; *p == ' '; p++)
            ;
    }
    if (rc == 1) {
        ret = -1 - ofs;
        /* Cut the output back to where the word that did not fit began. On
         * the very first word the original writes through an uninitialised
         * pointer instead; the output is empty then anyway. */
        if (mark)
            *mark = '\0';
    }
    free(block);
    free(fe);
    return ret;
}

int32_t sv_text_to_phon(const sv_langmod *m, const char *text, char *out,
                        int32_t out_size, uint32_t flags)
{
    return sv_text_to_phon_ex(m, text, out, out_size, flags, NULL);
}

int32_t sv_tts_phonemes(const sv_langmod *m, const char *text, uint32_t flags,
                        char **phonemes)
{
    return sv_tts_phonemes_ex(m, text, flags, NULL, phonemes);
}

int32_t sv_tts_phonemes_ex(const sv_langmod *m, const char *text,
                           uint32_t flags, const sv_userdict_t *dict,
                           char **phonemes)
{
    /* _SVTTS@32 up to its SVNarrate call. The phoneme buffer starts at
     * (strlen + 10) * 3 bytes, or * 6 in spell mode. When SVTextToPhon fills
     * it partway, what fit is kept and the rest of the text goes to a new
     * buffer; when not even the first word fits, the buffer is dropped and
     * the multiplier grows by 2. The chunks are then joined. */
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
        rc = sv_text_to_phon_ex(m, text, b, size, flags, dict);
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
