/*
 * narrate.c — TIBASE32's phoneme-string -> record -> frame pipeline.
 *
 * Reconstructed from FUN_1c003870 and everything it calls up to the frame
 * generator; see include/tispeech/narrate.h for the call order. Each function
 * names its address. The code follows the original's control flow closely,
 * including its pointer walks over the record and intonation arrays, because
 * the arithmetic is only reproducible if the walk is: several loops run past
 * the element they are "about" and the result depends on exactly where they
 * stop.
 *
 * Integer conventions, as in dsp.c and generator.c: `idiv` is C's `/` on int
 * (both truncate toward zero); `sar` is `>>` on a signed int; byte stores
 * truncate through uint8_t.
 */

#include "tispeech/narrate.h"
#include "tispeech/expression.h"
#include "tispeech/smoothing.h"

#include <stdlib.h>
#include <string.h>

static uint32_t ph_attrs(const uint8_t *phonemes, uint16_t code)
{
    return sv_rd32(phonemes + (ptrdiff_t)(int16_t)code * SV_PH_STRIDE + 2);
}

/* ------------------------------------------------------------------------ */
/* Voice setup                                                               */
/* ------------------------------------------------------------------------ */

/* 0x1c00e120. The row-to-block copy is not a straight memcpy: row +0x06 goes
 * to block +0x48, and two runs of three words swap places. */
void sv_voice_load(sv_engine *e, const uint8_t *voices, unsigned row)
{
    const uint8_t *r = voices + row * 74;
    uint16_t *v = e->voice;
#define VB(off, roff) (v[(off) / 2] = sv_rd16(r + (roff)))
    VB(0x04, 0x04);
    for (unsigned k = 0x06; k <= 0x26; k += 2)
        VB(k, k + 2);
    VB(0x2e, 0x2a); VB(0x30, 0x2c); VB(0x32, 0x2e);
    VB(0x28, 0x30); VB(0x2a, 0x32); VB(0x2c, 0x34);
    for (unsigned k = 0x34; k <= 0x46; k += 2)
        VB(k, k + 2);
    VB(0x48, 0x06);
#undef VB
}

/* 0x1c00ecd0. */
int sv_engine_set_speaking_mode(sv_engine *e, uint32_t value)
{
    if (!(value & 7))
        return SV_TP_E_BADARG;
    if (value & 2)
        e->handle_flags |= 0x10;
    else
        e->handle_flags &= ~0x10u;
    return 0;
}

/* 0x1c00df20, plus the flag handling _SVNarrate@20 does around it
 * (0x1c003830..0x1c003854). 0x1c00df33 copies handle+0xcc (e->handle_flags
 * here) into state+0x4e verbatim; the preamble then ORs in the per-call
 * `flags` argument and the two hardcoded bits. Folded into one assignment
 * since nothing ever reads the intermediate value. */
void sv_narrate_begin(sv_engine *e, const char *text, uint32_t flags)
{
    const uint16_t *v = e->voice;
#define VW(off) (v[(off) / 2])
    e->flags = e->handle_flags | flags | 0x80000000u;
    if (!(e->flags & 0x40000000u))
        e->flags |= 0x20000000u;
    e->text = text;
    e->st_44 = VW(0x06);
    e->st_46 = VW(0x04);
    e->st_2b8 = VW(0x04);
    e->sample_rate = 11025;
    e->st_4c = VW(0x10);
    e->st_60 = VW(0x12);
    e->st_62 = VW(0x14);
    e->slew_rise = VW(0x18);
    e->slew_fall = VW(0x1a);
    e->st_52 = VW(0x08);
    e->st_2f2 = VW(0x0c);
    e->st_8a = VW(0x0e);
    e->st_5c = VW(0x24);
    e->st_5e = VW(0x26);
    e->rate = VW(0x22);
    e->st_5a = VW(0x0a);
    e->chunk_length = 0;
    e->st_84 = VW(0x28); e->st_86 = VW(0x2a); e->st_88 = VW(0x2c);
    e->st_7e = VW(0x2e); e->st_80 = VW(0x30); e->st_82 = VW(0x32);
    e->st_8c = VW(0x3a); e->st_8e = VW(0x3c); e->st_90 = VW(0x3e);
    e->st_92 = VW(0x34); e->st_94 = VW(0x36); e->st_96 = VW(0x38);
    e->st_98 = VW(0x40);
    e->st_9c = VW(0x44);
    e->st_9a = VW(0x42);
    e->st_9e = VW(0x46);
    e->st_ac = VW(0x48);
#undef VW
    e->st_ae = 0;
    e->st_a8 = 0x50;
    e->st_aa = 0x2ee;
    e->st_6e = 0;
    e->st_72 = 0;
    e->st_74 = 0;
    e->st_68 = 0x41;
    e->st_6c = 0x0a;
    e->pause_short = 0x60;
    e->pause_long = 0x8c;
    e->pause_comma = 0xc8;
    e->st_312 = 0xff;
    e->records = NULL;
}

/* ------------------------------------------------------------------------ */
/* 0x1c00c500 — sentence chunking                                            */
/* ------------------------------------------------------------------------ */
int sv_next_sentence(sv_engine *e)
{
    const char *p = e->text + e->chunk_length;
    int32_t n = 0, counted = 0;
    int in_text = 1;
    e->text = p;
    for (;;) {
        char c = *p++;
        if (c == 0)
            break;
        n++;
        if (c == '@')
            in_text = 0;
        if (c == ' ')
            in_text = 1;
        if (in_text)
            counted++;
        if (c == '.' || c == '?')
            break;
    }
    e->chunk_length = n;
    e->chunk_phonemes = counted;
    return n;
}

/* ------------------------------------------------------------------------ */
/* 0x1c005720 — signed decimal after an '@w' number marker                   */
/* ------------------------------------------------------------------------ */
static int16_t parse_number(const char *s)
{
    int neg = 0;
    uint16_t v = 0;
    unsigned char c;
    if (*s == 0)
        return (int16_t)0x8000;
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    for (;;) {
        c = (unsigned char)*s++;
        if (c < '0' || c > '9')
            break;
        v = (uint16_t)(v * 10 + (int16_t)(signed char)c - 0x30);
    }
    if (neg)
        v = (uint16_t)-v;
    if (c == ';' || c == '}' || c == ' ')
        return (int16_t)v;
    if (c == '%' && !neg)
        return (int16_t)-v;
    return (int16_t)0x8000;
}

/* ------------------------------------------------------------------------ */
/* 0x1c005180 — inline `{...}` command parser                                */
/*                                                                           */
/* Syntax: `{word}`, `{word arg}` or `{word arg extra}`; several commands in  */
/* one pair of braces are separated by `;`, and several pairs of braces in a  */
/* row chain automatically (optional spaces between `}` and the next `{`).    */
/* Each `word` emits one 3-word command into e->commands. A `word` is one of: */
/*                                                                           */
/*   - a decimal number (parse_number's syntax): command 0x28, value = the   */
/*     number, extra always 0, and (an original quirk kept as-is) the value  */
/*     is never checked against the malformed-number sentinel (0x1c005337).  */
/*   - a musical note, `[a-g]['#']digit`: command 0xb4, value =              */
/*     -(semitone_offset[letter] + octave*12 + sharp), extra = a second,     */
/*     space-separated word parsed as a number (0 if absent). `digit` must   */
/*     be 0..8 or this is not a note after all and falls through to the      */
/*     keyword table below (0x1c005381).                                    */
/*   - `p` directly followed by a digit, e.g. "p5": shorthand for "pitch 5"  */
/*     with no space -- the digits splice in as the keyword's own argument   */
/*     word (0x1c005404).                                                   */
/*   - one of SV_CMD_KEYWORD_COUNT named keywords (base->cmd_keywords, in    */
/*     table order, matched by case-sensitive PREFIX -- so shorter names     */
/*     must sit after every longer name sharing the same prefix, which is    */
/*     why "p" is entry 44 rather than entry 0). Most take a plain number as */
/*     their argument (0x1c00564e); eight of them (language, voice, tract,   */
/*     glot, voicing, f0style, speak, mouths, sentsync, syllsync, phonsync,  */
/*     allsyncs) instead match a NAMED argument against a per-command list    */
/*     (0x1c005480..0x1c005649). The other two `*sync` commands (wordsync,   */
/*     usync) take a plain number like everything else -- which four of the  */
/*     six get the named list is a fixed dispatch-table lookup in the        */
/*     original (0x1c0056a4, embedded in .text next to FUN_1c005180's own    */
/*     code, so not extracted as data); hardcoded here as four more cases,   */
/*     the same as every other fixed command code below.                    */
/*                                                                           */
/* Every word (number, note, or keyword) may be followed by a further,       */
/* space-separated word that becomes the command's `extra` field, always a   */
/* number (0 if absent). A word that fails to parse -- an unmatched keyword, */
/* an out-of-range value, or a value/extra of exactly -32768 (the same bit   */
/* pattern the parser uses as its own "malformed" sentinel, an ambiguity in  */
/* the original kept as-is) fails the WHOLE `{...}` run with SV_NAR_E_COMMAND; */
/* nothing after the last successfully-parsed word is written.               */
/* ------------------------------------------------------------------------ */

