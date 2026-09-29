/*
 * langgen.c — the language module's frame generator, vtable slot +0x08.
 *
 * TIENG32!0x1c204690 and everything it calls, written against the English
 * addresses and used for both languages. TISPAN32's +0x08 (0x1c404020) is
 * the same program with Spanish rules in four places: formant targets
 * (formant_targets_span), voicing targets (voicing_targets_span), frication
 * and the frame loop (both marked `span`). Its globals sit at a constant
 * 0x1c1c50 from English's except one extra flag (GLIDE), so the Spanish code
 * uses the English names; tools/extract_span_frontend.py builds the image.
 *
 * HOW THIS FILE IS WRITTEN
 * ------------------------
 * The generator keeps all of its working state in module globals: seventeen
 * 0x40-byte parameter tracks, a 0x400-byte contour buffer per track, a
 * cursor per track into that buffer, and a five-record window of phoneme
 * codes, attribute words and flags. The helpers communicate only through
 * those globals. Rather than invent a layout for ~200 of them, each module
 * instance owns a mutable copy of the module's .data section and this file
 * addresses globals and tables by their ORIGINAL virtual address through the
 * accessors below. Pointers the original stores inside .data (a track's
 * contour buffer, the per-track cursors) are kept as those same 32-bit VAs,
 * so the arithmetic on them is the original's. Engine state, records and
 * frames are native and are reached through sv_engine as in narrate.c.
 *
 * The names are the module's own: the pointer at +0x00 of each track is a
 * two-letter string in TIENG32 .data (0x1c24c0d4..0x1c24c117), which is how
 * the tracks are identified — F0 F1 F2 F3, B1 B2 B3, AV AF AH AK, K1 Q1, TL,
 * DI, MH, MW.
 */

#include "tispeech/narrate.h"

#include <stdlib.h>
#include <string.h>

#include "langmod_priv.h"

#define SV_LANG_SPANISH 2   /* sv_langmod.id, as svapi.h numbers languages */

/* Native frames, by byte offset from a frame pointer. */
#define FB(p, off) (((uint8_t *)(p))[(off)])
#define FW(p, off) (*(sv_au16 *)((uint8_t *)(p) + (off)))

/* The seventeen tracks (TIENG32 0x1c250e90, one-time init at 0x1c2046db). */
enum {
    F0 = 0x1c250690, F1 = 0x1c2506d0, F2 = 0x1c250750, F3 = 0x1c250790,
    B1 = 0x1c2505b0, B2 = 0x1c250600, B3 = 0x1c250640, AV = 0x1c2508b0,
    AF = 0x1c250710, AH = 0x1c2507d0, AK = 0x1c250820, K1 = 0x1c2508f0,
    Q1 = 0x1c250930, TL = 0x1c250a10, DI = 0x1c250870, MH = 0x1c250970,
    MW = 0x1c2509c0
};
/* A track field: T(F2, 0x1c) is the dword at F2+0x1c. */
#define T(trk, off) L((trk) + (off))

/* Per-track cursor into the contour buffer; CV(n) is the value under it. */
enum {
    CUR_F0 = 0x1c250574, CUR_F1 = 0x1c25057c, CUR_F2 = 0x1c250580,
    CUR_F3 = 0x1c250588, CUR_B1 = 0x1c250568, CUR_B2 = 0x1c25056c,
    CUR_B3 = 0x1c250570, CUR_AV = 0x1c250598, CUR_AF = 0x1c250578,
    CUR_AH = 0x1c250584, CUR_AK = 0x1c250590, CUR_K1 = 0x1c25059c,
    CUR_Q1 = 0x1c2505a0, CUR_TL = 0x1c250864, CUR_DI = 0x1c250594,
    CUR_MH = 0x1c2505a4, CUR_MW = 0x1c250680
};
#define CV(cur) W(L(cur))

/* The five-record window, filled at 0x1c2049f6..0x1c204bc8 from engine
 * window[0..4]: code (byte), attribute word, flags word, flags & 0x10,
 * stress (byte), frame count, manner class. [2] is the current record. */
#define CODE0  B(0x1c255301)
#define CODE1  B(0x1c255303)
#define CODE2  B(0x1c250e7e)
#define CODE3  B(0x1c250810)
#define CODE4  B(0x1c2509b4)
#define ATTR0  ((uint32_t)L(0x1c250ed8))
#define ATTR1  ((uint32_t)L(0x1c250e64))
#define ATTR2  ((uint32_t)L(0x1c250e70))
#define ATTR3  ((uint32_t)L(0x1c2552fc))
#define ATTR4  ((uint32_t)L(0x1c250e5c))
#define FLAGS0 ((uint32_t)L(0x1c2552f8))
#define FLAGS1 ((uint32_t)L(0x1c2552f0))
#define FLAGS2 ((uint32_t)L(0x1c250e78))
#define FLAGS3 ((uint32_t)L(0x1c250554))
#define FLAGS4 ((uint32_t)L(0x1c250e54))
#define F10_0  B(0x1c250edc)
#define F10_1  B(0x1c250e68)
#define F10_2  B(0x1c250e74)
#define F10_3  B(0x1c255300)
#define F10_4  B(0x1c250e62)
#define NFR2   UW(0x1c250566)
#define NFR3   UW(0x1c250e60)
#define CLS1   UW(0x1c2505f0)
#define CLS2   UW(0x1c250564)
#define CLS3   UW(0x1c250e7c)

/* 26-byte phoneme definition fields. */
static int32_t ph16(const uint8_t *ph, unsigned code, unsigned off)
{
    return (int16_t)sv_rd16(ph + code * SV_PH_STRIDE + off);
}

/* ------------------------------------------------------------------------ */
/* Leaves                                                                    */
/* ------------------------------------------------------------------------ */

/* 0x1c202fe0: roll a track's targets forward one phoneme. */
static void track_shift(sv_engine *e, uint32_t trk)
{
    T(trk, 0x10) = T(trk, 0x08);
    T(trk, 0x08) = T(trk, 0x18);
    T(trk, 0x14) = T(trk, 0x0c);
    T(trk, 0x0c) = T(trk, 0x1c);
    T(trk, 0x20) = T(trk, 0x24);
}

/* 0x1c203010: manner class of a record, from its own definition table.
 * The same decision tree as sv_gen_phoneme_class() in generator.c. */
static uint16_t manner_class(const sv_record *r)
{
    if (r->code == SV_RECORD_END)
        return 9;
    uint32_t f = sv_rd32(r->phonemes + (size_t)r->code * SV_PH_STRIDE + 2);
    if (f & 0x10000000u) return 0;
    if (f & 0x00002000u) return 1;
    if (f & 0x00008000u) return 2;
    if (f & 0x00000040u) return (f & 0x08000000u) ? 3 : 5;
    if (f & 0x00040000u) return (f & 0x08000000u) ? 4 : 6;
    if (f & 0x00000004u) return 7;
    if (f & 0x20000000u) return 9;
    return (f & 0x100u) ? 8 : 9;
}

static int32_t sar32(int32_t v, int s)
{
    return v < 0 ? (int32_t)~(~(uint32_t)v >> s) : (int32_t)((uint32_t)v >> s);
}

/* 0x1c203870: one track's contour for the current phoneme. The arithmetic
 * is src/generator.c's sv_gen_track_contour(), which is verified against the
 * original; it is restated here against the .data copy because the original
 * also runs with a frame count of 0 (records of zero duration), where it
 * writes 256 into the ramp and 0 into the word just below it and nothing
 * else. generator.c refuses that case. */
static void track_contour(sv_engine *e, uint32_t trk)
{
    int32_t n = (int32_t)NFR2;
    int32_t hold = (int32_t)((uint32_t)T(trk, 0x38) * (uint32_t)n) / 100 + 1;
    int32_t fall = ((int32_t)((uint32_t)T(trk, 0x3c) * (uint32_t)n) + 50) / 100;
    int32_t span = fall - hold;
    if (span <= 1)
        span = 1;
    int32_t step = 0x8000 / span, acc = 0x8000 - step;
    sv_a16 *ramp = (sv_a16 *)DP(0x1c250a50);
    sv_a16 *fwd = (sv_a16 *)DP(0x1c250150);
    sv_a16 *out = (sv_a16 *)DP((uint32_t)T(trk, 0x04));

    int32_t row = T(trk, 0x34);
    if (row >= 8)
        row = 8;
    if (row <= 0)
        row = 0;
    int32_t fr = L(0x1c24c1b0 + 4 * (row * 16 + T(trk, 0x2c)));
    int32_t br = L(0x1c24c1b0 + 4 * (row * 16 + T(trk, 0x30)));

    for (int32_t i = 0; i < hold; i++)
        ramp[i] = 256;
    for (int32_t i = hold; i < n; i++) {
        if (acc < 0)
            acc = 0;
        ramp[i] = (int16_t)sar32(acc, 7);
        acc -= step;
    }
    ramp[n - 1] = 0;

    int32_t v = T(trk, 0x20), tgt = T(trk, 0x08);
    for (int32_t i = 0; i < n; i++) {
        v += sar32((int32_t)((uint32_t)(tgt - v) * (uint32_t)fr), 8);
        fwd[i] = (int16_t)sar32((int32_t)((uint32_t)ramp[i] * (uint32_t)v), 8);
    }
    v = T(trk, 0x24);
    tgt = T(trk, 0x0c);
    for (int32_t i = n - 1; i >= 0; i--) {
        int32_t w = 256 - ramp[i];
        out[i] = (int16_t)(uint16_t)((uint32_t)sar32((int32_t)((uint32_t)w * (uint32_t)v), 8)
                                     + (uint16_t)fwd[i]);
        v += sar32((int32_t)((uint32_t)(tgt - v) * (uint32_t)br), 8);
    }
}

/* 0x1c203a30: hold AF at a burst level for the first frames of a stop. */
static void burst(sv_engine *e)
{
    if (W(0x1c25058c) <= 0 || W(0x1c25055c) <= 0)
        return;
    sv_a16 *p = (sv_a16 *)DP((uint32_t)L(0x1c250714) + 2u * NFR2);
    for (int16_t k = 0; W(0x1c25058c) > k; k++) {
        p--;
        *p = W(0x1c25055c);
    }
    if (ATTR2 & 1) {
        p[0] = (int16_t)(p[0] - 8);
        p[1] = (int16_t)(p[1] - 4);
        sv_a16 *q = (sv_a16 *)DP((uint32_t)L(0x1c250714) + 2u * NFR2 - 2);
        q[0] = (int16_t)(q[0] - 8);
        q[-1] = (int16_t)(q[-1] - 4);
    }
}

/* 0x1c203770: B1/B2/B3 targets for nasalised vowels, from the 21-entry
 * (code, low, high) table at 0x1c24c420. */
static void nasal_bandwidths(sv_engine *e)
{
    uint32_t a = ATTR2;
    if (!(a & 0x10))
        return;
    int16_t k = 0;
    uint8_t cl = CODE2;
    while (B(0x1c24c420 + 3 * k) != cl) {
        k++;
        if (k >= 0x15)
            break;
    }
    int16_t lo = B(0x1c24c421 + 3 * k), hi = B(0x1c24c422 + 3 * k);
    if ((a & 0xc0000000u) && (ATTR3 & 0x2000)) {
        lo = (int16_t)(lo - 10);
        hi = (int16_t)(hi - 15);
    }
    if (lo >= 0x5f)
        lo = 0x5f;
    if (lo <= 0)
        lo = 0;
    if (hi >= 0x64)
        hi = 0x64;
    int32_t h = hi;
    if (h <= lo + 5)
        h = lo + 5;
    T(B1, 0x38) = lo; T(F1, 0x38) = lo; T(B2, 0x38) = lo;
    T(F2, 0x38) = lo; T(B3, 0x38) = lo; T(F3, 0x38) = lo;
    T(B1, 0x3c) = (int16_t)h; T(F1, 0x3c) = (int16_t)h; T(B2, 0x3c) = (int16_t)h;
    T(F2, 0x3c) = (int16_t)h; T(B3, 0x3c) = (int16_t)h; T(F3, 0x3c) = (int16_t)h;
    if (cl == 0x23) {
        T(B1, 0x38) = 0x32; T(F1, 0x38) = 0x32;
        T(B1, 0x3c) = 0x5f; T(F1, 0x3c) = 0x5f;
    }
}

/* 0x1c204500 / 0x1c204550 / 0x1c2045b0 / 0x1c204610: scale a pitch target
 * by the voice's range above 110 and write it into one frame of the
 * phoneme: the first, the middle, the last, or the middle of the NEXT
 * phoneme (whose own generation may overwrite it). */
static uint16_t pitch_target(const sv_engine *e, int32_t v)
{
    if (v > 0x6e)
        v = ((int32_t)e->st_60 * (v - 0x6e)) / 100 + 0x6e;
    return (uint16_t)(((int32_t)e->st_46 * v) / 0x6e);
}
static void pitch_first(sv_engine *e, void *fr, int32_t v)
{
    FW(fr, 0x18) = pitch_target(e, v);
}
static void pitch_mid(sv_engine *e, void *fr, int32_t v)
{
    FW(fr, (int32_t)((NFR2 & ~1u) << 4) + 0x18) = pitch_target(e, v);
}
static void pitch_last(sv_engine *e, void *fr, int32_t v)
{
    FW(fr, (int32_t)((uint32_t)NFR2 << 5) - 8) = pitch_target(e, v);
}
static void pitch_next(sv_engine *e, void *fr, int32_t v)
{
    FW(fr, (int32_t)(((NFR3 & ~1u) << 4) + ((uint32_t)NFR2 << 5)) + 0x18) =
        pitch_target(e, v);
}

/* ------------------------------------------------------------------------ */
/* 0x1c201b80 — formant and bandwidth targets                                */
/*                                                                           */
/* Each formant track (F1..F3, B1..B3) is rolled forward and given the NEXT  */
/* phoneme's targets from its definition entry (+6 F1, +8 F2, +0xa F3,       */
/* +0xc B1, +0xe B2, +0x10 B3): +0x18 where that phoneme starts, +0x1c where */
/* it ends (diphthongs take the end from the following entry). Then a long  */
/* run of coarticulation rules adjusts targets (+0x18/+0x1c), the start of  */
/* the current phoneme (+0x20), the backward start (+0x24), the transition  */
/* percentage (+0x28) and the two rate columns (+0x2c/+0x30).                */
/* ------------------------------------------------------------------------ */

#define FB6(stmt)                                                            \
    do { uint32_t t_[6] = {B1, F1, B2, F2, B3, F3};                          \
         for (int k_ = 0; k_ < 6; k_++) { uint32_t trk = t_[k_]; stmt; } } while (0)

