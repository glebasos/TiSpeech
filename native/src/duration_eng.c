/*
 * duration_eng.c — English segment durations, TIENG32 vtable +0x04.
 *
 * TIENG32!0x1c2010d0..0x1c201a4c. Called once per record by
 * TIBASE32!FUN_1c00c3e0 with the current record in engine window[2]. A
 * Klatt-style rule cascade: a percentage starts at 100, each rule scales it,
 * and the duration is
 *
 *     MINDUR + (INHDUR - MINDUR) * PRCNT / 100
 *
 * with INHDUR and MINDUR the int16 fields at +0x14 and +0x12 of the phoneme
 * definition. Every scaling is `(p * k + 50) / 100` in signed 32-bit, which
 * the original spells with lea/shl chains; the factor is given in each
 * comment.
 *
 * The original keeps the five-record window (codes, classes, flags, stress of
 * records i-2..i+2) in TIENG32 .data at 0x1c255304..0x1c255368. Nothing else
 * in the module reads those globals, so they are locals here.
 */

#include "tispeech/narrate.h"

static int32_t scale(int32_t p, int32_t k)
{
    return (p * k + 50) / 100;
}

int sv_eng_duration(sv_engine *e)
{
    sv_record *r = e->window[2];
    uint16_t code[5];
    uint32_t cls[5], fl[5];
    uint16_t st[5];

    for (int k = 0; k < 3; k++) {
        const sv_record *q = r + k - 2;
        code[k] = q->code;
        cls[k] = sv_rec_class(q);
        fl[k] = q->flags;
        st[k] = q->stress;
    }
    if (r[1].code == SV_RECORD_END) {
        code[3] = 0; cls[3] = 0x100000; fl[3] = 0; st[3] = 0;
        code[4] = 0; cls[4] = 0x100000; fl[4] = 0; st[4] = 0;
    } else {
        code[3] = r[1].code;
        cls[3] = sv_rec_class(&r[1]);
        fl[3] = r[1].flags;
        st[3] = r[1].stress;
        if (r[2].code == SV_RECORD_END) {
            code[4] = 0; cls[4] = 0x100000; fl[4] = 0; st[4] = 0;
        } else {
            code[4] = r[2].code;
            cls[4] = sv_rec_class(&r[2]);
            fl[4] = r[2].flags;
            st[4] = r[2].stress;
        }
    }
    (void)fl[0]; (void)fl[1]; (void)fl[3]; (void)fl[4];
    (void)st[0]; (void)st[1]; (void)st[4];

    /* 0x1c20124c: inline commands on the record itself. */
    int32_t override = 0, pscale = 100;
    const int16_t *c = r->commands;
    if (c && c[0] != SV_CMD_END) {
        do {
            int16_t v = c[1];
            switch (c[0]) {
            case 0x28:
            case 0x50:
                if (v < 0)
                    pscale = (v * pscale) / -100;
                else
                    override += v;
                break;
            case 0x32:
                e->rate = (uint32_t)(int32_t)(int16_t)sv_rd16(
                    r->lang->voices + v * 74 + 0x24);
                break;
            case 0x5a: e->pause_comma = (uint16_t)v; break;
            case 0x64: e->pause_short = (uint16_t)v; break;
            case 0x6e: e->pause_long = (uint16_t)v; break;
            case 0x136: e->rate = (uint32_t)(int32_t)v; break;
            default: break;
            }
            c += 3;
        } while (c[0] != SV_CMD_END);
    }

    if (cls[2] & 0x100000u)
        return 0;

    int32_t dur;
    if ((cls[2] & 0x02000000u) && code[2] != 0x1b) {
        /* 0x1c201347: punctuation takes a fixed pause. */
        if (code[2] == 4)
            dur = e->pause_short;
        else if (cls[2] & 1)
            dur = e->pause_long;
        else
            dur = e->pause_comma;
        goto store;
    }

    int32_t pct = 100;
    uint32_t emph = fl[2] & 4;
    if (emph)
        pct = 130;
    if ((cls[2] & 0x18000u) && emph)
        pct = scale(pct, 140);
    uint32_t vowel = cls[2] & 0x10000000u;
    if (vowel) {
        if (!emph)
            pct = scale(pct, 60);
        if (!(fl[2] & 8))
            pct = scale(pct, 85);
        if (fl[2] & 0x40)
            pct = scale(pct, 80);
        if (st[2] < 3)
            pct = scale(pct, 60);
        static const int32_t by_stress[10] = {100, 90, 93, 96, 100, 100, 110, 120, 130, 140};
        pct = scale(st[2] <= 9 ? by_stress[st[2]] : 100, pct);
        if (code[2] == 0x29 && code[1] == 0x39)
            pct = scale(pct, 65);
        int32_t f = 100;
        if (st[2] != 0) {
            if ((cls[3] & 0x20000000u) || code[3] == 0) {
                f = 130;
            } else if ((cls[3] & 0x1000u) && !(cls[3] & 0x4000u) &&
                       (cls[3] & 0x08000000u)) {
                f = 150;
            } else if (cls[3] & 0x800000u) {
                f = 120;
            } else if ((cls[3] & 0x8000u) && st[3] == 0) {
                f = (sv_class(r->lang, code[4]) & 0x01000000u) ? 60 : 85;
            } else if ((cls[3] & 0x01000000u) || code[3] == 0x3f) {
                f = (code[3] == 0x54 || code[3] == 0x55) ? 55 : 70;
            }
        }
        if (!emph)
            f = (f * 30 + 15) / 40 + 25;
        pct = scale(f, pct);
    }

    uint32_t cons = cls[2] & 2;
    if (cons) {
        if ((int16_t)code[1] > 8)
            pct = scale(pct, 85);
        if ((cls[2] & 0x1800000u) && code[1] == 0x41)
            pct = scale(pct, 60);
        if (code[0] == 0x41 && (cls[1] & 0x1800000u) && (cls[2] & 0x30000u))
            pct = scale(pct, 50);
        if (code[1] == 0x41 && (cls[2] & 0x8000u))
            pct = scale(pct, 60);
        if ((cls[2] & 0x1000u) && !(cls[2] & 0x08000000u) && emph)
            pct = scale(pct, 150);
        if (st[2] < 3) {
            if ((cls[2] & 0x30000u) && (cls[3] & 0x10000000u))
                pct = scale(pct, 20);
            else
                pct = scale(pct, 70);
        }
        if (code[2] == 0x39 && code[3] == 0x29)
            pct = scale(pct, 70);
    }

    /* 0x1c20181d: a pause on either side is looked through. */
    uint32_t save1 = cls[1], save3 = cls[3];
    if (code[1] == 0)
        cls[1] = cls[0];
    if (code[3] == 0)
        cls[3] = cls[4];
    if (vowel) {
        if (cls[3] & 0x10000000u)
            pct = scale(pct, 120);
        if (cls[1] & 0x10000000u)
            pct = scale(pct, 70);
    } else if (cons) {
        uint32_t a = cls[1] & 2;
        if (a && (cls[3] & 2))
            pct = scale(pct, 50);
        else if (a || (cls[3] & 2))
            pct = scale(pct, 70);
    }
    cls[1] = save1;
    cls[3] = save3;

    const uint8_t *def = r->phonemes + (ptrdiff_t)(int16_t)code[2] * SV_PH_STRIDE;
    int32_t inh = (int16_t)sv_rd16(def + 0x14);
    int32_t mn = (int16_t)sv_rd16(def + 0x12);
    if (st[2] == 0)
        mn = (mn + 1) >> 1;
    if (pct >= 0xaa)
        pct = 0xaa;
    dur = mn + ((inh - mn) * pct + 50) / 100;
    if (st[2] != 0 && (cls[2] & 4) && (save1 & 0x01000000u))
        dur += 25;
    if (vowel && code[2] != 0x17 && code[2] != 0x18)
        dur = (int32_t)((uint32_t)((int32_t)e->rate * dur) / 100u);

store:
    if (override)
        dur = override;
    dur = dur * pscale / 100;
    if (dur >= 0xfa0)
        dur = 0xfa0;
    if (dur <= 0x19)
        dur = 0x19;
    r->duration = (uint16_t)dur;
    return 0;
}