static uint16_t cmd_value(const char *s) /* FUN_1c0056e0: NULL is an error */
{
    return s ? (uint16_t)parse_number(s) : (uint16_t)0x8000;
}

static uint16_t cmd_extra(const char *s) /* FUN_1c005700: NULL means "0" */
{
    return s ? (uint16_t)parse_number(s) : 0;
}

/* 0x1c005230: up to three space-separated words starting at `p`, each ending
 * at a space, ';', '}' or NUL; hitting one of the latter three where a word
 * would otherwise start leaves it (and every word after it) NULL. Never
 * advances `p` -- each command's words are (re-)scanned from its own start. */
static void cmd_tokenize(const char *p, const char **w1, const char **w2,
                         const char **w3)
{
    const char **slot[3] = { w1, w2, w3 };
    *w1 = *w2 = *w3 = NULL;
    if (!p)
        return;
    for (int k = 0; k < 3; k++) {
        char c;
        for (;;) {
            c = *p++;
            if (!c || c == ';' || c == '}')
                return;
            if (c != ' ') {
                p--;
                break;
            }
        }
        *slot[k] = p;
        for (;;) {
            c = *p++;
            if (!c || c == ';' || c == '}')
                return;
            if (c == ' ')
                break;
        }
    }
}

/* 0x1c0052d0: given the start of a just-processed command, find the start of
 * the next one -- right after a ';' (another command in the SAME braces), or
 * right after a '{' that (modulo spaces) immediately follows a '}' (a
 * chained "{...}{...}" pair). Returns NULL when the whole run ends: NUL
 * reached with no ';' or '}' seen, or a '}' not followed by another '{'. */
static const char *cmd_advance(const char *p)
{
    if (!p)
        return NULL;
    for (;;) {
        char c = *p++;
        if (!c)
            return NULL;
        if (c == ';')
            return p;
        if (c == '}')
            break;
    }
    for (;;) {
        char c = *p++;
        if (!c)
            return NULL;
        if (c != ' ')
            return c == '{' ? p : NULL;
    }
}

/* 0x1c0057e0: the index of the first entry of `list` (length `n`) that is a
 * case-sensitive prefix of `token`; 0x8000 if `token` is NULL or nothing
 * matches. */
static uint16_t cmd_match(const char *token, const char *const *list, int n)
{
    if (!token)
        return 0x8000;
    for (int i = 0; i < n; i++) {
        size_t len = strlen(list[i]);
        if (strncmp(token, list[i], len) == 0)
            return (uint16_t)i;
    }
    return 0x8000;
}

/* 0x1c005310: interpret one word (plus its up-to-two following words) into a
 * 3-word command. Returns 0 on success (out[] filled), nonzero if malformed. */
static int cmd_one(const sv_engine *e, const char *w1, const char *w2,
                   const char *w3, int16_t out[3])
{
    const sv_nar_tables *t = e->base;

    if (!w1)
        return -1;

    if (w1[0] >= '0' && w1[0] <= '9') {
        /* 0x1c005337 */
        out[0] = 0x28;
        out[1] = (int16_t)cmd_value(w1);
        out[2] = 0;
        return 0;
    }

    if (w1[0] >= 'a' && w1[0] <= 'g') {
        /* 0x1c005381: a musical note, letter['#']octave. */
        int letter = w1[0] - 'a';
        int sharp = (w1[1] == '#') ? 1 : 0;
        int16_t octave = (int16_t)cmd_value(w1 + 1 + sharp);
        if (octave >= 0 && octave <= 8) {
            out[0] = 0xb4;
            out[1] = (int16_t)-(t->cmd_notes[letter] + octave * 12 + sharp);
            out[2] = (int16_t)cmd_extra(w2);
            return (uint16_t)out[2] == 0x8000 ? -1 : 0;
        }
        /* out of range: not a note after all -- fall through to the keyword
         * table, exactly as the original does (0x1c0053b0/1c0053b6). */
    }

    if (w1[0] == 'p' && w1[1] >= '0' && w1[1] <= '9') {
        /* 0x1c005404: "p5" == "pitch 5" with no space. */
        w3 = w2;
        w2 = w1 + 1;
    }

    for (int i = 0; i < SV_CMD_KEYWORD_COUNT; i++) {
        const char *name = t->cmd_keywords[i].name;
        size_t len = strlen(name);
        if (strncmp(w1, name, len) != 0)
            continue;
        uint16_t code = t->cmd_keywords[i].code;
        uint16_t value;
        out[0] = (int16_t)code;
        switch (code) {
        case 0x41:  value = cmd_match(w2, t->cmd_language, 3);  break;
        case 0x32:  value = cmd_match(w2, t->cmd_voice, 20);    break;
        case 0xc8:  value = cmd_match(w2, t->cmd_tract, 4);     break;
        case 0xd2:  value = cmd_match(w2, t->cmd_glot, 9);      break;
        case 0xdc:  value = cmd_match(w2, t->cmd_voicing, 3);   break;
        case 0xf0:  value = cmd_match(w2, t->cmd_f0style, 5);   break;
        case 0x212: value = cmd_match(w2, t->cmd_speak, 4);     break;
        case 0x320: /* mouths   */
        case 0x32a: /* sentsync */
        case 0x33e: /* syllsync */
        case 0x348: /* phonsync */
        case 0x35c: /* allsyncs */
            value = cmd_match(w2, t->cmd_onoff, 2);
            break;
        default: /* including wordsync (0x334) and usync (0x352) */
            value = cmd_value(w2);
            break;
        }
        out[1] = (int16_t)value;
        out[2] = (int16_t)cmd_extra(w3);
        return (value == 0x8000 || (uint16_t)out[2] == 0x8000) ? -1 : 0;
    }
    return -1; /* no keyword matched */
}

/* 0x1c005180: parse one or more `{...}` groups, `cur` already pointing right
 * after the FIRST '{' (its content), writing 3-word commands from `cmd`
 * onward. On success returns the advanced command pointer and sets
 * `*cur_out`/`*c_out` the way the main loop's own `c = *cur++` would, one
 * past the run (the caller still owns updating `remaining` -- the exact
 * count consumed is `*cur_out - cur`, matching what the original tracks by
 * hand). On failure returns NULL, leaves `*cur_out` at the start of the
 * offending word and does not touch `*c_out`. */
static int16_t *parse_inline_commands(const sv_engine *e, const char *cur,
                                      const char **cur_out, char *c_out,
                                      int16_t *cmd)
{
    const char *group = cur;
    if (!cur)
        return NULL;
    for (;;) {
        const char *w1, *w2, *w3;
        int16_t out[3];
        cmd_tokenize(group, &w1, &w2, &w3);
        if (cmd_one(e, w1, w2, w3, out)) {
            *cur_out = group;
            return NULL;
        }
        /* ours: parse()'s pool-sizing loop (0x1c004a1b..0x1c004a97, already
         * ported unchanged) reserves one slot per ';' and two per '}', which
         * covers one slot per command here plus a final SV_CMD_END -- this
         * cannot actually trip, but the file's convention is to guard the
         * write rather than trust the arithmetic silently. */
        if ((size_t)(cmd - e->commands) + 3 > 3 * e->command_capacity) {
            *cur_out = group;
            return NULL;
        }
        cmd[0] = out[0];
        cmd[1] = out[1];
        cmd[2] = out[2];
        cmd += 3;
        const char *next = cmd_advance(group);
        if (!next)
            break;
        group = next;
    }
    /* 0x1c0051f7..0x1c005219: scan from the last group's start to (and past)
     * the closing '}', then fetch the next character exactly like the main
     * loop's `c = *cur++` -- including its unconditional advance past a NUL
     * reached without ever finding '}', which the original does too. */
    const char *p = group;
    while (*p && *p != '}')
        p++;
    if (*p == '}')
        p++;
    *c_out = *p;
    *cur_out = p + 1;
    return cmd;
}

/* ------------------------------------------------------------------------ */
/* 0x1c004a10 — the phoneme-string parser                                    */
/* ------------------------------------------------------------------------ */

static void set_code(const sv_engine *e, sv_record *r, uint16_t code)
{
    r->code = code;
    r->attrs = ph_attrs(e->phonemes, code);
    r->lang = e->lang;
    r->phonemes = e->phonemes;
}

static void put_cmd(int16_t **cmd, int16_t code, int16_t value)
{
    (*cmd)[0] = code;
    (*cmd)[1] = value;
    (*cmd)[2] = 0;
    *cmd += 3;
}