static void formant_targets(sv_engine *e, const sv_record *cur)
{
    const uint8_t *ph = cur->phonemes;
    track_shift(e, F1);
    track_shift(e, B1);
    track_shift(e, F2);
    track_shift(e, B2);
    track_shift(e, F3);
    track_shift(e, B3);

    unsigned code = (CODE3 == 0x4b || CODE3 == 0x4c) ? CODE4 : CODE3;
    const uint8_t *def = ph + code * SV_PH_STRIDE;
#define DEF(off) ((int32_t)(int16_t)sv_rd16(def + (off)))
    T(F1, 0x18) = DEF(0x06);
    T(B1, 0x18) = DEF(0x0c);
    T(F2, 0x18) = DEF(0x08);
    T(B2, 0x18) = DEF(0x0e);
    T(F3, 0x18) = DEF(0x0a);
    T(B3, 0x18) = DEF(0x10);
    if (ATTR3 & 0x10) {
        T(F1, 0x1c) = DEF(0x20);
        T(B1, 0x1c) = DEF(0x26);
        T(F2, 0x1c) = DEF(0x22);
        T(B2, 0x1c) = DEF(0x28);
        T(F3, 0x1c) = DEF(0x24);
        T(B3, 0x1c) = DEF(0x2a);
    } else {
        T(F1, 0x1c) = T(F1, 0x18);
        T(B1, 0x1c) = T(B1, 0x18);
        T(F2, 0x1c) = T(F2, 0x18);
        T(B2, 0x1c) = T(B2, 0x18);
        T(F3, 0x1c) = T(F3, 0x18);
        T(B3, 0x1c) = T(B3, 0x18);
    }
    if (CODE3 == 0x4b || CODE3 == 0x4c) {
        /* 0x1c201cec */
        int32_t v = DEF(0x0c) + 0xc8;
        T(B1, 0x1c) = v;
        T(B1, 0x18) = v;
        v = DEF(0x0e) + 0x28;
        if (v <= 0xc8)
            v = 0xc8;
        T(B2, 0x1c) = v;
        T(B2, 0x18) = v;
        v = DEF(0x10) + 0x3c;
        if (v <= 0x12c)
            v = 0x12c;
        T(B3, 0x1c) = v;
        T(B3, 0x18) = v;
        if (!(ATTR2 & 0x20000000u)) {
            T(F1, 0x18) = (T(F1, 0x1c) + T(F1, 0x0c)) >> 1;
            T(F2, 0x18) = (T(F2, 0x0c) + T(F2, 0x1c)) >> 1;
            T(F3, 0x18) = (T(F3, 0x1c) + T(F3, 0x0c)) >> 1;
        }
    }
#undef DEF
    if (ATTR3 & 0x20000000u) {
        /* 0x1c201d84: into a pause, hold the current formants. */
        T(F1, 0x18) = T(F1, 0x0c);
        T(B1, 0x18) = T(B1, 0x0c);
        T(F2, 0x18) = T(F2, 0x0c);
        T(B2, 0x18) = T(B2, 0x0c);
        T(F3, 0x18) = T(F3, 0x0c);
        T(B3, 0x18) = T(B3, 0x0c);
    }
    if (ATTR2 & 0x20000000u) {
        /* 0x1c201dd1: out of a pause, start at the next targets. */
        T(F1, 0x0c) = T(F1, 0x18);
        T(B1, 0x0c) = T(B1, 0x1c);
        T(F2, 0x0c) = T(F2, 0x18);
        T(B2, 0x0c) = T(B2, 0x1c);
        T(F3, 0x0c) = T(F3, 0x18);
        T(B3, 0x0c) = T(B3, 0x1c);
    }

    if (ATTR3 & 0x10000000u) {
        /* 0x1c201e22 */
        if (CODE3 == 0x1c && (ATTR1 & 0x800000u) && !(ATTR1 & 0x8000u)) {
            int32_t v = (T(F2, 0x18) + T(F2, 0x10)) >> 1;
            T(F2, 0x1c) = v;
            T(F2, 0x18) = v;
        }
        if ((ATTR3 & 0x80) && (ATTR3 & 0x1000) && (ATTR4 & 0x4000000u))
            T(F2, 0x1c) = T(F2, 0x18);
        if ((ATTR3 & 0x40000080u) && (ATTR4 & 0x800))
            T(F2, 0x1c) -= 0x12c;
        if ((ATTR3 & 0x40000000u) && (ATTR4 & 2))
            T(F2, 0x1c) -= 0x96;
        if (ATTR3 & 0x400000u) {
            int32_t v = (ph16(ph, CODE4, 0xa) + T(F3, 0x18) + T(F3, 0x08)) / 3;
            T(F3, 0x18) = v;
            T(F3, 0x1c) = v;
        }
    } else if ((ATTR3 & 0x40) && (ATTR3 & 0x10000) && (ATTR4 & 0x10000000u) &&
               (ATTR4 & 0x200000u)) {
        /* 0x1c201f17 */
        T(F2, 0x18) -= 0x32;
        T(F3, 0x18) -= 0xc8;
        T(F2, 0x1c) = T(F2, 0x18);
        T(F3, 0x1c) = T(F3, 0x18);
    }

    if ((ATTR3 & 2) && (ATTR2 & 0x180000u)) {
        T(F2, 0x1c) = 0x73a; T(F2, 0x18) = 0x73a;
        T(F3, 0x1c) = 0x898; T(F3, 0x18) = 0x898;
    }
    uint32_t r_edx = ATTR3 & 0x4000000u;
    if (r_edx && (ATTR2 & 0x180000u)) {
        T(F2, 0x1c) = 0x6a4; T(F2, 0x18) = 0x6a4;
        T(F3, 0x1c) = 0x76c; T(F3, 0x18) = 0x76c;
    }
    if (r_edx && (ATTR2 & 0x80000000u)) {
        T(F2, 0x1c) = 0x384; T(F2, 0x18) = 0x384;
        T(F3, 0x1c) = 0x9c4; T(F3, 0x18) = 0x9c4;
    }
    if (CODE3 == 0x37) {
        /* 0x1c201fe4 */
        if ((ATTR4 & 0x800000u) && !(ATTR4 & 0x8000u)) {
            int32_t v = (ph16(ph, CODE4, 8) + T(F2, 0x18) * 9) / 10;
            T(F2, 0x1c) = v;
            T(F2, 0x18) = v;
        }
        if (!(ATTR4 & 0x800000u) || (ATTR4 & 0x8000u)) {
            int32_t v = (T(F2, 0x18) * 9 + 0x91) / 10;
            T(F2, 0x1c) = v;
            T(F2, 0x18) = v;
        }
    }
    if (CODE3 == 0x36) {
        /* 0x1c20206f */
        int32_t v = (ph16(ph, CODE4, 8) + T(F2, 0x18) * 3) * 25 / 100;
        T(F2, 0x18) = v;
        T(F2, 0x1c) = (ph16(ph, CODE4, 8) + T(F2, 0x18) * 2) / 3;
        int32_t f3 = T(F2, 0x18) + 0xfa;
        T(F3, 0x18) = f3;
        T(F3, 0x1c) = (ph16(ph, CODE4, 0xa) + f3 * 2) / 3;
    }
    if ((ATTR3 & 0x800) && CODE4 == 0x29)
        T(F1, 0x1c) -= 0x50;
    if (CODE3 == 0x3f) {
        /* 0x1c20210c */
        int32_t v = (ph16(ph, CODE4, 6) + T(F1, 0x08) * 2 + T(F1, 0x18)) >> 2;
        T(F1, 0x1c) = v;
        T(F1, 0x18) = v;
        v = (ph16(ph, CODE4, 8) + T(F2, 0x18) + T(F2, 0x08)) / 3;
        T(F2, 0x1c) = v;
        T(F2, 0x18) = v;
        v = (ph16(ph, CODE4, 0xa) + T(F3, 0x18) + T(F3, 0x08)) / 3;
        T(F3, 0x1c) = v;
        T(F3, 0x18) = v;
    }
    if (CODE3 == 0x40) {
        /* 0x1c202193 */
        int32_t v = (ph16(ph, CODE4, 6) + T(F1, 0x0c)) >> 1;
        T(F1, 0x1c) = v;
        T(F1, 0x18) = v;
        v = (ph16(ph, CODE4, 8) + T(F2, 0x0c)) >> 1;
        T(F2, 0x1c) = v;
        T(F2, 0x18) = v;
        v = (ph16(ph, CODE4, 0xa) + T(F3, 0x0c)) >> 1;
        T(F3, 0x1c) = v;
        T(F3, 0x18) = v;
    }
    if (CODE3 == 0x3e && (ATTR2 & 0x40000080u)) {
        T(F1, 0x18) -= 0x64;
        T(F2, 0x18) += 0x28a;
        T(F1, 0x1c) = T(F1, 0x18);
        T(F2, 0x1c) = T(F2, 0x18);
        T(F3, 0x18) += 0x2ee;
        T(F3, 0x1c) = T(F3, 0x18);
    }
    if ((ATTR2 & 8) && CODE3 == 0x36) {
        /* 0x1c20225a: the +0x18 targets come from phoneme 0x18's entry
         * (0x270 = 24 * 26), whatever the next phoneme is. */
        T(F1, 0x1c) = T(F1, 0x18);
        T(F2, 0x1c) = T(F2, 0x18);
        T(F3, 0x1c) = T(F3, 0x18);
        T(F1, 0x18) = ph16(ph, 0x18, 6);
        T(F2, 0x18) = ph16(ph, 0x18, 8);
        T(F3, 0x18) = ph16(ph, 0x18, 0xa);
        T(B1, 0x18) = ph16(ph, 0x18, 0xc);
        T(B2, 0x18) = ph16(ph, 0x18, 0xe);
        T(B3, 0x18) = ph16(ph, 0x18, 0x10);
    }
    if (CODE3 == 0x29 && (ATTR4 & 0x800)) {
        /* 0x1c2022d8: average the end targets with phoneme 0x1e's entry
         * (0x30c = 30 * 26). */
        T(F1, 0x1c) = (ph16(ph, 0x1e, 6) + T(F1, 0x1c)) >> 1;
        T(F2, 0x1c) = (ph16(ph, 0x1e, 8) + T(F2, 0x1c)) >> 1;
        T(F3, 0x1c) = (ph16(ph, 0x1e, 0xa) + T(F3, 0x1c)) >> 1;
        T(B1, 0x1c) = (ph16(ph, 0x1e, 0xc) + T(B1, 0x1c)) >> 1;
        T(B2, 0x1c) = (ph16(ph, 0x1e, 0xe) + T(B2, 0x1c)) >> 1;
        T(B3, 0x1c) = (ph16(ph, 0x1e, 0x10) + T(B3, 0x1c)) >> 1;
    }
    /* 0x1c202350: keep F1 < F2 < F3 by fixed margins. */
    if (T(F3, 0x18) - T(F2, 0x18) < 0xfa)
        T(F2, 0x18) = T(F3, 0x18) - 0xfa;
    if (T(F3, 0x1c) - T(F2, 0x1c) < 0xfa)
        T(F2, 0x1c) = T(F3, 0x1c) - 0xfa;
    if (T(F2, 0x18) - T(F1, 0x18) < 0xc8)
        T(F1, 0x18) = T(F2, 0x18) - 0xc8;
    if (T(F2, 0x1c) - T(F1, 0x1c) < 0xc8)
        T(F1, 0x1c) = T(F2, 0x1c) - 0xc8;

    nasal_bandwidths(e);

    uint32_t r_esp10 = ATTR3 & 0x10002000u;
    if (r_esp10) {
        if (ATTR4 & 0x8000u) {
            T(B1, 0x1c) += 0x64;
            T(B2, 0x1c) += 0x32;
            T(B3, 0x1c) += 0x32;
            T(F1, 0x1c) = (T(F1, 0x1c) * 2 + 0x1f4) / 3;
        }
        if (ATTR2 & 0x8000u) {
            T(B1, 0x18) += 0x64;
            T(B2, 0x18) += 0x32;
            T(B3, 0x18) += 0x32;
            T(F1, 0x18) = (T(F1, 0x18) * 2 + 0x1f4) / 3;
        }
    }

    /* 0x1c202457: transition percentage from a (previous kind, next kind)
     * table at 0x1c24c3f0, kinds 0..3 = vowel, 0x1000000, 0x40, other. */
    W(0x1c250562) = W(0x1c25055a);
    uint32_t r_ebx = ATTR3 & 0x10000000u;
    if (r_ebx)
        W(0x1c25055a) = 0;
    else if (ATTR3 & 0x1000000u)
        W(0x1c25055a) = 1;
    else if (ATTR3 & 0x40)
        W(0x1c25055a) = 2;
    else
        W(0x1c25055a) = 3;
    {
        int32_t v = B(0x1c24c3f0 + W(0x1c25055a) + W(0x1c250562) * 4);
        T(F3, 0x28) = v; T(F2, 0x28) = v; T(F1, 0x28) = v;
        T(B3, 0x28) = v; T(B2, 0x28) = v; T(B1, 0x28) = v;
    }
    if (CODE2 == 0x37)
        FB6(T(trk, 0x28) = 0x14);
    uint32_t r_edi = ATTR2 & 0x800000u;
    if (r_edi && (ATTR3 & 0x40)) {
        T(B1, 0x28) = 0x5a; T(F1, 0x28) = 0x5a;
    } else if ((ATTR2 & 0x40) && (ATTR3 & 0x800000u)) {
        T(B1, 0x28) = 0xa; T(F1, 0x28) = 0xa;
    }
    {
        int32_t v = -1;
        if ((ATTR2 & 0x1000000u) && (ATTR3 & 0x800000u)) {
            T(F1, 0x28) = (ATTR2 & 0x4000000u) ? 0x14 : 0x28;
            v = 0x46;
        } else if (r_edi && (ATTR3 & 0x1000000u)) {
            T(F1, 0x28) = 0x3c;
            v = 0x1e;
        } else if (ATTR3 & 0x20000000u) {
            T(F3, 0x28) = 0; T(F2, 0x28) = 0; T(F1, 0x28) = 0;
            v = 0x32;
            T(B3, 0x28) = v;
        }
        if (v >= 0) {
            T(B2, 0x28) = v;
            T(B1, 0x28) = v;
        }
    }
    if (ATTR2 & 0x20000000u) {
        T(F3, 0x28) = 0x64; T(F2, 0x28) = 0x64; T(F1, 0x28) = 0x64;
    }
    uint32_t r_esi = ATTR2 & 0x10000000u;
    if (r_esi && (ATTR3 & 0x4000000u))
        T(F2, 0x28) = 0x50;
    else if ((ATTR2 & 0x4000000u) && r_ebx)
        T(F2, 0x28) = 0x1e;
    if (r_edi && (ATTR3 & 0x800))
        FB6(T(trk, 0x28) = 0x5a);
    uint32_t r_ecx = ATTR2 & 0x80000u;
    {
        int32_t v = -1;
        if (r_ecx && (ATTR3 & 0x8000u))
            v = 0xa;
        else if ((ATTR2 & 0x8000u) && (ATTR3 & 0x80000u))
            v = 0x5a;
        if (v >= 0) {
            T(F3, 0x28) = v; T(F2, 0x28) = v; T(B3, 0x28) = v; T(B2, 0x28) = v;
        }
    }
    if ((ATTR2 & 0xc0100000u) && (ATTR3 & 0x2000))
        FB6(T(trk, 0x28) = 0x14);

    /* 0x1c2026e2: backward start = current + (next - current) * pct / 100. */
    FB6(T(trk, 0x24) = T(trk, 0x0c) + (T(trk, 0x18) - T(trk, 0x0c)) * T(trk, 0x28) / 100);

    uint32_t r_eax = ATTR1 & 2;
    if (r_eax && CODE1 != 0x3f && r_edi) {
        T(F2, 0x20) = 0x640;
        T(F3, 0x20) = r_ecx ? 0x8fc : 0xa28;
    }
    if ((ATTR2 & 2) && CODE2 != 0x3f && (ATTR3 & 0x800000u))
        T(F3, 0x24) = (ATTR3 & 0x80000u) ? 0x8fc : 0xa3c;
    if (r_eax && CODE2 == 0x29)
        T(F2, 0x20) += 0x12c;
    r_ecx = ATTR2 & 0x10002000u;
    if (r_ecx && (ATTR3 & 0x4000000u)) {
        T(B1, 0x24) += 0x64;
        T(F2, 0x24) = (T(F3, 0x0c) + T(F2, 0x18) + T(F2, 0x0c)) / 3 - 0x190;
    }
    r_eax = ATTR3 & 0x4000000u;
    if (r_eax && (ATTR2 & 0x40000080u))
        T(F2, 0x24) += 0x64;
    r_edx = ATTR2 & 0x4000000u;
    if (r_edx && (ATTR3 & 0x40000080u))
        T(F2, 0x24) += 0x64;
    if (r_edx)
        T(F3, 0x24) = T(F2, 0x24) + ((ATTR3 & 0x400) ? 0x320 : 0x190);
    if (r_eax)
        T(F3, 0x24) = T(F2, 0x24) + ((ATTR2 & 0x400) ? 0x320 : 0x258);
    if ((ATTR2 & 0x800) && r_ebx) {
        T(F1, 0x24) -= 0x3c;
        T(F2, 0x24) -= 0x32;
    } else if (r_esi && (ATTR1 & 0x800)) {
        T(F1, 0x20) += 0x64;
        T(F2, 0x20) += 0x64;
    }
    if ((ATTR2 & 0x20000100u) && CODE2 != 0x40) {
        T(F1, 0x20) = T(F1, 0x08);
        T(F2, 0x20) = T(F2, 0x08);
        T(F3, 0x20) = T(F3, 0x08);
    }
    if (r_edx && r_esp10) {
        /* 0x1c20298e: fixed F2/F3 loci for a stop before a vowel, by the
         * vowel's code. */
        static const struct { uint8_t code; int16_t f2, f3; } loci[] = {
            {0x09, 0x8de, 0xc1c}, {0x0b, 0x866, 0xb54}, {0x0d, 0x834, 0xa14},
            {0x0f, 0x898, 0xa00}, {0x11, 0x7d0, 0x9c4}, {0x12, 0x7d0, 0x8ca},
            {0x13, 0x5dc, 0x898}, {0x15, 0x5dc, 0x7d0}, {0x17, 0x7d0, 0x8ca},
            {0x18, 0x866, 0xb54}, {0x19, 0x6a4, 0x802}, {0x1f, 0x866, 0xa41},
            {0x21, 0x7e4, 0x92e}, {0x23, 0x5dc, 0x7d0}, {0x25, 0x80c, 0x8d4},
            {0x27, 0x73a, 0x834}, {0x29, 0x7d0, 0x8fc}, {0x2b, 0x898, 0xaf0},
            {0x2d, 0x7f8, 0x9d8}, {0x31, 0x500, 0x7d0}, {0x33, 0x5dc, 0x7d0},
            {0x36, 0x47e, 0x776}, {0x37, 0x410, 0x8ca}, {0x56, 0x410, 0x8ca},
        };
        for (size_t k = 0; k < sizeof loci / sizeof loci[0]; k++)
            if (loci[k].code == CODE3) {
                T(F2, 0x24) = loci[k].f2;
                T(F3, 0x24) = loci[k].f3;
                break;
            }
    }

    /* 0x1c202bc7: rate columns. */
    FB6(T(trk, 0x2c) = 0xa; T(trk, 0x30) = 0xa);
    if (r_edi && (ATTR3 & 0x1000000u)) {
        T(B1, 0x30) = 7; T(F1, 0x30) = 7;
        T(B2, 0x30) = 9; T(F2, 0x30) = 9; T(B3, 0x30) = 9; T(F3, 0x30) = 9;
    }
    if (CODE2 == 0x39) {
        int32_t v = (ATTR3 & 0x1000000u) ? 3 : 5;
        T(B2, 0x30) = v; T(F2, 0x30) = v; T(B3, 0x30) = v; T(F3, 0x30) = v;
    }
    if (CODE2 == 0x36)
        FB6(T(trk, 0x30) = 6; T(trk, 0x2c) = 6);
    r_eax = ATTR1 & 0x2000;
    if (r_eax && CODE1 != 0x36) {
        if (!r_esi)
            goto after_glide;
        FB6(T(trk, 0x2c) = 7);
    }
    if (r_esi && F10_2 && F10_0 && r_eax && (ATTR0 & 0x40000u))
        FB6(T(trk, 0x2c) = 5);
after_glide:
    if ((ATTR1 & 0x1000000u) && r_edi) {
        T(B1, 0x2c) = 7; T(F1, 0x2c) = 7;
    }
    if (CLS1 == 3 && r_edi) {
        T(B1, 0x2c) = 5; T(F1, 0x2c) = 5;
        T(B2, 0x2c) = 7; T(F2, 0x2c) = 7; T(B3, 0x2c) = 7; T(F3, 0x2c) = 7;
    } else if (CLS1 != 3 && r_edi && CLS3 == 3) {
        T(B1, 0x30) = 5; T(F1, 0x30) = 5;
        T(B2, 0x30) = 7; T(F2, 0x30) = 7; T(B3, 0x30) = 7; T(F3, 0x30) = 7;
    } else if (CLS2 == 3) {
        T(B2, 0x30) = 3; T(B1, 0x30) = 2; T(B1, 0x2c) = 2; T(F1, 0x30) = 2;
        T(F1, 0x2c) = 2; T(B2, 0x2c) = 3; T(F2, 0x30) = 3; T(F2, 0x2c) = 3;
        T(B3, 0x30) = 3; T(B3, 0x2c) = 3; T(F3, 0x30) = 3; T(F3, 0x2c) = 3;
    }
    if (CODE3 == 0x3f) {
        T(F1, 0x28) = 0x4b;
        T(B1, 0x30) = 6; T(F1, 0x30) = 6;
    }
    if (CODE2 == 0x3f) {
        T(F1, 0x28) = 0x19;
        T(F1, 0x30) = 1; T(F1, 0x2c) = 1;
    }
    if (CODE1 == 0x3f) {
        T(B1, 0x2c) = 4; T(F1, 0x2c) = 4;
    }
    if (CODE3 == 0x1d)
        FB6(T(trk, 0x30) = 0xe);
    if (CODE2 == 0x37)
        FB6(T(trk, 0x30) = 3);
    if (r_ecx) {
        if (ATTR1 & 0x1000040u) {
            T(B3, 0x2c) = 5; T(B2, 0x2c) = 5; T(B1, 0x2c) = 5;
        }
        if (ATTR3 & 0x1000040u) {
            T(B3, 0x30) = 5; T(B2, 0x30) = 5; T(B1, 0x30) = 5;
        }
    }
}

