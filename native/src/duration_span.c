/*
 * duration_span.c — TISPAN32 vtable +0x04, 0x1c4010d0..0x1c4018a0.
 * Reconstructed from the original i386 instructions. Window loading and
 * inline duration/pause commands share English's layout; the percentage
 * cascade and final 15/13 adjustment are specific to Spanish.
 */
#include "tispeech/narrate.h"

static int32_t scale(int32_t p, int32_t k)
{
    return (p * k + 50) / 100;
}

int sv_span_duration(sv_engine *e)
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

    /* 0x1c401249: inline commands on the record itself. */
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
        /* 0x1c401345: punctuation takes a fixed pause. */
        if (code[2] == 4)
            dur = e->pause_short;
        else if (cls[2] & 1)
            dur = e->pause_long;
        else
            dur = e->pause_comma;
        goto store;
    }

    /* TISPAN32 0x1c401382..0x1c4017aa. Unlike English, Spanish
     * scales a stress-selected base duration rather than interpolating
     * between minimum and inherent duration. Preserve each rounding step. */
    int32_t pct = (fl[2] & 4) ? 130 : 100;
    if ((cls[2] & 0x18000u) && (fl[2] & 4))
        pct = scale(pct, 140);
    uint32_t vowel = cls[2] & 0x10000000u;
    if (vowel) {
        if (st[2] >= 6 && st[2] <= 9)
            pct = scale(pct, 110 + (st[2] - 6) * 10);
        if (fl[2] & 0x40) {
            pct = scale(pct, 80);
            if (st[2] == 0)
                pct = scale(pct, 80);
        }
        if ((int16_t)code[1] < 9)
            pct = scale(pct, 120);
        int32_t factor = 100;
        if ((fl[2] & 8) && (st[2] != 0 || (fl[2] & 0x40)))
            factor = 120;
        if (cls[3] & 0x01000000u)
            factor = 85;
        if ((cls[3] & 0x1000u) && (cls[3] & 0x08000000u))
            factor = 130;
        pct = scale(pct, factor);
        if (st[2] != 0 && (cls[3] & 2) && !(fl[3] & 0x80))
            pct = scale(pct, 70);
        if (code[3] == 0x1e || code[3] == 0x36 || code[3] == 0x6c)
            pct = scale(pct, 75);
        if (st[2] == 0) {
            if (st[3] != 0 && (cls[3] & 0x10000000u))
                pct = scale(pct, 150);
            else if (st[1] != 0 && (cls[1] & 0x10000000u))
                pct = scale(pct, 130);
        }
    }
    if (cls[2] & 2) {
        if (cls[2] & 0x00400000u)
            pct = scale(pct, 75);
        if ((int16_t)code[1] > 8)
            pct = scale(pct, 85);
        if ((int16_t)code[1] < 9 && (cls[2] & 0x30000u))
            pct = scale(pct, st[2] ? 200 : 140);
        if (cls[1] & 0x10000000u) {
            if (cls[3] & 2)
                pct = scale(pct, st[1] ? ((cls[3] & 0x08000000u) ? 200 : 160) : 130);
            else
                pct = scale(pct, 130);
        }
        if ((code[2] == 0x52 && code[1] == 0x3d) ||
            (code[2] == 0x51 && code[1] == 0x3c) ||
            (code[2] == 0x54 && code[1] == 0x3e))
            pct = scale(pct, 150);
    }

    /* 0x1c4017c0: no +50 rounding here, and no English minimum blend. */
    const uint8_t *def = r->phonemes + (ptrdiff_t)(int16_t)code[2] * SV_PH_STRIDE;
    int32_t base = (int16_t)sv_rd16(def + (st[2] ? 0x14 : 0x12));
    if (pct >= 300)
        pct = 300;
    dur = base * pct / 100;
    if (vowel)
        dur = (int32_t)((uint32_t)((int32_t)e->rate * dur) / 100u);

store:
    if (override)
        dur = override;
    dur = dur * pscale / 100;
    dur = dur * 15 / 13; /* 0x1c401863: includes punctuation and overrides */
    if (dur >= 4000)
        dur = 4000;
    if (dur <= 25)
        dur = 25;
    r->duration = (uint16_t)dur;
    return 0;
}