static int parse(sv_engine *e)
{
    const char *text = e->text;
    const sv_langmod *lang = e->lang;
    int32_t pool = 10;
    const char *p;
    char c;

    /* 0x1c004a1b..0x1c004a97: size the command pool. A run of '*' costs
     * two and leaves the scan ON the character after the run, which is then
     * tested for '@' and read again — so '@' right after stars counts twice. */
    p = text;
    c = *p++;
    while (c) {
        if (c == ';')
            pool++;
        if (c == '}')
            pool += 2;
        if (c == '*') {
            pool += 2;
            do {
                c = *p++;
            } while (c == '*');
            p--;
        }
        if (c == '@')
            pool += 2;
        c = *p++;
    }
    free(e->commands);
    e->commands = calloc((size_t)pool, 3 * sizeof(int16_t));
    e->command_capacity = (size_t)pool;
    if (!e->commands)
        return SV_NAR_E_NOMEM;

    /* 0x1c004ac1..0x1c004b7e: five lead-in records. */
    sv_record *r = e->records;
    set_code(e, &r[0], 0);
    set_code(e, &r[1], 0);
    set_code(e, &r[2], 0x1b);
    r[2].duration = 0x50;
    set_code(e, &r[3], 0x1b);
    r[3].duration = 0x50;
    set_code(e, &r[4], 0);
    e->record_count = 5;
    r += 5;

    const char *cur = text;
    int16_t *cmd = e->commands;
    int16_t *pending = NULL;
    uint16_t emphasis = 0, number = 0;
    int number_pending = 0;
    int result = 0;
    unsigned code = 0;
    int32_t remaining = e->chunk_length;

    c = *cur++;
    while (remaining > 0) {
        int emit;
        if (c == '{') {
            if (e->flags & 0x80) {
                /* 0x1c004c01: skip the braces entirely. */
                remaining--;
                if (c) {
                    while (c != '}') {
                        c = *cur++;
                        remaining--;
                        if (!c)
                            break;
                    }
                }
                if (remaining <= 0)
                    break;
                c = *cur++;
                continue;
            }
            /* 0x1c004c4d: FUN_1c005180 parses the braces into commands. */
            if (!pending)
                pending = cmd;
            {
                const char *entry = cur;
                const char *new_cur;
                char new_c;
                int16_t *new_cmd = parse_inline_commands(e, cur, &new_cur, &new_c, cmd);
                if (!new_cmd) {
                    result = SV_NAR_E_COMMAND;
                    remaining = 0;
                    break;
                }
                remaining -= (int32_t)(new_cur - entry);
                cmd = new_cmd;
                cur = new_cur;
                c = new_c;
            }
            continue;
        }

        /* 0x1c004c85: phoneme name lookup. */
        int idx = (signed char)c - 0x20;
        if (idx < 0 || idx > 0x3b)
            idx = 1;
        const uint8_t *pair = e->base->phoneme_names[idx];
        if (!pair)
            return SV_NAR_E_NOTIMPL; /* '[': the original dereferences NULL */
        char next = *cur;
        int toggle;
        if (next != 'H') {
            toggle = 1;
        } else {
            const char *q = cur + 1;
            toggle = 0;
            do {
                toggle = !toggle;
            } while (*q++ == 'H');
        }
        uint8_t want = toggle ? (uint8_t)next : 0;
        uint8_t first = pair[0];
        const uint8_t *hit = pair;
        if (want != first) {
            while (*hit != 0) {
                hit += 2;
                if (*hit == want)
                    break;
            }
        }
        code = hit[1];
        if (code == 0xff) {
            if (first)
                cur++;
            result = SV_NAR_E_PHONEME;
            remaining = 0;
            code = 8;
        } else {
            if (*hit != 0) {
                cur++;
                remaining--;
            }
            c = *cur++;
            remaining--;
        }

        /* 0x1c004d3b */
        emit = 1;
        uint32_t cls = sv_class(lang, (uint16_t)code);
        if (cls & 0x40000200u) {
            sv_record *prev = r - 1;
            if (sv_rec_class(prev) & 0x10000000u) {
                uint16_t s;
                if (code == 0x68)
                    s = 5;
                else if (code == 0x69)
                    s = 2;
                else
                    s = (uint8_t)(code - 0x5e);
                prev->stress = s;
                if (s)
                    prev->flags |= 0x30;
                emit = 0;
            } else {
                emit = 0;
                cur--;
                result = SV_NAR_E_PHONEME;
                c = 0;
            }
        }
        if (code == 0x6a) {
            if (!(e->flags & 0x80))
                emphasis++;
            emit = 0;
        }
        if (code == 0x6b) {
            /* 0x1c004dcc. The original's error test here compares a masked
             * dword against 0xffff8000 and can never fire, so a malformed
             * number is taken as its partial value. */
            number = (uint16_t)parse_number(cur - 1);
            for (;;) {
                c = *cur++;
                if (!c)
                    break;
                remaining--;
                if (c < '0' || c > '9')
                    break;
            }
            number_pending = 1;
            emit = 0;
        }
        /* 0x1c004e47 */
        if (sv_class(lang, (uint16_t)code) & 0x04000000u) {
            if (e->record_count == 4) {
                emit = 0;
            } else if (sv_rec_class(r - 1) & 0x04000000u) {
                if (code == 0) {
                    emit = 0;
                } else {
                    e->record_count--;
                    r--;
                    emit = 1;
                }
            }
        }
        /* 0x1c004e82 */
        if (code == 5) {
            r->flags |= 0x800;
            emit = 0;
        } else if (code == 6) {
            sv_record *prev = r - 1;
            if (sv_rec_class(prev) & 0x04000000u) {
                if (prev->code == 0)
                    r[-2].flags |= 0x1000;
            } else {
                prev->flags |= 0x1000;
            }
            emit = 0;
        }
        /* 0x1c004ec2: English only, R-coloured vowels merge with a
         * following code 0x36. */
        if (lang->id == 1 && code == 0x36 && pending == NULL && emphasis == 0) {
            unsigned orig = code;
            switch ((int16_t)r[-1].code) {
            case 9:  code = 0x2b; break;
            case 13: code = 0x2d; break;
            case 17: code = 0x2f; break;
            case 39: code = 0x31; break;
            case 41: code = 0x33; break;
            default: break;
            }
            if (orig != code) {
                e->record_count--;
                r--;
                if (r->commands) {
                    cmd -= 3;
                    pending = r->commands;
                }
            }
        } else if (code == 0x5c) {
            code = 0x27;
        }

        if (emit) {
            if (r >= e->records + e->record_capacity)
                return SV_NAR_E_NOMEM; /* ours: the original overruns */
            e->record_count++;
            set_code(e, r, (uint16_t)code);
            if (code != 0) {
                if ((size_t)(cmd - e->commands) + 9 > 3 * e->command_capacity)
                    return SV_NAR_E_NOMEM; /* ours */
                if (emphasis) {
                    if (!pending)
                        pending = cmd;
                    put_cmd(&cmd, 0x2bc, (int16_t)emphasis);
                    r->flags |= 0x400;
                    emphasis = 0;
                }
                if (number_pending) {
                    if (!pending)
                        pending = cmd;
                    put_cmd(&cmd, 0x334, (int16_t)number);
                    number_pending = 0;
                }
                if (pending)
                    put_cmd(&cmd, SV_CMD_END, 0);
                r->commands = pending;
                pending = NULL;
            }
            r++;
        }
    }

    /* 0x1c00504a */
    e->text_end = cur - 1;
    if (result == 0 && e->record_count > 4) {
        sv_record *last = r - 1;
        if (last->code == 0) {
            last->code = 4;
        } else if (!(sv_rec_class(last) & 0x02000000u) || code == 0x1b) {
            r->code = 4;
            r++;
            r[-1].attrs = ph_attrs(e->phonemes, 4);
            e->record_count++;
        }
        if (pending) {
            cmd[0] = SV_CMD_END;
            cmd[1] = 0;
            cmd[2] = 0;
            r[-1].commands = pending;
        }
        r[0].code = 0x1b;
        r[0].attrs = ph_attrs(e->phonemes, 0x1b);
        r[1].code = 0x1b;
        r[1].attrs = ph_attrs(e->phonemes, 0x1b);
        r[2].code = SV_RECORD_END;
        r[2].stress = 0xff;
        r[2].duration = 0xff;
        r[2].attrs = 0;
        e->record_count += 3;
    }
    if (e->record_count == 4) {
        result = SV_NAR_DONE;
        e->record_count = 0;
    }
    return result;
}

/* ------------------------------------------------------------------------ */
/* 0x1c003e40 — language and phoneme-table switches                          */
/* ------------------------------------------------------------------------ */
static void apply_switches(sv_engine *e)
{
    const uint8_t *ph = e->phonemes;
    const sv_langmod *lang = e->lang;
    sv_record *r = e->records;
    for (; r->code != SV_RECORD_END; r++) {
        int16_t *c = r->commands;
        if (c && c[0] != SV_CMD_END) {
            do {
                if (c[0] == 0x41 && c[1] >= 0 && c[1] <= 2) {
                    lang = e->modules[c[1]];
                    e->lang_primary = lang;
                    e->lang = lang;
                }
                if (c[0] == 0x32) {
                    int v = c[1] - 1;
                    if (v == 0 || v == 2 || v == 4 || v == 12 || v == 14)
                        ph = lang->phonemes_b;
                    else
                        ph = lang->phonemes_a;
                }
                c += 3;
            } while (c[0] != SV_CMD_END);
        }
        r->lang = lang;
        r->phonemes = ph;
    }
    r->lang = lang;
    r->phonemes = ph;
}