/* Spanish only: set when the current phoneme is the glide 0x36 or 0x6c
 * (TISPAN32 0x1c412b28, a global English does not have; placed in an unused
 * gap of the English layout). The frame loop wobbles its formants. */
#define GLIDE L(0x1c250ee4)
#define IS_GLIDE(c) ((c) == 0x36 || (c) == 0x6c)

/* TISPAN32 0x1c4019c0: Spanish formant targets. The skeleton is English's
 * (same target loading, pause holds, spacing, nasal bandwidths and
 * transition table); the coarticulation rules around it are Spanish's own. */
static void formant_targets_span(sv_engine *e, const sv_record *cur)
{
    const uint8_t *ph = cur->phonemes;
    if ((ATTR2 & 0x10002000u) && !(ATTR2 & 0x10)) {
        FB6(T(trk, 0x38) = 0);
        FB6(T(trk, 0x3c) = 0x64);
    }
    track_shift(e, F1);
    track_shift(e, B1);
    track_shift(e, F2);
    track_shift(e, B2);
    track_shift(e, F3);
    track_shift(e, B3);

    unsigned code = (CODE3 == 0x4b || CODE3 == 0x4c) ? CODE4 : CODE3;
    const uint8_t *def = ph + code * SV_PH_STRIDE;
#define DEF(off) ((int32_t)(int16_t)sv_rd16(def + (off)))
    T(F1, 0x18) = DEF(0x06);
    T(B1, 0x18) = DEF(0x0c);
    T(F2, 0x18) = DEF(0x08);
    T(B2, 0x18) = DEF(0x0e);
    T(F3, 0x18) = DEF(0x0a);
    T(B3, 0x18) = DEF(0x10);
    if (ATTR3 & 0x10) {
        T(F1, 0x1c) = DEF(0x20);
        T(B1, 0x1c) = DEF(0x26);
        T(F2, 0x1c) = DEF(0x22);
        T(B2, 0x1c) = DEF(0x28);
        T(F3, 0x1c) = DEF(0x24);
        T(B3, 0x1c) = DEF(0x2a);
    } else {
        FB6(T(trk, 0x1c) = T(trk, 0x18));
    }
    if (CODE3 == 0x4b || CODE3 == 0x4c) {
        int32_t v = DEF(0x0c) + 0xc8;
        T(B1, 0x1c) = v;
        T(B1, 0x18) = v;
        v = DEF(0x0e) + 0x28;
        if (v <= 0xc8)
            v = 0xc8;
        T(B2, 0x1c) = v;
        T(B2, 0x18) = v;
        v = DEF(0x10) + 0x3c;
        if (v <= 0x12c)
            v = 0x12c;
        T(B3, 0x1c) = v;
        T(B3, 0x18) = v;
        if (!(ATTR2 & 0x20000000u)) {
            T(F1, 0x18) = (T(F1, 0x1c) + T(F1, 0x0c)) >> 1;
            T(F2, 0x18) = (T(F2, 0x0c) + T(F2, 0x1c)) >> 1;
            T(F3, 0x18) = (T(F3, 0x1c) + T(F3, 0x0c)) >> 1;
        }
    }
#undef DEF
    if (ATTR3 & 0x20000000u)
        FB6(T(trk, 0x18) = T(trk, 0x0c));
    if (ATTR2 & 0x20000000u) {
        T(F1, 0x0c) = T(F1, 0x18);
        T(B1, 0x0c) = T(B1, 0x1c);
        T(F2, 0x0c) = T(F2, 0x18);
        T(B2, 0x0c) = T(B2, 0x1c);
        T(F3, 0x0c) = T(F3, 0x18);
        T(B3, 0x0c) = T(B3, 0x1c);
    }

    if (ATTR3 & 0x10000000u) {
        if ((ATTR3 & 0x80) && (ATTR3 & 0x1000) && (ATTR4 & 0x4000000u))
            T(F2, 0x1c) = T(F2, 0x18);
        if ((ATTR3 & 0x40000080u) && (ATTR4 & 0x800))
            T(F2, 0x1c) -= 0x12c;
        if ((ATTR3 & 0x40000000u) && (ATTR4 & 2))
            T(F2, 0x1c) -= 0x96;
        if (IS_GLIDE(CODE2)) {
            /* 0x1c401d2c: F3 toward the glide's own (phoneme 0x36). */
            if (ATTR2 & 0x200000u)
                T(F3, 0x18) = ph16(ph, 0x36, 0xa);
            else
                T(F3, 0x18) = (ph16(ph, 0x36, 0xa) + T(F3, 0x18)) >> 1;
        }
        if (CODE3 == 0x11 && F10_3 == 0) {
            T(F1, 0x18) = 0x294; T(F1, 0x1c) = 0x294;
            T(F2, 0x18) = 0x514; T(F2, 0x1c) = 0x514;
            T(F3, 0x18) = 0x960; T(F3, 0x1c) = 0x960;
        }
    } else if ((ATTR3 & 0x40) && (ATTR3 & 0x10000) && (ATTR4 & 0x10000000u) &&
               (ATTR4 & 0x200000u)) {
        T(F2, 0x18) -= 0x32;
        T(F3, 0x18) -= 0xc8;
        T(F2, 0x1c) = T(F2, 0x18);
        T(F3, 0x1c) = T(F3, 0x18);
    }

    if ((ATTR3 & 2) && (ATTR2 & 0x180000u)) {
        T(F2, 0x1c) = 0x73a; T(F2, 0x18) = 0x73a;
        T(F3, 0x1c) = 0x898; T(F3, 0x18) = 0x898;
    }
    uint32_t r_edx = ATTR3 & 0x4000000u;
    if (r_edx && (ATTR2 & 0x180000u)) {
        T(F2, 0x1c) = 0x6a4; T(F2, 0x18) = 0x6a4;
        T(F3, 0x1c) = 0x76c; T(F3, 0x18) = 0x76c;
    }
    if (r_edx && (ATTR2 & 0x80000000u)) {
        T(F2, 0x1c) = 0x384; T(F2, 0x18) = 0x384;
        T(F3, 0x1c) = 0x9c4; T(F3, 0x18) = 0x9c4;
    }
    if (CODE3 == 0x37) {
        if ((ATTR4 & 0x800000u) && !(ATTR4 & 0x8000u)) {
            int32_t v = (ph16(ph, CODE4, 8) + T(F2, 0x18) * 9) / 10;
            T(F2, 0x1c) = v;
            T(F2, 0x18) = v;
        }
        if (!(ATTR4 & 0x800000u) || (ATTR4 & 0x8000u)) {
            int32_t v = (T(F2, 0x18) * 9 + 0x91) / 10;
            T(F2, 0x1c) = v;
            T(F2, 0x18) = v;
        }
        if (ATTR4 & 0x200000u) {
            T(F1, 0x1c) -= 0x64;
            T(F2, 0x1c) -= 0x64;
        }
    }
    if (IS_GLIDE(CODE3)) {
        if (!(ATTR2 & 0x200000u))
            T(F3, 0x18) = T(F3, 0x08);
        if (!(ATTR4 & 0x200000u))
            T(F3, 0x1c) = ph16(ph, CODE4, 0xa);
    }
    if (CODE3 == 0x40) {
        int32_t v = (ph16(ph, CODE4, 6) + T(F1, 0x0c)) >> 1;
        T(F1, 0x1c) = v;
        T(F1, 0x18) = v;
        v = (ph16(ph, CODE4, 8) + T(F2, 0x0c)) >> 1;
        T(F2, 0x1c) = v;
        T(F2, 0x18) = v;
        v = (ph16(ph, CODE4, 0xa) + T(F3, 0x0c)) >> 1;
        T(F3, 0x1c) = v;
        T(F3, 0x18) = v;
    }
    if (CODE3 == 0x3e && (ATTR2 & 0x40000080u)) {
        T(F2, 0x1c) = T(F2, 0x18);
        T(F3, 0x1c) = T(F3, 0x18);
        T(F1, 0x18) -= 0x64;
        T(F1, 0x1c) = T(F1, 0x18);
    }
    if (T(F3, 0x18) - T(F2, 0x18) < 0xfa)
        T(F2, 0x18) = T(F3, 0x18) - 0xfa;
    if (T(F3, 0x1c) - T(F2, 0x1c) < 0xfa)
        T(F2, 0x1c) = T(F3, 0x1c) - 0xfa;
    if (T(F2, 0x18) - T(F1, 0x18) < 0xc8)
        T(F1, 0x18) = T(F2, 0x18) - 0xc8;
    if (T(F2, 0x1c) - T(F1, 0x1c) < 0xc8)
        T(F1, 0x1c) = T(F2, 0x1c) - 0xc8;

    nasal_bandwidths(e);

    uint32_t r_esp10 = ATTR3 & 0x10002000u;
    if (r_esp10) {
        if (ATTR4 & 0x8000u) {
            T(B1, 0x1c) += 0x64;
            T(B2, 0x1c) += 0x32;
            T(B3, 0x1c) += 0x32;
            T(F1, 0x1c) = (T(F1, 0x1c) + 0x1f4) >> 1;
        }
        if (ATTR2 & 0x8000u) {
            T(B1, 0x18) += 0x64;
            T(B2, 0x18) += 0x32;
            T(B3, 0x18) += 0x32;
            T(F1, 0x18) = (T(F1, 0x18) + 0x1f4) >> 1;
        }
    }

    W(0x1c250562) = W(0x1c25055a);
    uint32_t r_ebx = ATTR3 & 0x10000000u;
    if (r_ebx)
        W(0x1c25055a) = 0;
    else if (ATTR3 & 0x1000000u)
        W(0x1c25055a) = 1;
    else if (ATTR3 & 0x40)
        W(0x1c25055a) = 2;
    else
        W(0x1c25055a) = 3;
    {
        int32_t v = B(0x1c24c3f0 + W(0x1c25055a) + W(0x1c250562) * 4);
        FB6(T(trk, 0x28) = v);
    }
    if (CODE2 == 0x37)
        FB6(T(trk, 0x28) = 0x14);
    uint32_t r_edi = ATTR2 & 0x10000000u;
    if (r_edi && (ATTR3 & 0x800))
        FB6(T(trk, 0x28) = 0x14);
    if (r_edi && (ATTR3 & 0x1000000u)) {
        T(B1, 0x28) = 0x41; T(F1, 0x28) = 0x41;
        T(B2, 0x28) = 0x32; T(F2, 0x28) = 0x32; T(B3, 0x28) = 0x32; T(F3, 0x28) = 0x32;
    }
    if (r_edi && IS_GLIDE(CODE3)) {
        T(F1, 0x28) = (ATTR2 & 0x80) ? 0x64 : 0x1e;
        T(F2, 0x28) = 0x50;
        T(F3, 0x28) = 0x32;
        T(B3, 0x28) = 0x14; T(B2, 0x28) = 0x14; T(B1, 0x28) = 0x14;
    }
    if (IS_GLIDE(CODE2)) {
        T(F1, 0x28) = (ATTR1 & 0x80) ? 0 : 0x46;
        T(F3, 0x28) = 0x32;
        T(F2, 0x28) = 0x14;
        T(B3, 0x28) = 0x14; T(B2, 0x28) = 0x14; T(B1, 0x28) = 0x14;
    }
    uint32_t r_ecx = ATTR2 & 0x800000u;
    if (r_ecx && (ATTR3 & 0x40)) {
        T(B1, 0x28) = 0x5a; T(F1, 0x28) = 0x5a;
    } else if ((ATTR2 & 0x40) && (ATTR3 & 0x800000u)) {
        T(B1, 0x28) = 0xa; T(F1, 0x28) = 0xa;
    }
    if (ATTR3 & 0x20000000u) {
        T(F3, 0x28) = 0; T(F2, 0x28) = 0; T(F1, 0x28) = 0;
        T(B3, 0x28) = 0x32; T(B2, 0x28) = 0x32; T(B1, 0x28) = 0x32;
    }
    if (ATTR2 & 0x20000000u) {
        T(F3, 0x28) = 0x64; T(F2, 0x28) = 0x64; T(F1, 0x28) = 0x64;
    }
    if (r_edi && (ATTR3 & 0x4000000u))
        T(F2, 0x28) = 0x50;
    else if ((ATTR2 & 0x4000000u) && r_ebx)
        T(F2, 0x28) = 0x1e;
    if (r_ecx && CODE3 == 0x1e)
        FB6(T(trk, 0x28) = 0x5a);
    uint32_t r_esi = ATTR2 & 0x80000u;
    {
        int32_t v = -1;
        if (r_esi && (ATTR3 & 0x8000u))
            v = 0xa;
        else if ((ATTR2 & 0x8000u) && (ATTR3 & 0x80000u))
            v = 0x5a;
        if (v >= 0) {
            T(F3, 0x28) = v; T(F2, 0x28) = v; T(B3, 0x28) = v; T(B2, 0x28) = v;
        }
    }
    if ((ATTR2 & 0xc0100000u) && (ATTR3 & 0x2000))
        FB6(T(trk, 0x28) = 0x14);

    FB6(T(trk, 0x24) = T(trk, 0x0c) + (T(trk, 0x18) - T(trk, 0x0c)) * T(trk, 0x28) / 100);

    if ((ATTR1 & 2) && r_ecx) {
        T(F2, 0x20) = 0x640;
        T(F3, 0x20) = r_esi ? 0x8fc : 0xa28;
    }
    if ((ATTR2 & 2) && (ATTR3 & 0x800000u))
        T(F3, 0x24) = (ATTR3 & 0x80000u) ? 0x8fc : 0xa3c;
    r_esi = ATTR2 & 0x10002000u;
    if (r_esi && (ATTR3 & 0x4000000u)) {
        T(B1, 0x24) += 0x64;
        T(F2, 0x24) = (T(F3, 0x0c) + T(F2, 0x18) + T(F2, 0x0c)) / 3 - 0x190;
    }
    uint32_t r_eax = ATTR3 & 0x4000000u;
    if (r_eax && (ATTR2 & 0x40000080u))
        T(F2, 0x24) += 0x64;
    r_edx = ATTR2 & 0x4000000u;
    if (r_edx && (ATTR3 & 0x40000080u))
        T(F2, 0x24) += 0x64;
    if (r_edx)
        T(F3, 0x24) = T(F2, 0x24) + ((ATTR3 & 0x400) ? 0x320 : 0x190);
    if (r_eax)
        T(F3, 0x24) = T(F2, 0x24) + ((ATTR2 & 0x400) ? 0x320 : 0x258);
    if ((ATTR2 & 0x800) && r_ebx) {
        T(F1, 0x24) -= 0x3c;
        T(F2, 0x24) -= 0x32;
    } else if (r_edi && (ATTR1 & 0x800)) {
        T(F1, 0x20) += 0x64;
        T(F2, 0x20) += 0x64;
    }
    if ((ATTR2 & 0x20000100u) && CODE2 != 0x40) {
        T(F1, 0x20) = T(F1, 0x08);
        T(F2, 0x20) = T(F2, 0x08);
        T(F3, 0x20) = T(F3, 0x08);
    }
    if (r_edx && r_esp10) {
        /* 0x1c4026e4: Spanish stop loci by vowel code. */
        static const struct { uint8_t code; int16_t f2, f3; } loci[] = {
            {0x09, 0x8de, 0xc1c}, {0x0d, 0x834, 0xa14}, {0x11, 0x76c, 0x97e},
            {0x1f, 0x866, 0xa41}, {0x21, 0x7e4, 0x92e}, {0x23, 0x4b0, 0x960},
            {0x27, 0x3e8, 0x9f6}, {0x29, 0x5dc, 0x76c}, {0x36, 0x78a, 0x9a6},
            {0x37, 0x410, 0x8ca}, {0x38, 0x5dc, 0x76c}, {0x5c, 0x3e8, 0x9f6},
            {0x6c, 0x78a, 0x9a6},
        };
        for (size_t k = 0; k < sizeof loci / sizeof loci[0]; k++)
            if (loci[k].code == CODE3) {
                T(F2, 0x24) = loci[k].f2;
                T(F3, 0x24) = loci[k].f3;
                break;
            }
    }

    /* 0x1c4027ec: rate columns. */
    FB6(T(trk, 0x2c) = 0xa; T(trk, 0x30) = 0xc);
    if (ATTR2 & 0x1000000u)
        FB6(T(trk, 0x30) = 5);
    if (r_ecx && (ATTR3 & 0x1000000u)) {
        T(B1, 0x30) = 6; T(F1, 0x30) = 6;
        T(B2, 0x30) = 8; T(F2, 0x30) = 8; T(B3, 0x30) = 8; T(F3, 0x30) = 8;
    }
    if (CODE2 == 0x39) {
        int32_t v = (ATTR3 & 0x1000000u) ? 3 : 5;
        T(B2, 0x30) = v; T(F2, 0x30) = v; T(B3, 0x30) = v; T(F3, 0x30) = v;
    }
    if (IS_GLIDE(CODE2)) {
        T(F1, 0x30) = 4; T(F1, 0x2c) = 4; T(F2, 0x30) = 4; T(F2, 0x2c) = 4;
        T(F3, 0x30) = 5; T(F3, 0x2c) = 5;
        T(B1, 0x30) = 2; T(B1, 0x2c) = 2; T(B2, 0x30) = 2; T(B2, 0x2c) = 2;
        T(B3, 0x30) = 2; T(B3, 0x2c) = 2;
    }
    r_eax = ATTR1 & 0x2000;
    if (r_eax && CODE1 != 0x36) {
        if (!r_edi)
            goto after_glide;
        FB6(T(trk, 0x2c) = 7);
    }
    if (r_edi && F10_2 && F10_0 && r_eax && (ATTR0 & 0x40000u))
        FB6(T(trk, 0x2c) = 5);
after_glide:
    if (CLS1 == 3) {
        if (r_ecx) {
            /* 0x1c4029b8: F3 +0x2c is written at the shared tail below. */
            T(B1, 0x2c) = 5; T(F1, 0x2c) = 5;
            T(B2, 0x2c) = 7; T(F2, 0x2c) = 7; T(B3, 0x2c) = 7; T(F3, 0x2c) = 7;
            goto after_class;
        }
    } else if (r_ecx && CLS3 == 3) {
        T(B1, 0x30) = 5; T(F1, 0x30) = 5;
        T(B2, 0x30) = 7; T(F2, 0x30) = 7; T(B3, 0x30) = 7; T(F3, 0x30) = 7;
        goto after_class;
    }
    if (CLS2 == 3) {
        T(B2, 0x30) = 3; T(B1, 0x30) = 2; T(B1, 0x2c) = 2; T(F1, 0x30) = 2;
        T(F1, 0x2c) = 2; T(B2, 0x2c) = 3; T(F2, 0x30) = 3; T(F2, 0x2c) = 3;
        T(B3, 0x30) = 3; T(B3, 0x2c) = 3; T(F3, 0x30) = 3; T(F3, 0x2c) = 3;
    }
after_class:
    if (CODE2 == 0x37)
        FB6(T(trk, 0x30) = 3);
    if (r_edi && (ATTR3 & 0x800))
        FB6(T(trk, 0x30) = 5);
    if (r_esi) {
        if (ATTR1 & 0x1000040u) {
            T(B3, 0x2c) = 5; T(B2, 0x2c) = 5; T(B1, 0x2c) = 5;
        }
        if (ATTR3 & 0x1000040u) {
            T(B3, 0x30) = 5; T(B2, 0x30) = 5; T(B1, 0x30) = 5;
        }
    }
    GLIDE = IS_GLIDE(CODE2);
    if (IS_GLIDE(CODE3)) {
        T(B2, 0x24) = 0xfa;
        T(B2, 0x30) = 2;
    }
    if (IS_GLIDE(CODE1)) {
        T(B2, 0x20) = 0xfa;
        T(B2, 0x2c) = 2;
    }
}