/* ------------------------------------------------------------------------ */
/* 0x1c00c550 — mark the onset consonants of stressed syllables              */
/* ------------------------------------------------------------------------ */
static void mark_onsets(sv_engine *e)
{
    sv_record *d = e->records + 5;
    for (; d->code != SV_RECORD_END; d++) {
        sv_record *s = d - 1, *c;
        uint32_t k;
        if (d->stress == 0)
            continue;
        if (!(sv_rec_class(s) & 2))
            continue;
        s->flags |= 0x10;
        c = s;
        k = sv_rec_class(s);
        if (k & 0x8000u) {
            c = s - 1;
        } else {
            if (k & 0x30000u)
                c = s - 1;
            if (sv_rec_class(c) & 0x1800000u) {
                c->flags |= 0x10;
                c--;
            }
        }
        k = sv_rec_class(c);
        if ((k & 0x4000u) || ((k & 0x1000u) && !(k & 0x8000000u)))
            c->flags |= 0x10;
    }
}

/* ------------------------------------------------------------------------ */
/* 0x1c005910 — phonological rewrite rules                                   */
/*                                                                           */
/* Rules are 16 bytes, from the record's language table +0x24:               */
/*   +0 insert-before code, +1 replacement code, +2 insert-after code (0xff   */
/*   = none; all three 0xff deletes), +3/+4/+5 previous/current/next code to  */
/*   match (0xff = any), +6..+10 five context tests, +11 flags. A test's     */
/*   0x60 bits pick "flag 0x10 clear/set" or "class bit c&0x1f clear/set" on  */
/*   the record under test, and its 0x80 bit moves the test to the next       */
/*   record; the first test looks at the previous record.                     */
/* ------------------------------------------------------------------------ */

static uint32_t class_mask(unsigned k)
{
    /* 0x1c0122a8: 1 << k for k < 31. Entry 31 is the first four bytes of
     * the string that follows ("germ"), which the original reads. */
    return k < 31 ? (1u << k) : 0x6d726567u;
}

static int rule_match(const uint8_t *rule, const sv_record *r) /* 0x1c005a00 */
{
    if (rule[4] != 0xff && (int16_t)r->code != rule[4])
        return 0;
    if (rule[3] != 0xff && (int16_t)r[-1].code != rule[3])
        return 0;
    if (rule[5] != 0xff && (int16_t)r[1].code != rule[5])
        return 0;
    return 1;
}

static int rule_context(const uint8_t *rule, const sv_record *r,
                        uint32_t st_flag) /* 0x1c005a60 */
{
    const sv_record *next = r + 1, *at = r;
    uint32_t cls = sv_rec_class(r - 1), fl = r[-1].flags;
    int ok = 1;
    for (int i = 0; i < 5 && ok; i++) {
        unsigned c = rule[6 + i];
        if (c != 0xff) {
            /* 0x1c005acd..0x1c005afd: 0x00/0x40 forbid, 0x20/0x60 require. */
            switch (c & 0x60) {
            case 0x00: if (fl & 0x10) ok = 0; break;
            case 0x20: if (!(fl & 0x10)) ok = 0; break;
            case 0x40: if (class_mask(c & 0x1f) & cls) ok = 0; break;
            default:   if (!(class_mask(c & 0x1f) & cls)) ok = 0; break;
            }
        }
        if (c & 0x80) {
            uint16_t code = at->code;
            cls = sv_rec_class(at);
            fl = at->flags;
            if (next == at && (rule[0xb] & 0x80) && code == 0) {
                cls = sv_rec_class(at + 1);
                fl = at[1].flags;
                at++;
            }
            at++;
        }
    }
    if (rule[0xb] & 0x10) {
        if ((rule[0xb] & 0x1f) != 0x11)
            return 0;
        if (st_flag == 0)
            return 0;
    }
    return ok;
}

static int rule_insert_before(const uint8_t *rule, sv_record *r, int n) /* 0x1c005c00 */
{
    uint16_t code = rule[0];
    if (code == 0xff)
        return 0;
    memmove(r + 1, r, (size_t)n * sizeof *r);
    r->flags = 0;
    r->stress = 0;
    r->code = code;
    r->attrs = ph_attrs(r->phonemes, code);
    r->commands = NULL;
    return 1;
}

static int rule_replace(const uint8_t *rule, sv_record *r) /* 0x1c005cb0 */
{
    uint16_t code = rule[1];
    if (code == 0xff)
        return 0;
    r->code = code;
    r->attrs = ph_attrs(r->phonemes, code);
    return 1;
}

static int rule_insert_after(const uint8_t *rule, sv_record *r, int n) /* 0x1c005d40 */
{
    uint16_t code = rule[2];
    if (code == 0xff)
        return 0;
    if (n > 1)
        memmove(r + 2, r + 1, (size_t)(n - 1) * sizeof *r);
    r[1].code = code;
    r[1].stress = 0;
    r[1].flags = 0;
    r[1].attrs = ph_attrs(r[1].phonemes, code);
    r[1].commands = NULL;
    return 1;
}

static int rule_delete(const uint8_t *rule, sv_record *r, int n) /* 0x1c005cf0 */
{
    if (rule[0] != 0xff || rule[1] != 0xff || rule[2] != 0xff)
        return 0;
    if (n > 1)
        memmove(r, r + 1, (size_t)(n - 1) * sizeof *r);
    return 1;
}

static int apply_rules(sv_engine *e)
{
    sv_record *r = e->records + 5;
    int n = (int)e->record_count - 5;
    uint32_t st_flag = e->flags & 0x10;
    while (r->code != SV_RECORD_END) {
        const uint8_t *rule = r->lang->rules;
        int stop;
        do {
            stop = rule[0xb] & 0x20;
            if (rule_match(rule, r) && !(rule[0xb] & 0x12) &&
                rule_context(rule, r, st_flag)) {
                if (e->record_count + 1 >= e->record_capacity)
                    return SV_NAR_E_NOMEM; /* ours: the original overruns */
                if (rule_insert_before(rule, r, n)) {
                    n++;
                    e->record_count++;
                }
                rule_replace(rule, r);
                if (rule_insert_after(rule, r, n)) {
                    r++;
                    e->record_count++;
                }
                if (rule_delete(rule, r, n)) {
                    r--;
                    e->record_count--;
                }
                if (!(rule[0xb] & 0x40))
                    stop = 1;
            }
            rule += 16;
        } while (!stop);
        r++;
        n--;
    }
    return SV_NAR_OK;
}

/* ------------------------------------------------------------------------ */
/* 0x1c00c6d0 — phrase and word grouping                                     */
/*                                                                           */
/* Three cursors walk the records at different speeds: `q` one ahead of the   */
/* record examined, `esi` over records already assigned a group, `edi` over   */
/* those whose stress position is being settled. Transliterated as such.     */
/* ------------------------------------------------------------------------ */
static void group_words(sv_engine *e)
{
    sv_record *esi = e->records + 5, *edi = esi, *q = esi + 1;
    uint16_t pc = esi->code;
    uint32_t ecx = 0;
    int edx = 0, ebx = -1, seen_vowel = 0;
    if (pc == SV_RECORD_END)
        return;
    do {
        edx++;
        sv_record *p = q - 1;
        uint32_t k = sv_rec_class(p) & 0x14000000u;
        if (k == 0x04000000u) {
            /* 0x1c00c743: a boundary. */
            uint16_t t = q[-edx].code;
            if (t == 0x1e || t == 0x1d) {
                uint32_t f = esi[-1].flags;
                edx--;
                ebx--;
                edi++;
                esi->flags |= f & 0x60;
                esi++;
            }
            edx--;
            if (ebx > 0)
                edi += ebx;
            int ebp = edx - ebx;
            edi->flags |= 8;
            if (pc == 0 && !(edi[ebp - 1].flags & 0x1000)) {
                edi += ebp;
            } else {
                int cnt = ebp;
                while (cnt-- > 0) {
                    edi++;
                    edi[-1].flags |= 4;
                }
            }
            esi->flags |= 0x80;
            while (edx-- != 0) {
                esi->flags |= ecx;
                esi++;
            }
            ecx = 0;
            edx = 0;
            esi++;
            edi++;
            seen_vowel = 0;
        } else if (k == 0x10000000u) {
            /* 0x1c00c7eb: a vowel. */
            if (pc != 0x1e && pc != 0x1d) {
                if (seen_vowel) {
                    ecx |= 0x40;
                    uint16_t t = q[-edx].code;
                    if (t == 0x1e || t == 0x1d) {
                        uint32_t f = esi[-1].flags;
                        edx--;
                        ebx--;
                        edi++;
                        esi->flags |= f & 0x60;
                        esi++;
                    }
                    esi->flags |= 0x80;
                    ebx = (ebx + edx) >> 1;
                    edx -= ebx;
                    int cnt = ebx;
                    edi += ebx;
                    while (cnt-- != 0) {
                        esi->flags |= ecx;
                        esi++;
                    }
                    ecx = 0x40;
                } else {
                    seen_vowel = 1;
                }
                ebx = edx - 1;
                ecx |= esi[ebx].flags & 0x20;
            }
        }
        pc = q->code;
        q++;
    } while (pc != SV_RECORD_END);
}

/* ------------------------------------------------------------------------ */
/* 0x1c003de0 — records that follow a pause                                  */
/* ------------------------------------------------------------------------ */
static void mark_after_pause(sv_engine *e)
{
    uint32_t prev = sv_rec_class(&e->records[4]);
    for (sv_record *r = e->records + 5; r->code != SV_RECORD_END; r++) {
        uint32_t cls = sv_rec_class(r);
        if (prev & 0x4000000u)
            r->flags |= 0x100;
        if (cls & 1)
            break;
        prev = cls;
    }
}

/* ------------------------------------------------------------------------ */
/* 0x1c00cc10 — the eight intonation arrays                                  */
/* ------------------------------------------------------------------------ */
static int alloc_intonation(sv_engine *e)
{
    unsigned n = 0;
    for (sv_record *r = e->records + 5; r->code != SV_RECORD_END; r++)
        if (r->flags & 0x80)
            n++;
    n = (uint16_t)(n + 3);
    free(e->into_base);
    /* One allocation of 8n bytes in the original; the last array runs two
     * bytes past it. Padded here so those reads stay inside the block. */
    uint8_t *b = calloc(8 * (size_t)n + 16, 1);
    e->into_base = b;
    if (!b)
        return -2;
    e->groups = (uint16_t)(n - 3);
    e->into_c4 = b + 2;
    e->into_c8 = b + n + 2;
    e->into_cc = b + 2 * n + 2;
    e->into_d0 = b + 3 * n + 2;
    e->into_d4 = b + 4 * n + 2;
    e->into_d8 = b + 5 * n + 2;
    e->into_e0 = b + 6 * n + 2;
    e->into_dc = b + 7 * n + 2;
    e->into_c4[n - 3] = 0xff;
    e->into_c8[n - 3] = 0xff;
    e->into_cc[n - 3] = 0xff;
    e->into_d0[n - 3] = 0xff;
    e->into_d4[n - 3] = 0xff;
    e->into_d8[n - 3] = 0xff;
    e->into_e0[n - 3] = 0xff;
    e->into_dc[n - 3] = 0xff;
    e->phrase_length = 0;
    e->phrase_index = 0;
    e->phrase_accent = 0;
    e->phrase_record = &e->records[4];
    return 0;
}

/* ------------------------------------------------------------------------ */
/* 0x1c00c8c0 — syllable classification, five passes                         */
/* ------------------------------------------------------------------------ */
static void classify_c900(sv_engine *e)
{
    sv_record *r = e->records;
    uint8_t *d = e->into_dc;
    while (r->code != SV_RECORD_END) {
        uint16_t ax = r->stress;
        uint32_t fl = r->flags;
        uint32_t cls = sv_rec_class(r);
        r++;
        if (cls & 0x04000000u) {
            uint8_t al = (uint8_t)(d[-1] | 0x80);
            d[-1] = al;
            if (sv_rec_class(r - 1) & 0x80000u)
                d[-1] = (uint8_t)(al | 0x40);
        } else if (fl & 0x80) {
            if (fl & 0x20) {
                uint8_t s = (uint8_t)(ax & 0xf);
                while (s == 0) {
                    ax = r->stress;
                    fl = r->flags;
                    r++;
                    s = (uint8_t)(ax & 0xf);
                }
                uint8_t al = (uint8_t)((s * 13) / 9);
                if (fl & 0x40)
                    al = (uint8_t)(al + 2);
                if (al > 4)
                    *d |= 0x20;
                *d |= al;
            }
            d++;
        }
    }
}

static void classify_c9b0(sv_engine *e)
{
    uint8_t *d8 = e->into_d8 - 1, *dc = e->into_dc - 1;
    int eax = 0;
    sv_record *r = e->records;
    uint16_t cx = r->code;
    while (cx != SV_RECORD_END) {
        uint32_t fl = r->flags;
        *dc |= eax ? 0x10 : 0;
        if (fl & 0x80) {
            d8++;
            dc++;
        }
        if (cx == 4)
            *d8 |= 0x90;
        if (fl & 0x800) {
            eax = 1;
            *d8 |= 2;
        } else if (fl & 0x1000) {
            eax = 0;
            *d8 |= 0xe;
        }
        cx = r[1].code;
        r++;
    }

    int flag = 0;
    uint8_t *mark = NULL;
    uint8_t dl = 0;
    uint8_t *s = e->into_d8, *t = e->into_dc;
    unsigned cnt = e->groups;
    while (cnt--) {
        uint8_t cl = (uint8_t)(*s & 0xf);
        s++;
        uint8_t b = (uint8_t)(*t & 0x20);
        t++;
        if (cl == 2) {
            s[-1] &= 0xf0;
            flag = 1;
            dl = 2;
        }
        if (b) {
            mark = s - 1;
            if (flag) {
                *mark |= dl;
                dl = 0;
            }
        }
        if (cl == 0xe) {
            s[-1] &= 0xf0;
            dl = 0;
            flag = 0;
            if (mark)
                *mark |= 0xe;
            mark = NULL;
        }
    }
}

static void classify_cac0(sv_engine *e)
{
    for (unsigned k = 0; k < e->groups; k++)
        e->into_e0[k] |= 1;
}

static uint16_t final_code(const sv_engine *e)
{
    /* `[count*26 + records - 0x5c]`: the code of records[count - 4]. */
    return e->records[(int)e->record_count - 4].code;
}

static void classify_caf0(sv_engine *e)
{
    /* ours: a sentence with no stressed group at all (e->groups == 0 --
     * reachable once inline commands can produce a sentence with no real
     * phonetic content, e.g. "{voice male}" alone) makes the original
     * compute this and classify_cb60's `groups - 1` as a 32-bit index of
     * -1. On the original's 32-bit target that wraps back to "one byte
     * before the array", landing harmlessly inside the same padded
     * allocation; the identical C expression on a 64-bit host adds a
     * ~4-billion-byte offset instead, which is not the same computation
     * and is not safely reproducible, so it is refused here rather than
     * chased into whatever it would hit. Nothing downstream reads the
     * intonation arrays when there is no stressed group to describe. */
    if (e->groups == 0)
        return;
    uint16_t ax = final_code(e);
    uint8_t cl = 0;
    if (ax == 1)
        cl = 4;
    else if (ax == 2)
        cl = 8;
    unsigned g = e->groups;
    uint8_t *dc = e->into_dc + g - 1;
    uint8_t *pe0 = e->into_e0 + g - 1;
    *pe0 |= cl;
    g--;
    while (g) {
        dc--;
        if (*dc & 0x80)
            break;
        pe0[-1] |= cl;
        pe0--;
        g--;
    }
}

static void classify_cb60(sv_engine *e)
{
    /* ours: see classify_caf0 -- e->groups == 0 makes `e->into_e0[g - 1]`
     * below a 64-bit out-of-bounds index rather than the original's
     * harmless 32-bit wraparound. */
    if (e->groups == 0)
        return;
    unsigned g = e->groups;
    uint8_t *pdc = e->into_dc, *pd8 = e->into_d8, *mark = NULL;
    uint8_t dl = (uint8_t)(e->into_e0[g - 1] & 0xc);
    uint16_t di = final_code(e);
    if (di == 4)
        pdc[g - 1] |= 0x40;
    while (g--) {
        uint8_t bl = *pdc;
        if (bl & 0x20)
            mark = pd8;
        if (bl & 0x40) {
            *pd8 |= 0xb0;
            if (mark) {
                *mark = (uint8_t)((*mark & 0xf4) | 4);
                mark = NULL;
            }
        }
        pdc++;
        pd8++;
    }
    if (dl)
        pd8[-1] &= 0xf;
    if (dl == 4)
        pd8[-1] |= 0x80;
    if (di == 1)
        pd8[-1] = 0;
}

/* ------------------------------------------------------------------------ */
/* 0x1c00cdc0 — intonation targets, phrase by phrase                         */
/* ------------------------------------------------------------------------ */

static void into_shift(sv_engine *e, ptrdiff_t d)
{
    e->into_c4 += d; e->into_c8 += d; e->into_cc += d; e->into_d0 += d;
    e->into_d4 += d; e->into_d8 += d; e->into_dc += d; e->into_e0 += d;
}

static int phrase_begin(sv_engine *e) /* 0x1c00ceb0 */
{
    into_shift(e, e->phrase_length);
    e->phrase_length = 0;
    e->phrase_stressed = 0;
    e->phrase_first = 0;
    e->phrase_index++;
    e->phrase_last = 0;
    e->phrase_accent = 0;
    uint8_t *p = e->into_dc;
    if (*p == 0xff)
        return 0;
    int first = 1;
    for (;;) {
        uint8_t cl = *p++;
        if (cl & 0x80)
            e->phrase_accent++;
        if (cl & 0x20) {
            e->phrase_stressed++;
            if (first) {
                first = 0;
                e->phrase_first = e->phrase_length;
            }
            e->phrase_last = e->phrase_length;
        }
        e->phrase_length++;
        if (cl & 0x40)
            break;
    }
    e->phrase_accent = (uint16_t)((e->phrase_accent + 4) / 3);
    return 1;
}

static void phrase_top(sv_engine *e) /* 0x1c00cfb0 */
{
    int v = (6 - (int)e->phrase_index) * (int)e->phrase_accent -
            (int)e->phrase_index * 4 + 0x3d;
    if (v <= 0x3e)
        v = 0x3e;
    if (v >= 0x48)
        v = 0x48;
    e->into_c8[e->phrase_first] = (uint8_t)v;
}