/* ------------------------------------------------------------------------ */
/* 0x1c2030b0 — voicing (AV), aspiration (AH), DI and TL targets             */
/*                                                                           */
/* Transition percentages and rate columns come from a 10x10 table of        */
/* 10-byte entries at 0x1c24c460, indexed by manner class pairs:             */
/* (current, next) for +0x28/+0x30 and (previous, current) for +0x2c.        */
/* Byte 0/1/2 of an entry is AV/AH/AF +0x28, 3/4/5 their +0x2c, 6/7/8 +0x30. */
/* ------------------------------------------------------------------------ */
static void voicing_targets(sv_engine *e)
{
    track_shift(e, AV);
    track_shift(e, AH);
    uint32_t c23 = ((uint32_t)CLS2 * 10 + CLS3) * 10;
    uint32_t c12 = ((uint32_t)CLS1 * 10 + CLS2) * 10;
    T(AV, 0x28) = B(0x1c24c460 + c23);
    T(AV, 0x2c) = B(0x1c24c463 + c12);
    T(AV, 0x30) = B(0x1c24c466 + c23);
    if (CODE1 == 0x3f)
        T(AV, 0x2c) = 1;
    if (CODE2 == 0x3f) {
        T(AV, 0x30) = 1;
        T(AV, 0x2c) = 1;
    }
    if (CODE3 == 0x3f)
        T(AV, 0x30) = 1;
    T(AV, 0x38) = 0;
    T(AV, 0x3c) = 0x4b;
    T(AH, 0x28) = B(0x1c24c461 + c23);
    T(AH, 0x2c) = B(0x1c24c464 + c12);
    T(AH, 0x30) = B(0x1c24c467 + c23);

    /* 0x1c2031b2: the next phoneme's voicing level, definition +0x16, as
     * an index into the level table at 0x1c2065be. */
    unsigned c3 = CODE3;
    T(AV, 0x18) = e->window[3]->phonemes[c3 * SV_PH_STRIDE + 0x16];
    if ((c3 == 0x1c || c3 == 0x1b) && !(ATTR2 & 0x8000000u))
        T(AV, 0x18) = 0;
    uint32_t r_edi = ATTR2 & 0x10000000u;
    if (r_edi && (ATTR3 & 0x8000000u) && (ATTR3 & 0x40) && (ATTR4 & 0x10000000u))
        T(AV, 0x18) += 6;
    T(AV, 0x1c) = T(AV, 0x18);
    if ((ATTR3 & 0x8000000u) && !(ATTR4 & 0x8000000u))
        T(AV, 0x1c) = T(AV, 0x18) - 3;
    T(AH, 0x18) = 0;
    T(AV, 0x18) = TB(0x1c2065be + (uint32_t)T(AV, 0x18));
    T(AV, 0x1c) = TB(0x1c2065be + (uint32_t)T(AV, 0x1c));
    if (ATTR3 & 0x44) {
        switch (c3) {
        case 0x35: T(AH, 0x18) = 0x2f; break;
        case 0x41: T(AH, 0x18) = 0x21; break;
        case 0x42: T(AH, 0x18) = 0x20; break;
        case 0x43: T(AH, 0x18) = 0x24; break;
        case 0x44: T(AH, 0x18) = 0x21; break;
        case 0x4b:
        case 0x4c: T(AH, 0x18) = 0x2b; break;
        default: break;
        }
    }
    T(AH, 0x18) = TB(0x1c2065c8 + (uint32_t)T(AH, 0x18));
    T(AH, 0x1c) = T(AH, 0x18);

    /* 0x1c2032fb: aspiration after a released stop or fricative. */
    if (!(ATTR1 & 0x8000000u) && (ATTR1 & 0x40040u)) {
        uint32_t r_ebx = ATTR2 & 0x10002000u;
        if (r_ebx || CODE2 == 0x1c) {
            W(0x1c250e6e) = 5;
            W(0x1c250e52) = 0x2e;
            if (F10_1 == 0) {
                W(0x1c250e6e) = 2;
                W(0x1c250e52) = 0x28;
            }
            if (!r_edi)
                W(0x1c250e6e) = ((ATTR2 & 0x800) || (ATTR1 & 0x400)) ? 6 : 7;
            if ((ATTR0 & 0x8000000u) && F10_2 == 0)
                W(0x1c250e6e)--;
            if (ATTR1 & 1)
                W(0x1c250e6e) -= 3;
            if (CODE2 == 0x1c || CODE2 == 0x1b) {
                W(0x1c250e6e) = 8;
                W(0x1c250e52) -= 3;
            }
            uint32_t r_esi = ATTR1 & 0x40000u;
            if (r_esi && CODE0 == 0x41 && !(FLAGS1 & 0x80))
                W(0x1c250e6e) = 0;
            else if (ATTR1 & 0x40)
                W(0x1c250e6e) = r_edi ? 1 : (int16_t)(NFR2 >> 1);
            int16_t cx = (int16_t)NFR2, ax = cx;
            if (ax >= W(0x1c250e6e))
                ax = W(0x1c250e6e);
            W(0x1c250e6e) = ax;
            if ((ATTR2 & 0x80000u) && (ATTR1 & 2)) {
                if (CODE2 == 0x36)
                    W(0x1c250e52) += 6;
                else
                    W(0x1c250e52)++;
            }
            if ((e->st_52 == 0 || e->st_52 == 3) && !(ATTR1 & 0x400) && r_ebx) {
                W(0x1c250e6e) += 2;
                if (F10_2 != 0 || (FLAGS2 & 4))
                    W(0x1c250e6e) += 2;
            }
            if (e->st_4c == 3)
                W(0x1c250e6e) = (int16_t)(W(0x1c250e6e) >> 1);
            if (W(0x1c250e6e) > 0 && NFR2 != 0) { /* NFR2 == 0 faults on div */
                T(AH, 0x08) = TB(0x1c2065c8 + (int32_t)W(0x1c250e52));
                if (ATTR1 & 0x40)
                    T(AH, 0x08) = T(AH, 0x14);
                T(AH, 0x2c) = 2;
                T(AH, 0x0c) = 0;
                T(AH, 0x38) = 0;
                uint32_t q = (uint32_t)((int32_t)W(0x1c250e6e) * 100 + 0x32) / (uint32_t)NFR2;
                if (q >= 0x64)
                    q = 0x64;
                T(B2, 0x08) += 0x64;
                T(AH, 0x3c) = (int32_t)q;
                if (r_esi) {
                    if (ATTR1 & 0x4000000u)
                        T(B3, 0x20) += 0x96;
                    T(AH, 0x2c) = 0;
                    T(AV, 0x08) = 0;
                    T(B2, 0x2c) = 0;
                    T(B1, 0x2c) = 0;
                    T(AV, 0x2c) = 0;
                    int32_t c = T(AH, 0x3c) + (-400 / (int32_t)cx);
                    if (c <= 0)
                        c = 0;
                    T(B2, 0x38) = c; T(B1, 0x38) = c; T(AV, 0x38) = c;
                    T(B2, 0x3c) = T(AH, 0x3c); T(B1, 0x3c) = T(AH, 0x3c);
                    T(AV, 0x3c) = T(AH, 0x3c);
                }
            }
        }
    }

    T(AV, 0x24) = T(AV, 0x0c) + (T(AV, 0x18) - T(AV, 0x0c)) * T(AV, 0x28) / 100;
    T(AH, 0x24) = T(AH, 0x0c) + (T(AH, 0x18) - T(AH, 0x0c)) * T(AH, 0x28) / 100;
    if (T(AV, 0x18) <= 0)
        T(AV, 0x18) = 0;
    if (T(AV, 0x1c) <= 0)
        T(AV, 0x1c) = 0;
    if (T(AV, 0x24) <= 0)
        T(AV, 0x24) = 0;

    /* 0x1c203627: DI and TL. TL is spectral tilt in Q8. */
    track_shift(e, DI);
    track_shift(e, TL);
    T(TL, 0x18) = (ATTR3 & 0x8000000u) ? 0xc : 8;
    if (F10_3)
        T(TL, 0x18) += 2;
    if ((FLAGS3 & 4) && !F10_3)
        T(TL, 0x18) -= 2;
    if (ATTR3 & 0x1000000u)
        T(TL, 0x18) -= 5;
    if (CODE3 == 1)
        T(B1, 0x0c) += 0x32;
    int32_t s = T(TL, 0x18) << 8;
    T(TL, 0x30) = 3;
    T(TL, 0x2c) = 3;
    T(TL, 0x18) = s;
    T(TL, 0x1c) = s;
    T(DI, 0x24) = T(DI, 0x0c) + (T(DI, 0x18) - T(DI, 0x0c)) * T(DI, 0x28) / 100;
    T(TL, 0x28) = 0x32;
    T(TL, 0x24) = T(TL, 0x0c) + (s - T(TL, 0x0c)) * 50 / 100;
}

/* TISPAN32 0x1c402d00: Spanish voicing targets. The table layout, AV/AH
 * interpolation and DI/TL tail are English's; the vowel-glide codes 0x36 and
 * 0x6c replace English's 0x3f special case, the aspiration switch has its
 * own codes, and there is no post-release aspiration block. */
static void voicing_targets_span(sv_engine *e)
{
    track_shift(e, AV);
    track_shift(e, AH);
    uint32_t c23 = ((uint32_t)CLS2 * 10 + CLS3) * 10;
    uint32_t c12 = ((uint32_t)CLS1 * 10 + CLS2) * 10;
    T(AV, 0x28) = B(0x1c24c460 + c23);
    T(AV, 0x38) = 0;
    T(AV, 0x3c) = 0x4b;
    T(AV, 0x2c) = B(0x1c24c463 + c12);
    T(AV, 0x30) = B(0x1c24c466 + c23);
    T(AH, 0x28) = B(0x1c24c461 + c23);
    T(AH, 0x2c) = B(0x1c24c464 + c12);
    T(AH, 0x30) = B(0x1c24c467 + c23);
    if (CODE2 == 0x36 || CODE2 == 0x6c) {
        T(AV, 0x30) = 1;
        T(AV, 0x2c) = 1;
    }
    if (ATTR2 & 0x800000u) {
        if (CODE3 == 0x36 || CODE3 == 0x6c)
            T(AV, 0x28) = 0;
        if (CODE1 == 0x36 || CODE1 == 0x6c)
            T(AV, 0x20) = T(AV, 0x08);
    }

    unsigned c3 = CODE3;
    T(AV, 0x18) = e->window[3]->phonemes[c3 * SV_PH_STRIDE + 0x16];
    if ((ATTR3 & 0x20000000u) && !(ATTR2 & 0x8000000u))
        T(AV, 0x18) = 0;
    if ((ATTR2 & 0x10000000u) && (ATTR3 & 0x8000000u) && (ATTR3 & 0x40) &&
        (ATTR4 & 0x10000000u))
        T(AV, 0x18) += 6;
    T(AV, 0x1c) = T(AV, 0x18);
    if ((ATTR3 & 0x8000000u) && !(ATTR4 & 0x8000000u))
        T(AV, 0x1c) = T(AV, 0x18) - 3;
    T(AH, 0x18) = 0;
    T(AV, 0x18) = TB(0x1c2065be + (uint32_t)T(AV, 0x18));
    T(AV, 0x1c) = TB(0x1c2065be + (uint32_t)T(AV, 0x1c));
    if (ATTR3 & 0x44) {
        switch (c3) {
        case 0x41: case 0x42: T(AH, 0x18) = 0x20; break;
        case 0x43: T(AH, 0x18) = 0x24; break;
        case 0x44: T(AH, 0x18) = 0x21; break;
        case 0x4b: T(AH, 0x18) = 0x28; break;
        default: break;
        }
    }
    T(AH, 0x18) = TB(0x1c2065c8 + (uint32_t)T(AH, 0x18));
    T(AH, 0x1c) = T(AH, 0x18);

    T(AV, 0x24) = T(AV, 0x0c) + (T(AV, 0x18) - T(AV, 0x0c)) * T(AV, 0x28) / 100;
    T(AH, 0x24) = T(AH, 0x0c) + (T(AH, 0x18) - T(AH, 0x0c)) * T(AH, 0x28) / 100;
    if (T(AV, 0x18) <= 0)
        T(AV, 0x18) = 0;
    if (T(AV, 0x1c) <= 0)
        T(AV, 0x1c) = 0;
    if (T(AV, 0x24) <= 0)
        T(AV, 0x24) = 0;

    track_shift(e, DI);
    track_shift(e, TL);
    T(TL, 0x18) = (ATTR3 & 0x8000000u) ? 0xc : 8;
    if (F10_3)
        T(TL, 0x18) += 2;
    if ((FLAGS3 & 4) && !F10_3)
        T(TL, 0x18) -= 2;
    if (ATTR3 & 0x1000000u)
        T(TL, 0x18) -= 5;
    if (CODE3 == 1)
        T(B1, 0x0c) += 0x32;
    int32_t s = T(TL, 0x18) << 8;
    T(TL, 0x30) = 3;
    T(TL, 0x2c) = 3;
    T(TL, 0x18) = s;
    T(TL, 0x1c) = s;
    T(DI, 0x24) = T(DI, 0x0c) + (T(DI, 0x18) - T(DI, 0x0c)) * T(DI, 0x28) / 100;
    T(TL, 0x28) = 0x32;
    T(TL, 0x24) = T(TL, 0x0c) + (s - T(TL, 0x0c)) * 50 / 100;
}