static void phrase_declination(sv_engine *e) /* 0x1c00d010 */
{
    int lo = ((e->into_e0[e->phrase_length - 1] & 0xc) == 8) ? 0x3f : 0x37;
    int n = e->phrase_stressed;
    if (n > 1) {
        uint8_t *s = e->into_c8 + e->phrase_first;
        int hi = *s;
        int step = ((hi - lo) * 4) / n;
        hi *= 4;
        s++;
        n--;
        uint8_t *m = e->into_dc + e->phrase_first + 1;
        if (n < 1 || n > 2) {
            int a = (step * 115 + 50) / 100;
            int b = (-(step * 30) - 50) / 100;
            b = b / (n - 2);
            int st2 = step + b;
            while (!(*m++ & 0x20))
                s++;
            hi -= a;
            s++;
            s[-1] = (uint8_t)(hi >> 2);
            n -= 3;
            while (n != 0) {
                if (*m++ & 0x20) {
                    hi -= st2;
                    n--;
                    *s = (uint8_t)(hi >> 2);
                }
                s++;
            }
            while (!(*m++ & 0x20))
                s++;
            hi -= a;
            *s = (uint8_t)(hi >> 2);
            e->into_c8[e->phrase_last] = (uint8_t)((hi - st2) >> 2);
        } else {
            while (n != 0) {
                if (*m++ & 0x20) {
                    hi -= step;
                    n--;
                    *s = (uint8_t)(hi >> 2);
                }
                s++;
            }
        }
    }
    int n2 = e->phrase_stressed;
    uint8_t *s = e->into_c8 + e->phrase_first;
    uint8_t *m = e->into_dc + e->phrase_first;
    while (n2) {
        uint8_t bl = *m;
        if (bl & 0x20) {
            int c = *s;
            int v = ((c - lo) * 5 + 0x19) * 2 / 100;
            v = v * ((bl & 0xf) - 8) + c;
            if (v <= 0x19)
                v = 0x19;
            if (v >= 0xc8)
                v = 0xc8;
            n2--;
            *s = (uint8_t)v;
        }
        s++;
        m++;
    }
}

static void phrase_accents(sv_engine *e) /* 0x1c00d200 */
{
    unsigned ec = e->phrase_first;
    uint8_t *c8 = e->into_c8 + ec, *d0 = e->into_d0 + ec, *d4 = e->into_d4 + ec;
    uint8_t *dc = e->into_dc + ec, *d8 = e->into_d8 + ec;
    int n = e->phrase_stressed;
    while (n) {
        if (*dc & 0x20) {
            int x = (int)*c8 - 0x37;
            int k = *d8 & 0xf;
            if (k >= 7)
                k -= 0x10;
            int a = (k * 20 + 0x32) / 100;
            a = (a + 1) * x;
            a = (a * 40 + 0x32) / 100;
            *d0 = (uint8_t)a;
            int b = ((1 - k) * x * 20 - 0x32) / 100;
            if (b <= 0)
                b = 0;
            *d4 = (uint8_t)b;
            if ((*dc & 0x10) && k == 0) {
                *d0 = (uint8_t)(*d0 + (-(*d0 * 30) - 50) / 100);
                *d4 = (uint8_t)(*d4 + (-(*d4 * 30) - 50) / 100);
            }
            n--;
        }
        dc++;
        d0++;
        d4++;
        c8++;
        d8++;
    }
    e->into_d0[ec] = (uint8_t)(e->into_c8[ec] - 0x37);
}

static void phrase_shape(sv_engine *e) /* 0x1c00d370 */
{
    /* 0x1c00d373..0x1c00d396 compares the local date against 9999-12 and
     * cannot fire; omitted. */
    int n = e->phrase_stressed;
    unsigned ec = e->phrase_first;
    if (n > 1) {
        uint8_t *pc8 = e->into_c8 + ec;
        uint8_t *pd0 = e->into_d0 + ec;
        uint8_t *pd4 = e->into_d4 + ec;
        uint8_t *q = pc8 + 1;
        uint8_t *m = e->into_dc + ec + 1;
        uint8_t *r = pd0 + 1;
        uint8_t *pd4n = pd4 + 1;
        int k = 0;
        n--;
        while (n != 0) {
            uint8_t mm = *m++;
            if (mm & 0x20) {
                int c1, bx, v24;
                switch (k) {
                case 0: c1 = -40; bx = -20; v24 = 20; break;
                case 1: c1 = 0; bx = 0; v24 = 0; break;
                case 2: c1 = 15; bx = 10; v24 = -15; break;
                case 3: c1 = 25; v24 = -25; bx = 15; break;
                default: c1 = 30; v24 = -25; bx = 15; break;
                }
                uint8_t a = *pd0;
                *pd0 = (uint8_t)(a + (a * c1 + 50) / 100);
                *r = (uint8_t)(*r + (*r * c1 + 50) / 100);
                uint8_t b = *pc8;
                *pc8 = (uint8_t)(b + (((int)b - 0x37) * bx + 50) / 100);
                uint8_t nq = (uint8_t)(*q + (((int)*q - 0x37) * v24 + 50) / 100);
                *q = nq;
                if (k >= 3 && !(m[-1] & 0x10))
                    *r = (uint8_t)(nq - 0x34);
                if (k == 0) {
                    uint8_t d4v = *pd4;
                    int t = (int)*r + ((int)*pc8 - (int)*q - (int)d4v);
                    if (t > 0)
                        *pd4 = (uint8_t)(t + d4v);
                    else
                        *r = (uint8_t)(*r - t);
                    if (m[-3] & 0x20) {
                        uint8_t x = *pd0;
                        *pd0 = *pd4;
                        *pd4 = x;
                    }
                }
                k = 0;
                pc8 = q;
                pd0 = r;
                pd4 = pd4n;
                n--;
            } else {
                k++;
            }
            q++;
            r++;
            pd4n++;
        }
    }

    int n3 = e->phrase_stressed;
    if (n3 == 0)
        return;
    uint8_t *m = e->into_dc + ec, *s = e->into_c8 + ec;
    int mx = 0;
    while (n3) {
        if (*m++ & 0x20) {
            if (*s > mx)
                mx = *s;
            n3--;
        }
        s++;
    }
    unsigned ee = e->phrase_last;
    uint8_t *pe0 = e->into_e0 + ee, *mm = e->into_dc + ee, *s8 = e->into_c8 + ee;
    uint8_t *p4 = e->into_d4 + ee, *p0 = e->into_d0 + ee;
    uint8_t bl = *pe0 & 0xc;
    while (bl) {
        if (*mm & 0x20) {
            if (bl == 8) {
                int a = (*p4 * 80 + 50) / 100;
                *p0 = (uint8_t)(*p0 + a);
                *p4 = (uint8_t)(*p4 - a);
            }
            if (*mm & 0x80) {
                int dl = (e->st_52 == 1 || e->st_52 == 2) ? 0x2a : 0x25;
                if (bl == 4)
                    *p4 = (uint8_t)(*s8 - dl);
                else
                    *p4 = (uint8_t)(*s8 + (-(mx * 120) - 50) / 100);
            }
        }
        p4--;
        pe0--;
        p0--;
        mm--;
        s8--;
        bl = *pe0 & 0xc;
    }
}

static void phrase_d6e0(sv_engine *e) /* 0x1c00d6e0 */
{
    uint8_t *p8 = e->into_d8, *m = e->into_dc, *p4 = e->into_d4;
    unsigned n = e->phrase_length;
    int mark = 0;
    while (n--) {
        if (*m & 0x20)
            mark = 1;
        uint8_t al = *p8 & 0xf0;
        if (al && mark) {
            int a;
            if (al & 0x80)
                a = (*p4 * 30 + 0x32) / 100;
            else
                a = (-(*p4 * 80) - 50) / 100;
            *p4 = (uint8_t)(*p4 + a);
        }
        if (*m & 0x80)
            mark = 0;
        m++;
        p4++;
        p8++;
    }
}

static void phrase_contour(sv_engine *e) /* 0x1c00d780 */
{
    {
        uint8_t *m = e->into_dc, *pcc = e->into_cc, *pc4 = e->into_c4, *pc8 = e->into_c8;
        unsigned cnt = e->phrase_stressed == 0 ? e->phrase_length : e->phrase_first;
        while (cnt--) {
            *pc4++ = 0x37;
            *pc8++ = (uint8_t)((*m++ & 0xf) * 2 + 0x37);
            *pcc++ = 0x37;
        }
    }

    unsigned ec = e->phrase_first;
    uint8_t *M = e->into_dc + ec, *C4 = e->into_c4 + ec, *C8 = e->into_c8 + ec;
    uint8_t *CC = e->into_cc + ec, *D0 = e->into_d0 + ec, *D4 = e->into_d4 + ec;
    int left = (int)e->phrase_stressed - 1;
    *C4 = (uint8_t)(*C8 - *D0);
    *CC = (uint8_t)(*C8 - *D4);
    if (left <= 0)
        left = 0;
    uint8_t *M1 = M + 1, *C4n = C4 + 1, *C8n = C8 + 1, *CCn = CC + 1;
    uint8_t *D0n = D0 + 1, *D4n = D4 + 1;
    int gap = 0;
    while (left != 0) {
        if (*M1 & 0x20) {
            *C4n = (uint8_t)(*C8n - *D0n);
            *CCn = (uint8_t)(*C8n - *D4n);
            if (gap != 0) {
                int d = (int)*CC - (int)*C4n;
                if (d < 0) {
                    int h = d / 2;
                    d = 0;
                    *CC = (uint8_t)(*CC - h);
                    *C4n = (uint8_t)(*C4n + h);
                }
                if (*M1 & 0x10) {
                    int dd = d / gap;
                    int half = dd / 2;
                    int g = gap;
                    while (g-- != 0) {
                        uint8_t v = *CC;
                        C4++;
                        C8++;
                        CC++;
                        *C4 = v;
                        *C8 = (uint8_t)(v - half);
                        *CC = (uint8_t)(*C4 - dd);
                    }
                } else if (gap == 1) {
                    C4++;
                    uint8_t v = *CC;
                    *C4 = v;
                    C8[1] = (uint8_t)(v - d / 2);
                    CC[1] = (uint8_t)(*C4 - d);
                } else if (gap == 2) {
                    C4++;
                    uint8_t P = *CC;
                    C8++;
                    *C4 = P;
                    int a = (-(d * 30) - 50) / 100;
                    CC++;
                    *C8 = (uint8_t)(P + a);
                    int b = (-(d * 60) - 50) / 100;
                    C4++;
                    uint8_t Q = (uint8_t)(b + C4[-1]);
                    *CC = Q;
                    *C4 = Q;
                    int c = (-(d * 20) - 50) / 100;
                    C8[1] = (uint8_t)(Q + c);
                    int f = (-(d * 40) - 50) / 100;
                    CC[1] = (uint8_t)(*C4 + f);
                } else {
                    uint8_t P = *CC;
                    C4++;
                    int x = (int)P - 0x34;
                    C8++;
                    *C4 = P;
                    CC++;
                    *C8 = (uint8_t)(P + (-(x * 22) - 50) / 100);
                    C4++;
                    uint8_t Q = (uint8_t)((-(x * 45) - 50) / 100 + C4[-1]);
                    C8++;
                    *CC = Q;
                    *C4 = Q;
                    CC++;
                    *C8 = (uint8_t)(Q + (-(x * 18) - 50) / 100);
                    uint8_t R = (uint8_t)((-(x * 35) - 50) / 100 + *C4);
                    *CC = R;
                    C4[1] = R;
                    C4++;
                    C8++;
                    CC++;
                    *C8 = (uint8_t)(R + ((-5 - x) * 10) / 100);
                    int rest = gap - 3;
                    uint8_t *Mp = M + 3;
                    *CC = (uint8_t)(*C4 + (-(x * 20) - 50) / 100);
                    while (rest-- != 0) {
                        uint8_t S = *CC;
                        C4++;
                        C8++;
                        *C4 = S;
                        CC[1] = S;
                        uint8_t mv = (uint8_t)(Mp[1] & 0xf);
                        CC++;
                        Mp++;
                        *C8 = (uint8_t)(S + mv * 2);
                    }
                }
            }
            M = M1;
            C4 = C4n;
            C8 = C8n;
            CC = CCn;
            gap = 0;
            left--;
        } else {
            gap++;
        }
        M1++;
        C4n++;
        C8n++;
        CCn++;
        D0n++;
        D4n++;
    }

    /* 0x1c00db90: the tail after the last stressed group. */
    unsigned len = e->phrase_length;
    if (!(e->into_dc[len - 1] & 0x20)) {
        unsigned si = e->phrase_last;
        int bp = (int)len;
        if (e->phrase_stressed != 0)
            bp = (int)len - (int)si - 1;
        uint8_t *T4 = e->into_c4 + si, *T8 = e->into_c8 + si, *TC = e->into_cc + si;
        uint8_t kind = e->into_e0[len - 1] & 0xc;
        int x = (kind == 4) ? (int)*TC - 0x25 : (int)*TC - 0x37;
        int step = bp ? x / bp : 0; /* the original faults on bp == 0 */
        while (bp-- != 0) {
            uint8_t v = *TC;
            T4++;
            TC++;
            T8++;
            *T4 = v;
            v = (uint8_t)(v - step);
            *TC = v;
            *T8 = (uint8_t)(v + 2);
        }
        if (kind == 8) {
            int mx = 0;
            const uint8_t *p = e->into_c8;
            for (unsigned k = len; k; k--, p++)
                if (*p > mx)
                    mx = *p;
            e->into_cc[len - 1] = (uint8_t)((mx * 120 + 50) / 100);
        }
    }
}

static void phrase_voicing(sv_engine *e) /* 0x1c00dc80 */
{
    sv_record *r = e->phrase_record;
    uint8_t *M = e->into_dc, *C4 = e->into_c4, *C8 = e->into_c8, *CC = e->into_cc;
    unsigned n = e->phrase_length;
    while (n--) {
        while (!(r->flags & 0x80))
            r++;
        uint32_t k = sv_rec_class(r);
        int voiced = (k & 2) && (k & 0x8000000u);
        int unvoiced = (k & 2) && !(k & 0x8000000u);
        uint8_t bl;
        int num;
        if (!(*M & 0x20)) {
            bl = *CC;
            int a = (int)*C8 - (int)bl;
            num = (a * 5 + 5) * 10;
        } else {
            if (unvoiced)
                *C8 = (uint8_t)(*C8 + (*C8 * 20 - 0x41a) / 100);
            int w = voiced ? 0x14 : 0;
            if (unvoiced)
                w = 0x50;
            bl = *C4;
            *C4 = (uint8_t)(bl + (((int)*C8 - (int)bl) * w + 50) / 100);
            int any_unvoiced = 0, all_voiced = 1;
            if (!(M[1] & 0x20)) {
                sv_record *q = r + 1;
                for (;;) {
                    uint32_t kk = sv_rec_class(q);
                    if (!any_unvoiced)
                        any_unvoiced = (kk & 2) && !(kk & 0x8000000u);
                    all_voiced = all_voiced && (kk & 2) && (kk & 0x8000000u);
                    if (q->flags & 0x88)
                        break;
                    q++;
                }
            }
            int w2 = any_unvoiced ? 0x42 : (all_voiced ? 0x14 : 0x32);
            bl = *CC;
            num = ((int)*C8 - (int)bl) * w2 + 50;
        }
        M++;
        C4++;
        *CC = (uint8_t)(bl + num / 100);
        C8++;
        CC++;
    }
}

static void intonation(sv_engine *e)
{
    if (phrase_begin(e)) {
        do {
            if (e->phrase_stressed) {
                phrase_top(e);
                phrase_declination(e);
                phrase_accents(e);
                phrase_shape(e);
            }
            phrase_d6e0(e);
            phrase_contour(e);
            phrase_voicing(e);
            sv_record *r = e->phrase_record;
            while (r->code != SV_RECORD_END && !(sv_rec_class(r) & 0x80000u))
                r++;
            e->phrase_record = r + 1;
        } while (phrase_begin(e));
    }
    into_shift(e, -(ptrdiff_t)e->groups);
}

/* ------------------------------------------------------------------------ */
/* 0x1c00c3e0 — per-record durations, language vtable +0x04                  */
/* ------------------------------------------------------------------------ */
static void durations(sv_engine *e)
{
    for (sv_record *r = e->records + 2; r->code != SV_RECORD_END; r++) {
        e->window[2] = r;
        r->lang->duration(e);
    }
}

/* ------------------------------------------------------------------------ */
/* 0x1c00f7c0 — rate and emphasis commands                                   */
/* ------------------------------------------------------------------------ */
static void rate_commands(sv_engine *e)
{
    sv_record *c = e->records, *s = e->records;
    uint32_t total = 0;
    int k = 0;
    int32_t emph = 0;
    while (c->code != SV_RECORD_END) {
        int16_t *cmd = c->commands;
        if (cmd && cmd[0] != SV_CMD_END) {
            do {
                if (cmd[0] == 0x2c6) {
                    uint16_t v = (uint16_t)cmd[1];
                    if ((int16_t)v <= 0x14)
                        v = 0x14;
                    e->st_a8 = v;
                    e->st_aa = (uint16_t)(60000u / v);
                    break;
                }
                int16_t dx = cmd[2];
                if (dx != 0) {
                    if (dx < 0) {
                        cmd[2] = (int16_t)(((8 - (int)c->duration) * dx / 100) >> 3);
                    } else {
                        int v = (int)(int16_t)c->duration - 8;
                        if (v >= dx)
                            v = dx;
                        cmd[2] = (int16_t)(v >> 3);
                    }
                }
                cmd += 3;
            } while (cmd[0] != SV_CMD_END);
        }
        if (c->flags & 0x04000000u) {
            uint32_t m = (uint32_t)e->st_aa * (uint32_t)emph;
            if (m != 0 && k != 0) {
                int cnt = k;
                do {
                    uint32_t d = ((uint32_t)s->duration * m) / total;
                    if (d >= 0xfa0)
                        d = 0xfa0;
                    if (d <= 0x19)
                        d = 0x19;
                    s->duration = (uint16_t)d;
                    int16_t *p = s->commands;
                    if (p && p[0] != SV_CMD_END) {
                        do {
                            if (p[2]) {
                                uint32_t q = ((uint32_t)(int32_t)p[2] * m) / total;
                                uint32_t lim = (uint32_t)c->duration - 1;
                                if (q >= lim)
                                    q = lim;
                                p[2] = (int16_t)q;
                            }
                            p += 3;
                        } while (p[0] != SV_CMD_END);
                    }
                    s++;
                } while (--cnt);
            }
            s = c;
            total = 0;
            k = 0;
            emph = 0;
            /* The original dereferences c->commands without a NULL test. */
            const int16_t *p = c->commands;
            if (p) {
                for (; p[0] != SV_CMD_END; p += 3)
                    if (p[0] == 0x2bc) {
                        emph = p[1];
                        break;
                    }
            }
        }
        c++;
        k++;
        total += c[-1].duration;
    }
}