/* ------------------------------------------------------------------------ */
/* 0x1c249310 — frication: AF, AK, K1, Q1, and the burst                    */
/* ------------------------------------------------------------------------ */
static void frication_targets(sv_engine *e, const sv_record *cur)
{
    const uint8_t *ph = cur->phonemes;
    /* TISPAN32 0x1c40a9e0 is this function with the differences marked
     * `span`: 0x6c joins 0x36 as the glide, 0x3f and 0x15 lose their special
     * cases, and several constants and burst levels change. */
    const int span = e->lang->id == SV_LANG_SPANISH;
    track_shift(e, K1);
    track_shift(e, Q1);
    track_shift(e, AF);
    track_shift(e, AK);
    T(AF, 0x1c) = 0;
    T(AF, 0x18) = 0;
    uint32_t r_ecx = ATTR3 & 0x40;
    if (r_ecx) {
        static const int32_t af[8] = {0x30, 0x3d, 0x37, 0x31, 0x2e, 0x3c, 0x38, 0x35};
        unsigned k = (unsigned)CODE3 - 0x41;
        T(AF, 0x18) = k <= 7 ? af[k] : 0;
        if (span && k == 0)
            T(AF, 0x18) = 0x2f;
        if ((ATTR2 & 0x10000) && !(ATTR3 & 0x80))
            T(AF, 0x18) += 3;
        if (F10_3 == 0) {
            T(AF, 0x18) -= 2;
            if (span ? (ATTR3 & 2) : (FLAGS3 & 4))
                T(AF, 0x18) -= 2;
        }
        T(AF, 0x1c) = T(AF, 0x18);
    }
    int32_t af18 = T(AF, 0x18);
    T(AF, 0x38) = 0;
    T(AF, 0x3c) = 0x4b;
    T(AK, 0x18) = -10;
    T(K1, 0x18) = 0xbb8;
    T(Q1, 0x18) = 0x190;
    T(AF, 0x18) = TB(0x1c2065be + (uint32_t)af18);
    T(AF, 0x1c) = TB(0x1c2065be + (uint32_t)T(AF, 0x1c));

    uint32_t r_ebx = ATTR3 & 0x40040u;
    if (r_ebx || (!span && CODE3 == 0x3f)) {
        int r_esi = (CODE4 == 0x29 || CODE4 == 0x38 || (!span && CODE4 == 0x15));
        uint32_t r_edx = ATTR3 & 2;
        int16_t bp, ax, di;
        if (r_edx) {
            if (span) {
                if ((ATTR3 & 0x40000u) && (CODE4 == 0x36 || CODE4 == 0x6c)) {
                    bp = 0xdac; ax = 0x1c2; di = -18;
                } else {
                    bp = 0xfa0; ax = 0x1f4; di = -18;
                }
            } else if ((ATTR3 & 0x40000u) && CODE4 == 0x36) {
                bp = 0xa28; ax = 0x190; di = -18;
            } else {
                bp = 0x1194; ax = 0x1f4; di = -18;
            }
        } else if (ATTR3 & 0x10000) {
            bp = r_esi ? 0x8fc : 0x9c4;
            ax = 0x12c;
            di = -18;
        } else if (ATTR3 & 0x4000000u) {
            if (ATTR4 & 0x800000u) {
                bp = (int16_t)((ph16(ph, CODE4, 8) + ph16(ph, CODE4, 0xa)) >> 1);
                ax = 0xfa;
            } else {
                bp = 0x834;
                ax = 0xdc;
            }
            di = -18;
        } else if (ATTR3 & 8) {
            bp = 0xe10; ax = 0x3e8; di = -6;
        } else if (ATTR3 & 0x400) {
            bp = 0xe10; ax = 0x3e8; di = -3;
        } else {
            /* 0x1c2495ab reads an uninitialised stack word. On every path
             * into this function it holds 2030b0's saved EBX, which is the
             * generator's loop constant 0x4b (0x1c204be1), so the original
             * deterministically uses 0x4b for all three. */
            bp = ax = di = span ? 0x19 : 0x4b;   /* TISPAN32's EBX there is 0x19 */
        }
        T(K1, 0x1c) = bp; T(K1, 0x18) = bp;
        T(Q1, 0x1c) = ax; T(Q1, 0x18) = ax;
        T(AK, 0x1c) = di; T(AK, 0x18) = di;
        if (r_edx) {
            if (r_esi)
                T(K1, 0x1c) -= 0x12c;
            if (ATTR2 & 0x200000u)
                T(K1, 0x18) -= span ? 0x12c : 0x96;
        }
        if (ATTR3 & 0x10000) {
            if (ATTR2 & 0x10002000u) {
                int32_t v = ((T(F3, 0x0c) + T(F2, 0x0c)) >> 1) + 0x64;
                T(K1, 0x18) = v;
                T(Q1, 0x18) = v / 6;
            }
            if (ATTR4 & 0x10002000u) {
                int32_t v = ((ph16(ph, CODE4, 8) + ph16(ph, CODE4, 0xa)) >> 1) + 0x64;
                T(K1, 0x1c) = v;
                T(Q1, 0x1c) = v / 6;
            }
        }
    }
    if (!r_ecx && (ATTR2 & 0x40)) {
        T(K1, 0x18) = T(K1, 0x0c);
        T(Q1, 0x18) = T(Q1, 0x0c);
        T(AK, 0x18) = T(AK, 0x0c);
    }
    uint32_t r_edi = ATTR2 & 0x40;
    if (!r_edi && !(ATTR2 & 0x40000u)) {
        if (ATTR1 & 0x40) {
            T(K1, 0x08) = T(K1, 0x14);
            T(Q1, 0x08) = T(Q1, 0x14);
            T(AK, 0x08) = T(AK, 0x14);
        }
        if (r_ecx) {
            T(K1, 0x0c) = T(K1, 0x18);
            T(Q1, 0x0c) = T(Q1, 0x18);
            T(AK, 0x0c) = T(AK, 0x18);
        }
    }
    uint32_t r_esi2 = ATTR2 & 0x4000000u;
    if (r_esi2 && (ATTR3 & 0x10002000u))
        T(K1, 0x0c) = T(F2, 0x24) + 0x82;

    uint32_t c23 = ((uint32_t)CLS2 * 10 + CLS3) * 10;
    uint32_t c12 = ((uint32_t)CLS1 * 10 + CLS2) * 10;
    T(AF, 0x28) = B(0x1c24c462 + c23);
    T(K1, 0x28) = 0x32; T(Q1, 0x28) = 0x32; T(AK, 0x28) = 0x32;
    T(AF, 0x2c) = B(0x1c24c465 + c12);
    T(AF, 0x30) = B(0x1c24c468 + c23);
    int32_t v = (ATTR1 & 0x40) ? 2 : 5;
    T(AK, 0x2c) = v; T(Q1, 0x2c) = v; T(K1, 0x2c) = v;
    v = r_ecx ? 2 : 5;
    T(AK, 0x30) = v; T(Q1, 0x30) = v; T(K1, 0x30) = v;
    uint32_t r_ecx2 = ATTR2 & 0x40000u;
    if (r_ecx2) {
        T(AK, 0x28) = 0; T(Q1, 0x28) = 0; T(K1, 0x28) = 0;
        T(AK, 0x30) = 0; T(Q1, 0x30) = 0; T(K1, 0x30) = 0;
    }
    if (r_edi && (ATTR1 & 0x40000u)) {
        T(AK, 0x2c) = 0; T(Q1, 0x2c) = 0; T(K1, 0x2c) = 0;
    }
    T(AF, 0x24) = T(AF, 0x0c) + (T(AF, 0x18) - T(AF, 0x0c)) * T(AF, 0x28) / 100;
    T(K1, 0x24) = T(K1, 0x0c) + (T(K1, 0x18) - T(K1, 0x0c)) * T(K1, 0x28) / 100;
    T(Q1, 0x24) = T(Q1, 0x0c) + (T(Q1, 0x18) - T(Q1, 0x0c)) * T(Q1, 0x28) / 100;
    T(AK, 0x24) = T(AK, 0x0c) + (T(AK, 0x18) - T(AK, 0x0c)) * T(AK, 0x28) / 100;

    /* 0x1c2498e8: the burst — how many frames (0x1c25058c) and at what AF
     * level (0x1c25055c) FUN_1c203a30 later holds. */
    W(0x1c25058c) = 0;
    W(0x1c25055c) = 0;
    uint32_t r_edx = ATTR2 & 1;
    if (!(r_edx || (!span && CODE2 == 0x3f))) {
        if (!r_ecx2)
            return;
        if ((ATTR3 & 0x20000000u) && !(ATTR3 & 0x8000))
            return;
    }
    int16_t k = 0;
    while (B(0x1c24c400 + 2 * k) != CODE2) {
        k++;
        if (k >= 0xf)
            break;
    }
    W(0x1c25058c) = B(0x1c24c401 + 2 * k);
    if (CODE1 != 0x41 && CODE2 == 0x52 && (CODE3 == 0x36 || (span && CODE3 == 0x6c)))
        W(0x1c25058c) += 3;
    if (r_edx && F10_2 && CODE3 != (span ? 0x6c : 0x36))
        W(0x1c25058c) += 2;
    if (CODE1 == 0x41 && (FLAGS1 & 0x80))
        W(0x1c25058c) = 2;
    {
        int16_t ax = W(0x1c25058c);
        if (ax >= (int16_t)NFR2)
            ax = (int16_t)NFR2;
        W(0x1c25058c) = ax;
    }
    if (span && r_ecx2) {
        /* TISPAN32 0x1c40b0a2 */
        int16_t lvl = 0;
        switch (CODE2) {
        case 0x49: lvl = 0x3b; break;
        case 0x4a: lvl = 0x3e; break;
        case 0x51: lvl = 0x32; break;
        case 0x52: lvl = (CODE1 != 0x41 && CODE3 == 0x6c) ? 0x2b : 0x25; break;
        case 0x54: lvl = 0x3a; break;
        default: break;
        }
        W(0x1c25055c) = lvl;
    } else if (!span && (r_ecx2 || CODE2 == 0x3f)) {
        int16_t lvl = 0;
        switch (CODE2) {
        case 0x3f: lvl = 0x2a; break;
        case 0x49: lvl = 0x3c; break;
        case 0x4a: lvl = 0x3e; break;
        case 0x4d: lvl = 0x32; break;
        case 0x4e: lvl = 0x2c; break;
        case 0x4f: lvl = 0x3a; break;
        case 0x50:
        case 0x51: lvl = 0x38; break;
        case 0x52: lvl = (CODE1 != 0x41 && CODE3 == 0x36) ? 0x34 : 0x2e; break;
        case 0x54:
        case 0x55: lvl = 0x3b; break;
        default: break;
        }
        W(0x1c25055c) = lvl;
    }
    uint32_t r_ecx3 = ATTR2 & 0x10000;
    if (r_ecx3 && !(ATTR3 & 0x80))
        W(0x1c25055c) += 4;
    uint32_t r_eax = ATTR1 & 0x8000;
    if (r_eax && F10_2 == 0)
        W(0x1c25055c) -= 6;
    else if (!r_eax && F10_2 == 0)
        W(0x1c25055c) -= 3;
    if (r_ebx)
        W(0x1c25055c) -= 3;
    else if (!span && CODE3 == 0x1c)
        W(0x1c25055c) -= 5;
    if (CODE1 == 0x41 && (FLAGS1 & 0x80))
        W(0x1c25055c) -= 3;
    if (!span && r_esi2 && CODE3 == 0x36)
        W(0x1c25055c) += 6;
    if (r_edx)
        return;
    if (CODE3 == 0x40)
        W(0x1c25055c) = 0;
    if (ATTR3 & 1)
        return;
    uint32_t nx;
    if (ATTR3 & 0x40000u)
        nx = ATTR3;
    else if (CODE3 == 0x40 && (ATTR4 & 0x40000u))
        nx = ATTR4;
    else
        return;
    /* 0x1c249b74 / 0x1c249c17: no burst between homorganic stops. */
    if (((ATTR2 & 0x400) && (nx & 0x400)) || ((ATTR2 & 2) && (nx & 2)) ||
        (r_esi2 && (nx & 0x4000000u)) || (r_ecx3 && (nx & 0x10000)))
        W(0x1c25055c) = 0;
}

/* ------------------------------------------------------------------------ */
/* F0: 0x1c203ab0 (mode 0, the normal intonation), 0x1c2042c0 (mode 3,      */
/* notes), 0x1c204030 (mode 4). Mode 0 always runs; it consumes the         */
/* intonation arrays (engine into_c4/c8/cc/d8) at each accent-group start   */
/* and places pitch targets into single frames, which FUN_1c00cd40 later    */
/* interpolates between.                                                    */
/* ------------------------------------------------------------------------ */

/* The command switch 0x32/0x8c/0xb4/0xf0/0xfa shared by the three. Returns
 * the note or pitch for 0xb4 in *pitch (or -1 if the command had none). */
static int pitch_command(sv_engine *e, const int16_t *c, int notes, int32_t *pitch)
{
    *pitch = -1;
    switch (c[0]) {
    case 0x32: /* FUN_1c204680 is a bare `ret` */
        break;
    case 0x8c:
        e->st_ae = (uint16_t)c[1];
        break;
    case 0xb4: {
        int16_t ax = c[1];
        if (ax > 0) {
            if (ax >= 0x7d0)
                ax = 0x7d0;
            if (ax <= 0xa)
                ax = 0xa;
            if (!notes)
                e->st_46 = (uint16_t)ax;
            *pitch = ax;
        } else {
            int16_t bx = (int16_t)((int16_t)(e->st_ac * 12) + (int16_t)e->st_ae - ax);
            if (bx >= 0x55)
                bx = (int16_t)(bx - (int16_t)((uint16_t)(bx - 0x49) / 12u * 12u));
            if (!notes)
                break; /* 0x1c203b31: computed and discarded in mode 0 */
            if (bx < 0)
                bx = (int16_t)(bx + (int16_t)((uint16_t)(0xb - bx) / 12u * 12u));
            *pitch = -2 - bx; /* resolved by the caller through the table */
        }
        break;
    }
    case 0xf0:
        e->st_4c = (uint16_t)c[1];
        break;
    case 0xfa:
        e->st_60 = (uint16_t)c[1];
        break;
    default:
        break;
    }
    return 0;
}

static uint16_t note_or_pitch(sv_engine *e, int32_t p)
{
    if (p <= -2)
        return UW(0x1c24c028 + 2 * (-2 - p)); /* the note table */
    return (uint16_t)p;
}

static void f0_mode0(sv_engine *e, void *fr, const sv_record *cur)
{
    const int16_t *c = cur->commands;
    int32_t dummy;
    if (c && c[0] != SV_CMD_END) {
        do {
            pitch_command(e, c, 0, &dummy);
            c += 3;
        } while (c[0] != SV_CMD_END);
    }
    uint32_t ebx = (FLAGS2 & 4) >> 2;
    uint32_t a3 = ATTR3;
    int ebp = (a3 & 0x20000000u) || !(a3 & 0x8000000u) ||
              ((a3 & 0x1000000u) && !(a3 & 0x8000));
    if (FLAGS2 & 0x80) {
        /* 0x1c203bcc: an accent group starts here. */
        if ((ATTR1 & 0x100) && (ATTR2 & 0x8000000u))
            L(0x1c250a00) = 0x64;
        else
            L(0x1c250a00) = *e->into_c4 * 2;
        L(0x1c250e80) = *e->into_c8 * 2;
        e->into_c4++;
        L(0x1c250e84) = *e->into_cc * 2;
        e->into_c8++;
        e->into_cc++;
        W(0x1c250e6a) = (int16_t)((*e->into_d8 & 0x70) >> 1);
        e->into_d8++;
        L(0x1c250148) = 1;
        L(0x1c2552f4) = 0;
        uint32_t a = (ATTR2 & 0x2000000u) ? ATTR2
                   : (ATTR3 & 0x2000000u) ? ATTR3
                   : (ATTR4 & 0x2000000u) ? ATTR4 : 0;
        if (!(a & 0x400000u)) {
            if (a & 0x200) {
                L(0x1c250a00) += 4;
                L(0x1c250e80) += 7;
                L(0x1c250e84) += 4;
            } else if (a & 0x4000) {
                L(0x1c250a00) -= 4;
                L(0x1c250e80) -= 7;
                L(0x1c250e84) -= 4;
            }
        }
    }
    if (e->st_4c != 0)
        return;
    if ((ATTR2 & 0x2000) && (ATTR1 & 0x800000u) && (ATTR3 & 0x10000000u) && F10_3)
        pitch_mid(e, fr, L(0x1c250a00) - 10);
    if (L(0x1c2552f4)) {
        pitch_last(e, fr, W(0x1c250e6a) ? W(0x1c250e6a) + L(0x1c250e84) : L(0x1c250e84));
        return;
    }
    if (L(0x1c250148)) {
        L(0x1c2552f4) = 0;
        if (ATTR2 & 0x8000000u) {
            L(0x1c250148) = 0;
            if (ATTR2 & 0x2000000u) {
                pitch_first(e, fr, L(0x1c250a00));
            } else if (ATTR1 & 0x100) {
                pitch_first(e, fr, L(0x1c250a00));
                pitch_mid(e, fr, (L(0x1c250e80) + L(0x1c250a00)) >> 1);
            } else if (ebx && (ATTR3 & 0x2000000u)) {
                pitch_mid(e, fr, L(0x1c250a00));
            } else {
                pitch_last(e, fr, L(0x1c250a00));
            }
        }
    }
    if (ATTR2 & 0x2000000u) {
        int32_t v = L(0x1c250e80);
        if ((ATTR1 & 0x40000u) && !(ATTR1 & 0x8000000u))
            pitch_first(e, fr, v + 8);
        else if (F10_2) {
            if (ebx)
                pitch_first(e, fr, v);
            else if (FLAGS3 & 0x80)
                pitch_mid(e, fr, v);
            else if (ATTR3 & 0x10002000u)
                pitch_next(e, fr, v);
            else
                pitch_last(e, fr, v);
        } else if (ebx) {
            pitch_first(e, fr, v);
        } else {
            pitch_mid(e, fr, v);
        }
        if (!ebx) {
            if (W(0x1c250e6a) == 0) {
                if (FLAGS3 & 0x80)
                    pitch_last(e, fr, L(0x1c250e84));
                else
                    pitch_next(e, fr, L(0x1c250e84));
            }
            return;
        }
    }
    if (!ebx)
        return;
    if (ebp && W(0x1c250e6a) == 0) {
        pitch_last(e, fr, L(0x1c250e84));
        L(0x1c2552f4) = 1;
        return;
    }
    if (W(0x1c250e6a) == 0)
        return;
    if (ATTR2 & 0x2000000u)
        pitch_mid(e, fr, L(0x1c250e84));
    if (ebp) {
        pitch_last(e, fr, W(0x1c250e6a) + L(0x1c250e84));
        L(0x1c2552f4) = 1;
    }
}

static void f0_mode3(sv_engine *e, void *fr, const sv_record *cur) /* 0x1c2042c0 */
{
    uint16_t cx = UW(0x1c250560);
    for (uint16_t k = 0; k < NFR2; k++)
        FW(fr, 32 * k + 0x18) = cx;
    const int16_t *c = cur->commands;
    if (!c || c[0] == SV_CMD_END)
        return;
    do {
        int32_t p;
        pitch_command(e, c, 1, &p);
        if (c[0] == 0xb4) {
            cx = note_or_pitch(e, p);
            for (int16_t k = c[2]; (uint16_t)k < NFR2; k++)
                FW(fr, 32 * (int32_t)k + 0x18) = cx;
            UW(0x1c250560) = cx;
        }
        c += 3;
    } while (c[0] != SV_CMD_END);
}

static void f0_mode4(sv_engine *e, void *fr, const sv_record *cur) /* 0x1c204030 */
{
    const int16_t *c = cur->commands;
    if (c && c[0] != SV_CMD_END) {
        do {
            int32_t p;
            pitch_command(e, c, 1, &p);
            if (c[0] == 0xb4) {
                uint16_t v = note_or_pitch(e, p);
                for (int16_t k = c[2]; NFR2 > (uint16_t)k; k++)
                    FW(fr, 32 * (int32_t)k + 0x18) = v;
            }
            c += 3;
        } while (c[0] != SV_CMD_END);
    }
    if ((FLAGS2 & 0x80) || (ATTR2 & 0x20000000u)) {
        uint16_t ax;
        if (!(ATTR2 & 0x20000000u) && CODE2 != 0x1c) {
            uint32_t p = e->st_46;
            ax = (uint16_t)(W(0x1c24c1a0 + 2 * ((CODE2 + p) & 7)) * (int32_t)p / 0x6e);
        } else {
            ax = UW(0x1c250560);
        }
        FW(fr, -8) = UW(0x1c250560);
        FW(fr, 0x18) = ax;
        UW(0x1c250560) = ax;
    }
}

/* ------------------------------------------------------------------------ */
/* 0x1c204690 — the generator entry, once per record                        */
/* ------------------------------------------------------------------------ */

/* Table index of the form `(v + half) / step` taken as a byte (`mov dl,al`)
 * into a 256-entry .text table. */
#define IDX(v, half, shift) ((uint8_t)(((int32_t)(v) + (half)) / (1 << (shift))))

static void load_window(sv_engine *e, const sv_record *r, uint32_t code_va, uint32_t attr_va,
                        uint32_t f10_va, uint32_t stress_va, uint32_t flags_va,
                        uint32_t nfr_va, uint32_t cls_va, int end_as_1b)
{
    uint8_t c = (uint8_t)r->code;
    B(code_va) = (end_as_1b && c == 0xff) ? 0x1b : c;
    L(attr_va) = (int32_t)r->attrs;
    B(f10_va) = (uint8_t)(r->flags & 0x10);
    B(stress_va) = (uint8_t)r->stress;
    L(flags_va) = (int32_t)r->flags;
    UW(nfr_va) = (uint16_t)(((int32_t)r->duration + 4) / 8);
    UW(cls_va) = manner_class(r);
}


int sv_lang_generate(sv_engine *e)
{
    sv_langgen *g = G;
    sv_record *cur = e->window[2];
    if (cur->code == SV_RECORD_END) {
        L(0x1c24c154) = 1;
        return 0;
    }
    g->frame = e->frame_cursor;

    if (L(0x1c24c154)) {
        /* 0x1c2046db: once per sentence. */
        static const uint32_t order[17] = {F0, F1, F2, F3, B1, B2, B3, AV, AF,
                                           AH, AK, K1, Q1, TL, DI, MH, MW};
        static const uint32_t cursor[17] = {
            CUR_F0, CUR_F1, CUR_F2, CUR_F3, CUR_B1, CUR_B2, CUR_B3, CUR_AV, CUR_AF,
            CUR_AH, CUR_AK, CUR_K1, CUR_Q1, CUR_TL, CUR_DI, CUR_MH, CUR_MW};
        static const uint32_t buffer[17] = {
            0x1c250ef0, 0x1c2512f0, 0x1c2516f0, 0x1c251af0, 0x1c251ef0, 0x1c2522f0,
            0x1c2526f0, 0x1c252af0, 0x1c252ef0, 0x1c2532f0, 0x1c2536f0, 0x1c253af0,
            0x1c253ef0, 0x1c2542f0, 0x1c2546f0, 0x1c254af0, 0x1c254ef0};
        for (int k = 0; k < 17; k++) {
            L(0x1c250e90 + 4 * k) = (int32_t)order[k];
            L(cursor[k]) = (int32_t)buffer[k];
            T(order[k], 0x04) = (int32_t)buffer[k];
        }
        W(0x1c250e50) = 0;
        W(0x1c250e6a) = 0;
        L(0x1c250148) = 0;
        L(0x1c2552f4) = 0;
        L(0x1c250e84) = 0x6e;
        L(0x1c250e80) = 0x6e;
        L(0x1c250a00) = 0x6e;
        L(0x1c250ee0) = 0;
        T(F0, 0x08) = 0x64; T(DI, 0x08) = 0; T(F1, 0x08) = 0x190;
        T(B1, 0x08) = 0x64; T(TL, 0x08) = 0xf; T(F2, 0x08) = 0x578;
        T(B2, 0x08) = 0x64; T(Q1, 0x08) = 0x190; T(AV, 0x08) = 0;
        T(AH, 0x08) = 0; T(AF, 0x08) = 0; T(AK, 0x08) = 0; T(MH, 0x08) = 0;
        T(MW, 0x08) = 0; T(F3, 0x08) = 0x960; T(B3, 0x08) = 0xc8;
        T(K1, 0x08) = 0xbb8;
        g->s3c = (int16_t)e->st_98;
        g->s40 = (int16_t)e->st_9c;
        g->s44 = (int16_t)e->st_9a;
        g->s48 = (int16_t)e->st_9e;
        FW(g->frame, 0x18) = e->st_46;
        uint16_t di = FW(g->frame, 0x18);
        UW(0x1c250560) = di;
        FW(g->frame, (((int32_t)cur->duration + 4) / 8) * 32 + 0x18) = di;
        for (int k = 0; k < 17; k++) {
            uint32_t t = (uint32_t)L(0x1c250e90 + 4 * k);
            int32_t v = T(t, 0x08);
            T(t, 0x14) = v; T(t, 0x10) = v; T(t, 0x0c) = v;
            v = T(t, 0x08);
            T(t, 0x24) = v; T(t, 0x20) = v; T(t, 0x1c) = v; T(t, 0x18) = v;
            T(t, 0x00) = L(0x1c24c158 + 4 * k);
        }
        B(0x1c250e69) = 1;
        L(0x1c24c154) = 0;
    }

    /* 0x1c2049f6: the five-record window. */
    load_window(e, e->window[0], 0x1c255301, 0x1c250ed8, 0x1c250edc, 0x1c250550,
                0x1c2552f8, 0x1c2509b2, 0x1c2509b0, 0);
    load_window(e, e->window[1], 0x1c255303, 0x1c250e64, 0x1c250e68, 0x1c255302,
                0x1c2552f0, 0x1c250558, 0x1c2505f0, 0);
    load_window(e, cur, 0x1c250e7e, 0x1c250e70, 0x1c250e74, 0x1c250ed4,
                0x1c250e78, 0x1c250566, 0x1c250564, 0);
    load_window(e, e->window[3], 0x1c250810, 0x1c2552fc, 0x1c255300, 0x1c25055e,
                0x1c250554, 0x1c250e60, 0x1c250e7c, 1);
    load_window(e, e->window[4], 0x1c2509b4, 0x1c250e5c, 0x1c250e62, 0x1c250e6c,
                0x1c250e54, 0x1c250146, 0x1c250860, 1);
    B(0x1c250e63) = B(0x1c250e69);

    for (int k = 0; k < 17; k++) {
        uint32_t t = (uint32_t)L(0x1c250e90 + 4 * k);
        T(t, 0x28) = 0x32;
        T(t, 0x38) = 0x19;
        T(t, 0x3c) = 0x4b;
        T(t, 0x2c) = 7;
        T(t, 0x30) = 7;
        T(t, 0x34) = 3;
    }
    W(0x1c250e88) = W(0x1c25058c);
    if (e->lang->id == SV_LANG_SPANISH) {
        formant_targets_span(e, cur);
        voicing_targets_span(e);
    } else {
        formant_targets(e, cur);
        voicing_targets(e);
    }
    frication_targets(e, cur);

    /* 0x1c204c45: rate rows from the voice. */
    {
        int32_t v = e->lang->id == SV_LANG_SPANISH ? 5 : (int16_t)(e->st_5e + 3);
        /* TISPAN32 0x1c4045dc fixes the six formant/bandwidth rows at 5. */
        T(F3, 0x34) = v; T(F2, 0x34) = v; T(F1, 0x34) = v; T(B3, 0x34) = v;
        T(B2, 0x34) = v; T(B1, 0x34) = v;
        v = (int16_t)(e->st_5e + 3); T(Q1, 0x34) = v; T(K1, 0x34) = v;
        T(AV, 0x34) = (int16_t)e->st_7e + 3;
        T(AH, 0x34) = (int16_t)e->st_80 + 3;
        v = (int16_t)e->st_82 + 3;
        T(AK, 0x34) = v;
        T(AF, 0x34) = v;
    }
    for (int k = 0; k < 17; k++)
        track_contour(e, (uint32_t)L(0x1c250e90 + 4 * k));

    /* 0x1c204cc8: amplitudes in dB to linear levels, table at 0x1c20660e. */
    {
        sv_a16 *av = (sv_a16 *)DP((uint32_t)T(AV, 0x04));
        sv_a16 *ah = (sv_a16 *)DP((uint32_t)T(AH, 0x04));
        sv_a16 *af = (sv_a16 *)DP((uint32_t)T(AF, 0x04));
        for (uint16_t k = 0; k < NFR2; k++) {
            int16_t v = av[k] > 0 ? av[k] : 0;
            av[k] = TB(0x1c20660e + v);
            v = ah[k] > 0 ? ah[k] : 0;
            ah[k] = (int16_t)(TB(0x1c20660e + v) - 10);
            v = af[k] > 0 ? af[k] : 0;
            af[k] = TB(0x1c20660e + v);
        }
    }
    burst(e);

    if ((ATTR2 & 0x40000u) && (ATTR2 & 0x8000000u) &&
        (e->lang->id != SV_LANG_SPANISH || (F10_2 &&
            ((ATTR1 & 0x20000000u) || (FLAGS1 & 8) || CODE2 == 0x4f)))) {
        /* 0x1c204d5c: voiced stop closure — voice bar. */
        int16_t d = ((ATTR1 & 0x8000000u) && (ATTR1 & 0x800000u) && e->st_2f2 != 2) ? 5 : 0x3c;
        g->s1c = (g->s1c & 0xffff0000u) | (uint16_t)d;
        sv_a16 *av = (sv_a16 *)DP((uint32_t)T(AV, 0x04));
        sv_a16 *ah = (sv_a16 *)DP((uint32_t)T(AH, 0x04));
        sv_a16 *f1 = (sv_a16 *)DP((uint32_t)T(F1, 0x04));
        sv_a16 *b1 = (sv_a16 *)DP((uint32_t)T(B1, 0x04));
        sv_a16 *b2 = (sv_a16 *)DP((uint32_t)T(B2, 0x04));
        for (uint16_t k = 0; NFR2 > k; k++) {
            av[k] = (int16_t)(av[k] - d);
            ah[k] = 0;
            f1[k] = e->st_52 == 1 ? 0xfa : 0x78;
            b1[k] = (int16_t)(b1[k] + 0x28);
            b2[k] = (int16_t)(b2[k] + 0xfa);
        }
    }
    int nasal = (ATTR2 & 0x8000u) != 0;
    const int span = e->lang->id == SV_LANG_SPANISH;
    uint32_t s34 = nasal ? 0x10 : 0;

    static const uint32_t cur17[17] = {
        CUR_F0, CUR_F1, CUR_F2, CUR_F3, CUR_B1, CUR_B2, CUR_B3, CUR_K1, CUR_Q1,
        CUR_AV, CUR_AH, CUR_AF, CUR_AK, CUR_DI, CUR_TL, CUR_MW, CUR_MH};
    static const uint32_t trk17[17] = {F0, F1, F2, F3, B1, B2, B3, K1, Q1,
                                       AV, AH, AF, AK, DI, TL, MW, MH};
    for (int k = 0; k < 17; k++)
        L(cur17[k]) = T(trk17[k], 0x04);

    /* 0x1c204f19: one frame per iteration. */
    int32_t f1v, f2v = (int32_t)g->s1c, f3v = (int32_t)g->s1c, b1v = (int32_t)g->s1c;
    int32_t b3v = 0, s14, s30 = 0, s38 = 0, s50 = 0;
    uint32_t s54 = 0;
    for (uint16_t fi = 0; fi < NFR2; fi++) {
        const int16_t *c = cur->commands;
        if (c && c[0] != SV_CMD_END) {
            g->s1c = fi;
            do {
                if ((int32_t)c[2] == (int32_t)g->s1c) {
                    int16_t v = c[1];
                    switch (c[0]) {
                    case 0x32: {
                        /* 0x1c20508a: switch voice mid-phoneme. */
                        const uint8_t *row = e->lang->voices + v * 74;
#define RW(o) sv_rd16(row + (o))
                        if (e->st_4c != 3) {
                            e->st_4c = RW(0x12);
                            e->st_44 = RW(0x08);
                        }
                        e->st_52 = RW(0x0a); e->st_2f2 = RW(0x0e); e->st_46 = RW(0x04);
                        e->st_ac = RW(0x06); e->st_60 = RW(0x14); e->st_62 = RW(0x16);
                        e->slew_rise = RW(0x1a); e->slew_fall = RW(0x1c);
                        e->st_8a = RW(0x10); e->st_98 = RW(0x42); e->st_9a = RW(0x44);
                        e->st_9c = RW(0x46); e->st_9e = RW(0x48); e->st_92 = RW(0x36);
                        e->st_94 = RW(0x38); e->st_96 = RW(0x3a); e->st_8c = RW(0x3c);
                        e->st_8e = RW(0x3e); e->st_90 = RW(0x40); e->st_84 = RW(0x30);
                        e->st_86 = RW(0x32); e->st_88 = RW(0x34); e->st_7e = RW(0x2a);
                        e->st_80 = RW(0x2c); e->st_82 = RW(0x2e); e->st_5c = RW(0x26);
                        e->st_5e = RW(0x28);
#undef RW
                        break;
                    }
                    case 0xbe: e->st_44 = (uint16_t)v; break;
                    case 0xc8: e->st_52 = (uint16_t)v; break;
                    case 0xdc: e->st_2f2 = (uint16_t)v; break;
                    case 0xe6: e->st_8a = (uint16_t)v; break;
                    case 0xf0: e->st_4c = (uint16_t)v; break;
                    case 0x140: e->st_5c = (uint16_t)v; break;
                    case 0x14a: e->st_5e = (uint16_t)v; break;
                    case 0x154: e->st_7e = (uint16_t)v; break;
                    case 0x15e: e->st_80 = (uint16_t)v; break;
                    case 0x168: e->st_82 = (uint16_t)v; break;
                    case 0x172: e->st_84 = (uint16_t)v; break;
                    case 0x17c: e->st_86 = (uint16_t)v; break;
                    case 0x186: e->st_88 = (uint16_t)v; break;
                    case 0x1f4: e->st_92 = (uint16_t)v; break;
                    case 0x1fe: e->st_94 = (uint16_t)v; break;
                    case 0x208: e->st_96 = (uint16_t)v; break;
                    case 0x226: e->st_8c = (uint16_t)v; break;
                    case 0x230: e->st_8e = (uint16_t)v; break;
                    case 0x23a: e->st_90 = (uint16_t)v; break;
                    case 0x334:
                        s34 |= 0x20;
                        g->s10 = (g->s10 & 0xffffu) | ((uint32_t)(uint16_t)v << 16);
                        break;
                    case 0x352:
                        s34 |= 8;
                        g->s10 = (g->s10 & 0xffff00ffu) | ((uint32_t)(uint8_t)v << 8);
                        break;
                    default: break;
                    }
                }
                c += 3;
            } while (c[0] != SV_CMD_END);
        }

        s14 = (int16_t)(CV(CUR_TL) >> 8);
        if (nasal) {
            /* 0x1c205313: fixed nasal formants by code. */
            switch (CODE2) {
            case 0x3c: case 0x57:
                f2v = 0x4a1; f3v = 0x898; f1v = 0x14a; b3v = 0x3c; b1v = 0x1f4; break;
            case 0x3d: case 0x58:
                f2v = 0x640; f3v = 0x8fc; f1v = 0x109; b3v = 0x3c; b1v = 0x1f4; break;
            case 0x3e:
                f2v = 0x6d6; f3v = 0x8ca; f1v = 0xfa; b3v = 0x3c; b1v = 0x1f4; break;
            default:
                f1v = 0; /* not reached by any nasal in the English table */
                break;
            }
        } else {
            f1v = CV(CUR_F1);
            f2v = CV(CUR_F2);
            f3v = CV(CUR_F3);
            b1v = CV(CUR_B1);
            g->s28 = CV(CUR_B2);
            b3v = CV(CUR_B3);
        }

        /* 0x1c2053db: glottal source settings. */
        if (e->st_2f2 == 1) {
            if (e->st_52 == 0)
                b1v += 0x3c;
            int32_t d = CV(CUR_AV) - 7;
            if (d <= CV(CUR_AH))
                d = CV(CUR_AH);
            CV(CUR_AH) = (int16_t)d;
            if (CV(CUR_AV) > 0x14)
                CV(CUR_AV) = (int16_t)(CV(CUR_AV) + 4);
            s14 = (int16_t)(s14 - 4);
        } else if (e->st_2f2 == 2) {
            int32_t d = CV(CUR_AV) - 3;
            if (d <= CV(CUR_AH))
                d = CV(CUR_AH);
            b1v = 0xc8;
            CV(CUR_AH) = (int16_t)d;
            CV(CUR_AV) = 0;
        } else if (nasal) {
            CV(CUR_AH) = 0;
        } else {
            int32_t d = CV(CUR_AV) - 0x14;
            if (d <= CV(CUR_AH))
                d = CV(CUR_AH);
            CV(CUR_AH) = (int16_t)d;
        }
        if (ATTR2 & 0x8000)
            CV(CUR_AH) = (int16_t)(CV(CUR_AH) - 7);

        if (span && GLIDE) {
            /* TISPAN32 0x1c404e74: a glide alternates its formants frame by
             * frame, strongest on every fourth-plus-one frame. */
            switch (fi & 3) {
            case 0:
            case 2:
                f1v -= 0x32;
                f2v = (f2v * 3 + 0x4b0) >> 2;
                g->s28 = (g->s28 + 0xc8) >> 1;
                b1v += 0x28;
                CV(CUR_AV) = (int16_t)(CV(CUR_AV) - 3);
                f3v = (f3v + 0x7d0) >> 1;
                break;
            case 1:
                f1v -= 0x64;
                f2v = (f2v + 0x4b0) >> 1;
                b1v += 0x50;
                CV(CUR_AV) = (int16_t)(CV(CUR_AV) - 6);
                g->s28 = 0xc8;
                f3v = 0x7d0;
                break;
            default:
                break;
            }
        }

        /* 0x1c2054b0: formant scaling by voice type (st_52: 1 child-like,
         * 2 and 3 other vocal tracts), with st_5c as the depth. */
        int32_t ebp = (int16_t)e->st_5c + 10;
        if (e->st_52 == 1) {
            s14 = (int16_t)(s14 - 5);
            CV(CUR_AV) = (int16_t)(CV(CUR_AV) - 0xa);
            CV(CUR_AH) = (int16_t)(CV(CUR_AH) - 0x14);
            CV(CUR_AF) = (int16_t)(CV(CUR_AF) - 6);
            if (span) {
                /* TISPAN32 0x1c404f15: Spanish also rescales the formants. */
                f3v += 0x1d1;
                int32_t n1 = (f1v - 0x1c2) * ebp * 115 / 1000 + 0x24a;
                int32_t n2 = (f2v - 0x5aa) * ebp * 135 / 1000 + 0x6de;
                if (f3v - n2 < 0x12c)
                    f3v = n2 + 0x12c;
                b1v = n1 * b1v / f1v + 0x32;
                g->s28 = n2 * g->s28 / f2v;
                if (b1v >= 0x15e)
                    b1v = 0x15e;
                if (b1v <= 0x46)
                    b1v = 0x46;
                f1v = n1;
                f2v = n2;
            }
            CV(CUR_K1) = (int16_t)(CV(CUR_K1) + 0x96);
        } else if (e->st_52 == 2) {
            s14 = (int16_t)(s14 - 1);
            CV(CUR_AV) = (int16_t)(CV(CUR_AV) - 0xd);
            s50 = f2v;
            f3v += 0x429;
            CV(CUR_AH) = (int16_t)(CV(CUR_AH) - 0x16);
            s30 = f1v;
            CV(CUR_AF) = (int16_t)(CV(CUR_AF) - 7);
            f1v = (f1v - 0x1c2) * ebp * 145 / 1000 + 0x2e4;
            f2v = (f2v - 0x5aa) * ebp * 145 / 1000 + 0x834;
            if (f3v - f2v < 0x15e)
                f3v = f2v + 0x15e;
            b1v = f1v * b1v / s30 + 0x96;
            g->s28 = f2v * g->s28 / s50 + 0x32;
            if (b1v >= 0x12c)
                b1v = 0x12c;
            if (b1v <= 0x64)
                b1v = 0x64;
            CV(CUR_K1) = (int16_t)(CV(CUR_K1) + 0xfa);
        } else {
            s50 = f2v;
            s30 = f1v;
            s38 = f3v;
            int32_t a = (f1v - 0x1c2) * ebp, b = (f2v - 0x5aa) * ebp, cc = (f3v - 0x9a1) * ebp;
            if (e->st_52 == 3) {
                f1v = a * 92 / 1000 + 0x190;
                f2v = b * 92 / 1000 + 0x564;
                f3v = cc * 92 / 1000 + 0x92e;
                if (f3v - f2v < 0xfa)
                    f3v = f2v + 0xfa;
                CV(CUR_K1) = (int16_t)(CV(CUR_K1) - 0x64);
            } else {
                f1v = a * 100 / 1000 + 0x1c2;
                f2v = b * 100 / 1000 + 0x5aa;
                f3v = cc * 100 / 1000 + 0x9a1;
            }
            b1v = f1v * b1v / s30;
            g->s28 = f2v * g->s28 / s50;
            b3v = f3v * b3v / s38;
            if (b1v >= 0x12c)
                b1v = 0x12c;
            if (b1v <= 0x28)
                b1v = 0x28;
            int32_t t = (int16_t)e->st_46 - 0x6e;
            if (t <= 0)
                t = 0;
            t /= 14;
            if (t >= 0x14)
                t = 0x14;
            CV(CUR_AV) = (int16_t)(CV(CUR_AV) - t);
            CV(CUR_AH) = (int16_t)(CV(CUR_AH) - 0xd);
            CV(CUR_AF) = (int16_t)(CV(CUR_AF) - 2);
            f1v = (f1v + f1v * 33) / 33;
        }

        /* 0x1c205783: voice biases and formant scale factors. */
        CV(CUR_AV) = (int16_t)(CV(CUR_AV) + e->st_84);
        CV(CUR_AH) = (int16_t)(CV(CUR_AH) + e->st_86);
        CV(CUR_AF) = (int16_t)(CV(CUR_AF) + e->st_88);
        int32_t x1 = (int16_t)e->st_92 * f1v / 100;
        int32_t x2 = (int16_t)e->st_94 * f2v / 100;
        int32_t x3 = (int16_t)e->st_96 * f3v / 100;
#define CLAMP(v, lo, hi) do { if ((v) >= (hi)) (v) = (hi); if ((v) <= (lo)) (v) = (lo); } while (0)
        CLAMP(x1, 0x64, 0xff0);
        CLAMP(x2, 0x64, 0xff0);
        CLAMP(x3, 0x64, 0xff0);
        f1v = x1;
        f2v = x2;
        f3v = x3;
        int32_t y1 = (int16_t)e->st_8c * b1v / 100;
        int32_t y2 = (int16_t)e->st_8e * g->s28 / 100;
        int32_t y3 = (int16_t)e->st_90 * b3v / 100;
        CLAMP(y1, 0x28, 0x1f4);
        CLAMP(y2, 0x28, 0x1f4);
        CLAMP(y3, 0x28, 0x1f4);
        b1v = y1;
        g->s28 = y2;
        b3v = y3;
        g->s3c += sar32((int32_t)((uint32_t)((int16_t)e->st_98 - g->s3c) * 5000u), 16);
        g->s40 += sar32((int32_t)((uint32_t)((int16_t)e->st_9c - g->s40) * 5000u), 16);
        g->s44 += sar32((int32_t)((uint32_t)((int16_t)e->st_9a - g->s44) * 5000u), 16);
        g->s48 += sar32((int32_t)((uint32_t)((int16_t)e->st_9e - g->s48) * 5000u), 16);
        s14 = (int16_t)(s14 + (int16_t)e->st_8a);
        if (s14 >= 0xf)
            s14 = 0xf;
        if (s14 <= 0)
            s14 = 0;
        int32_t k1 = CV(CUR_K1), q1 = CV(CUR_Q1);
        if (e->sample_rate == 0x1f40) {
#define S118(v) ((v) = ((v) * 11) >> 3)
            S118(f1v); S118(b1v); S118(f2v); S118(g->s28); S118(f3v); S118(b3v);
            S118(g->s3c); S118(g->s44); S118(g->s40); S118(g->s48); S118(k1); S118(q1);
#undef S118
        }

        /* 0x1c205a70: mouth shape for this frame, definition +0x17 (or the
         * next entry's, in the second half of a diphthong); 0 keeps the
         * previous one. Reported by the renderer as event 0x3f0. */
        {
            const uint8_t *ph4 = e->window[4]->phonemes;
            uint8_t src;
            if ((ATTR2 & 0xc0100000u) && (uint16_t)(NFR2 >> 1) < fi)
                src = ph4[CODE2 * SV_PH_STRIDE + 0x31];
            else
                src = ph4[CODE2 * SV_PH_STRIDE + 0x17];
            B(0x1c250e69) = src;
            if (B(0x1c250e69) == 0)
                B(0x1c250e69) = B(0x1c250e63);
        }

        /* 0x1c205ae1: the frame. */
        uint8_t *f = (uint8_t *)g->frame;
        f[0x1a] = B(0x1c250e69);
        f[0] = TB(0x1c205ff4 + IDX(f1v, 8, 4));
        f[1] = TB(0x1c205ff4 + IDX(f2v, 8, 4));
        f[2] = TB(0x1c205ff4 + IDX(f3v, 8, 4));
        f[3] = TB(0x1c2060f4 + IDX(g->s3c, 0x10, 5));
        f[4] = TB(0x1c2060f4 + IDX(g->s40, 0x10, 5));
        f[5] = 0;
        f[6] = (uint8_t)((TB(0x1c2061f4 + IDX(b1v, 4, 3)) << 4) + TB(0x1c2061f4 + IDX(g->s28, 4, 3)));
        f[7] = (uint8_t)((TB(0x1c2061f4 + IDX(b3v, 4, 3)) << 4) + (uint8_t)s14);
        f[8] = (uint8_t)((TB(0x1c2062f4 + IDX(g->s44, 4, 3)) << 4) + TB(0x1c2062f4 + IDX(g->s48, 4, 3)));
        f[9] = TB(0x1c2060f4 + IDX(k1, 0x10, 5));
        f[0xb] = CODE2;
        {
            uint16_t ax = (uint16_t)(e->st_44 >> 1);
            if (ax <= 1)
                ax = 1;
            f[0xa] = (uint8_t)ax;
        }
        {
            int32_t v = CV(CUR_AV) * 2;
            if (v >= 0x80) v = 0x80;
            if (v <= 0) v = 0;
            f[0x10] = (uint8_t)v;
            v = CV(CUR_AH) * 2;
            if (v >= 0x80) v = 0x80;
            if (v <= 0) v = 0;
            f[0x11] = (uint8_t)v;
            v = CV(CUR_AF) * 2;
            if (v >= 0x80) v = 0x80;
            if (v <= 0) v = 0;
            f[0x12] = (uint8_t)v;
            v = CV(CUR_AK) * 2 + 0x5c;
            if (v <= 0) v = 0;
            f[0x13] = (uint8_t)v;
        }
        f[0x14] = (uint8_t)(TB(0x1c2062f4 + IDX(q1, 4, 3)) << 4);
        f[0x1b] = (uint8_t)(g->s10 >> 8);
        FW(f, 0x1c) = (uint16_t)(g->s10 >> 16);
        f[0x17] = (uint8_t)s34;
        f[0x1f] = (uint8_t)s54;
        s54++;
        if (e->st_4c == 2)
            FW(f, 0x18) = e->st_46;
        else if (e->st_4c != 4)
            FW(f, 0x18) = 0;
        g->frame++;
        if (s34 & 8)
            s34 ^= 8;
        if (s34 & 0x20)
            s34 ^= 0x20;
        /* 0x1c205de4: fifteen cursors advance; MW and MH never do. */
        for (int k = 0; k < 15; k++)
            L(cur17[k]) += 2;
    }
    (void)b3v;

    /* 0x1c205e64 */
    e->frames[0].pitch = e->st_46;
    void *start = (uint8_t *)g->frame - 32 * (size_t)NFR2;
    f0_mode0(e, start, cur);
    if (e->st_4c == 3)
        f0_mode3(e, start, cur);
    else if (e->st_4c == 4)
        f0_mode4(e, g->frame, cur);
    {
        uint8_t fl = (cur->flags & 0x100) ? 3 : 1;
        if (cur->flags & 0x80)
            fl |= 4;
        ((uint8_t *)start)[0x17] |= fl;
    }
    e->frame_cursor = g->frame;
    e->st_a0 = TB(0x1c2060f4 + IDX((int16_t)e->st_98, 0x10, 5));
    e->st_a4 = TB(0x1c2060f4 + IDX((int16_t)e->st_9c, 0x10, 5));
    e->st_a2 = TB(0x1c2062f4 + IDX((int16_t)e->st_9a, 4, 3));
    e->st_a6 = TB(0x1c2062f4 + IDX((int16_t)e->st_9e, 4, 3));
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Instances                                                                 */
/* ------------------------------------------------------------------------ */
int sv_langgen_attach(sv_langmod *m, const sv_image *data, const sv_image *text,
                      size_t ntext)
{
    sv_langgen *g = calloc(1, sizeof *g);
    if (!g || ntext > SV_LANGGEN_MAX_TEXT) {
        free(g);
        return -1;
    }
    g->d = malloc(data->size);
    if (!g->d) {
        free(g);
        return -1;
    }
    memcpy(g->d, data->bytes, data->size);
    g->dva = data->va;
    g->dsize = data->size;
    for (size_t k = 0; k < ntext; k++)
        g->text[k] = text[k];
    g->ntext = ntext;
    m->priv = g;
    m->generate = sv_lang_generate;
    return 0;
}

void sv_langgen_detach(sv_langmod *m)
{
    sv_langgen *g = m->priv;
    if (g) {
        free(g->d);
        free(g);
    }
    m->priv = NULL;
}