/* ------------------------------------------------------------------------ */
/* 0x1c00c420 — drop silent records, size and allocate the frames            */
/* ------------------------------------------------------------------------ */
static int alloc_frames(sv_engine *e)
{
    sv_record *dst = e->records;
    size_t nf = 4;
    for (sv_record *r = e->records; r->code != SV_RECORD_END; r++) {
        if (sv_rec_class(r) & 0x100000u)
            continue;
        if (dst != r)
            *dst = *r;
        dst++;
        nf += ((size_t)r->duration + 4) >> 3;
    }
    dst->code = SV_RECORD_END;
    free(e->frames);
    /* The generator writes pitch targets into the NEXT phoneme's frames
     * (0x1c204610), which near the end of the array lands past the
     * original's allocation. Slack keeps those writes in bounds; it is
     * never read back. */
    e->frames = calloc(nf + 64, sizeof(sv_frame));
    if (!e->frames)
        return 0x1b5b;
    e->frame_count = nf;
    e->frame_cursor = e->frames;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* 0x1c005840 — frame generation, language vtable +0x08                      */
/* ------------------------------------------------------------------------ */
static void generate(sv_engine *e)
{
    sv_record *r = e->records;
    e->frame_cursor = e->frames;
    e->window[0] = r;
    e->window[1] = r;
    e->window[2] = r + 1;
    e->window[3] = r + 2;
    e->window[4] = r + 3;
    sv_record *p = r + 1;
    if (!p->lang->generate)
        return;
    while (p->code != SV_RECORD_END) {
        p->lang->generate(e);
        e->window[0] = e->window[1];
        e->window[1] = e->window[2];
        e->window[2] = e->window[3];
        e->window[3] = e->window[4];
        if (e->window[4]->code != SV_RECORD_END)
            e->window[4]++;
        p++;
    }
    p->lang->generate(e);

    sv_frame *f = e->frame_cursor;
    f->formant_freq[0] = 0xff;
    f->formant_freq[1] = 0xff;
    f->formant_freq[2] = 0xff;
    f->amp_voicing = 0xff;
    f->pitch = 0xffff;
    if (e->st_4c == 3) {
        int k = (-4 - (int)e->window[1]->duration) / 8;
        f[-1].pitch = f[k - 1].pitch;
    }
}

/* ------------------------------------------------------------------------ */
/* 0x1c0039f7..0x1c003a21 — the passes after generation                      */
/* ------------------------------------------------------------------------ */

/* 0x1c00be40, through src/expression.c, which takes the record fields it
 * reads as a translated array (it skips the first record itself, as the
 * original does at 0x1c00bea0). */
static int expression(sv_engine *e)
{
    size_t n = 1;
    while (e->records[n].code != SV_RECORD_END)
        n++;
    sv_expr_record *xr = calloc(n + 1, sizeof *xr);
    if (!xr)
        return SV_NAR_E_NOMEM;
    for (size_t k = 0; k <= n; k++) {
        const sv_record *r = &e->records[k];
        /* Commands are little-endian int16 triples, the layout the pass
         * reads as bytes. */
        xr[k].commands = (const uint8_t *)r->commands;
        xr[k].code = r->code;
        xr[k].duration = r->duration;
    }
    sv_expr_state xs = {
        .pitch_divisor = e->sample_rate,
        .source_lock = e->st_52,
        .source_class = e->st_5a,
        .flutter_depth = e->st_62,
        .vibrato_rate = (int16_t)e->st_68,
        .pitch_scaled = e->st_6a,
        .glide_rate = (int16_t)e->st_6c,
        .glide_rising = (int32_t)e->st_6e,
        .pitch_previous = (int16_t)e->st_72,
        .glide = (int32_t)e->st_74,
        .vibrato_depth = e->st_78,
        .tremolo_depth = e->st_7a,
        .source_depth = e->st_7c,
    };
    int rc = sv_expression_apply(&xs, e->frames, e->frame_count + 64, xr, e->expr_tables);
    free(xr);
    e->st_5a = xs.source_class;
    e->st_62 = xs.flutter_depth;
    e->st_68 = (uint16_t)xs.vibrato_rate;
    e->st_6a = xs.pitch_scaled;
    e->st_6c = (uint16_t)xs.glide_rate;
    e->st_6e = (uint32_t)xs.glide_rising;
    e->st_72 = (uint16_t)xs.pitch_previous;
    e->st_74 = (uint32_t)xs.glide;
    e->st_78 = xs.vibrato_depth;
    e->st_7a = xs.tremolo_depth;
    e->st_7c = xs.source_depth;
    return rc == SV_EXPR_OK ? SV_NAR_OK : SV_NAR_E_NOTIMPL;
}

int sv_narrate_render(sv_engine *e, void (*emit)(void *ctx, const uint8_t *pcm, size_t n),
                      void *ctx)
{
    static uint8_t buf[0x2000];
    sv_frame_state *s = &e->render;
    /* Event reporting (0x1c004543..0x1c004605) is not reconstructed and
     * does not touch the audio path; the renderer is run with it off. */
    s->flags = e->flags | SV_FRAME_FLAG_NO_EVENTS;
    s->sample_rate = e->sample_rate;
    s->scratch_310 = e->st_310;
    s->restart = e->restart;
    s->speaking = e->speaking;
    int rc = sv_frame_render(s, e->frame_tables, e->frames, buf, 0x2000);
    if (rc < 0)
        return rc;
    emit(ctx, buf, 0x2000);
    while (s->speaking) {
        rc = sv_frame_render(s, e->frame_tables, e->frames, buf, 0x1000);
        if (rc < 0)
            return rc;
        emit(ctx, buf, 0x1000);
    }
    e->restart = s->restart;
    e->speaking = s->speaking;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* 0x1c003870, the generation half                                           */
/* ------------------------------------------------------------------------ */

#define STAGE(addr) do { if (stop_after == (addr)) return SV_NAR_OK; } while (0)

int sv_narrate_sentence(sv_engine *e, uint32_t stop_after)
{
    int rc;
    e->st_310 = 0;
    if (!sv_next_sentence(e))
        return SV_NAR_DONE;
    free(e->records);
    e->record_capacity = (uint16_t)(e->chunk_phonemes * 2 + 10);
    /* Twice the original's capacity: the rule engine can insert records and
     * the original bounds nothing. The extra is never observable. */
    e->records = calloc((size_t)e->record_capacity * 2 + 8, sizeof(sv_record));
    if (!e->records)
        return SV_NAR_E_NOMEM;
    e->record_capacity = (uint16_t)(e->record_capacity * 2 + 8);

    rc = parse(e);
    if (rc)
        return rc;
    STAGE(0x1c004a10);
    apply_switches(e);
    STAGE(0x1c003e40);
    mark_onsets(e);
    STAGE(0x1c00c550);
    rc = apply_rules(e);
    if (rc)
        return rc;
    STAGE(0x1c005910);
    group_words(e);
    STAGE(0x1c00c6d0);
    mark_after_pause(e);
    STAGE(0x1c003de0);
    if (alloc_intonation(e))
        return SV_NAR_E_NOMEM;
    STAGE(0x1c00cc10);
    classify_c900(e);
    classify_c9b0(e);
    classify_cac0(e);
    classify_caf0(e);
    classify_cb60(e);
    STAGE(0x1c00c8c0);
    intonation(e);
    STAGE(0x1c00cdc0);
    durations(e);
    STAGE(0x1c00c3e0);
    rate_commands(e);
    STAGE(0x1c00f7c0);
    rc = alloc_frames(e);
    if (rc)
        return rc;
    STAGE(0x1c00c420);
    generate(e);
    STAGE(0x1c005840);
    sv_smooth_pitch(e->frames);
    sv_smooth_pitch_slew(e->frames, e->slew_rise, e->slew_fall);
    sv_smooth_pitch_slew(e->frames, e->slew_rise, e->slew_fall);
    STAGE(0x1c00de60);
    rc = expression(e);
    if (rc)
        return rc;
    STAGE(0x1c00be40);
    /* 0x1c003a1c: FUN_1c00df10 is a bare `ret`. */
    e->restart = 0xff;
    e->speaking = e->st_310 != 0x65; /* 0x1c003a2a */
    return SV_NAR_OK;
}

void sv_narrate_free(sv_engine *e)
{
    free(e->records);
    e->records = NULL;
    free(e->commands);
    e->commands = NULL;
    free(e->into_base);
    e->into_base = NULL;
    free(e->frames);
    e->frames = NULL;
}
