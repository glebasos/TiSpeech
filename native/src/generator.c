/*
 * generator.c — TIENG32!FUN_1c203010 and TIENG32!FUN_1c203870.
 *
 * Two leaf functions out of the phoneme -> parameter-frame generator at
 * TIENG32 `0x1C204690`. See include/tispeech/generator.h for provenance and
 * for what is deliberately NOT reconstructed.
 *
 * Both are verified bit-exact against the original by tools/verify_generator.py.
 */

#include "tispeech/generator.h"

/* ------------------------------------------------------------------------ */
/* x86 arithmetic, spelled out.                                              */
/*                                                                           */
/* The original is 32-bit integer code throughout: `imul r32,r32` keeps the   */
/* low dword and discards the overflow, `sar` is an arithmetic shift, and one */
/* add (`addw` at 0x1c2039fc) is only 16 bits wide. Writing any of these as   */
/* plain C would be either undefined (signed overflow, negative >>) or        */
/* subtly wrong (a 32-bit add where the original does 16), so each is a       */
/* named helper that does the wrap explicitly. dsp.c documents the same trap: */
/* the narrowing is part of the algorithm, not an artefact of the compiler    */
/* that produced the original.                                               */
/* ------------------------------------------------------------------------ */

static int32_t sv_wrap32(int64_t v)
{
    return (int32_t)(uint32_t)(uint64_t)v;
}

static int32_t sv_mul32(int32_t a, int32_t b)
{
    return sv_wrap32((int64_t)a * (int64_t)b);
}

static int32_t sv_sar32(int32_t v, int shift)
{
    uint32_t u = (uint32_t)v;
    if (v < 0) {
        return (int32_t)~((~u) >> shift);
    }
    return (int32_t)(u >> shift);
}

/* `v` moved one step toward `target`, as both interpolation passes do it:
 *   0x1c203970..0x1c20397e   (forward)
 *   0x1c203a04..0x1c203a12   (backward)
 * Identical code in both: sub, imul by the rate, sar 8, add. */
static int32_t sv_approach(int32_t v, int32_t target, int32_t rate)
{
    int32_t delta = sv_wrap32((int64_t)target - (int64_t)v);
    return sv_wrap32((int64_t)v + (int64_t)sv_sar32(sv_mul32(delta, rate), 8));
}

/* ------------------------------------------------------------------------ */
/* TIENG32!FUN_1c203010 — phoneme -> manner class.                           */
/*                                                                           */
/* `0x1c203010..0x1c2030aa`. A leaf: the whole function is one table lookup   */
/* and a chain of bit tests. The original takes a phoneme record and reads    */
/* the code from +0x0c and the table base from +0x04; we take the two         */
/* separately, which is the only structural change.                          */
/* ------------------------------------------------------------------------ */
int sv_gen_phoneme_class(const sv_gen_phonemes *table, int code)
{
    const uint8_t *entry;
    uint32_t flags;

    /* 0x1c203014: the terminator is answered before the table is touched, so
     * a caller can classify the end of the array without a valid table. */
    if ((code & 0xFFFF) == 0x00FF) {
        return SV_GEN_CLASS_SILENCE;
    }

    if (table == 0 || table->entries == 0) {
        return SV_GEN_E_ARG;
    }

    /* 0x1c203023: `movzx edx, ax` — the code is used as an UNSIGNED 16-bit
     * index, so a negative int argument means the same entry a 16-bit
     * truncation would select. */
    code &= 0xFFFF;

    /* DIVERGENCE: the original indexes unchecked (0x1c203033) and would read
     * whatever follows the table for an out-of-range code. */
    if ((size_t)code >= table->count) {
        return SV_GEN_E_RANGE;
    }

    /* 0x1c203028..0x1c203033: 26*code, then the dword at +2. */
    entry = table->entries + (size_t)code * SV_GEN_PHONEME_STRIDE;
    entry += SV_GEN_PHONEME_FLAGS;
    flags = (uint32_t)entry[0]
          | ((uint32_t)entry[1] << 8)
          | ((uint32_t)entry[2] << 16)
          | ((uint32_t)entry[3] << 24);

    /* 0x1c203037..0x1c2030aa, in the original's order. The order matters:
     * several phonemes carry more than one of these bits and the first test
     * that fires decides the class. */
    if (flags & SV_GEN_PH_VOWEL) {
        return SV_GEN_CLASS_VOWEL;
    }
    if (flags & SV_GEN_PH_GLIDE) {
        return SV_GEN_CLASS_APPROXIMANT;
    }
    if (flags & SV_GEN_PH_NASAL) {
        return SV_GEN_CLASS_NASAL;
    }
    if (flags & SV_GEN_PH_FRICATIVE) {
        /* 0x1c20305a: `and eax,0x08000000; cmp eax,1; sbb eax,eax; and eax,2;
         * add ax,3` — 3 when the voiced bit is set, 5 when it is not. */
        return (flags & SV_GEN_PH_VOICED) ? SV_GEN_CLASS_FRICATIVE_VOICED
                                          : SV_GEN_CLASS_FRICATIVE_VOICELESS;
    }
    if (flags & SV_GEN_PH_STOP) {
        /* 0x1c203073: the same sequence with a base of 4. */
        return (flags & SV_GEN_PH_VOICED) ? SV_GEN_CLASS_STOP_VOICED
                                          : SV_GEN_CLASS_STOP_VOICELESS;
    }
    if (flags & SV_GEN_PH_ASPIRATE) {
        return SV_GEN_CLASS_ASPIRATE;
    }
    if (flags & SV_GEN_PH_SILENCE) {
        return SV_GEN_CLASS_SILENCE;
    }
    /* 0x1c20309a: `and eax,0x100; cmp eax,1; mov eax,9; adc eax,-1` — 8 when
     * the bit is set (carry clear), 9 when it is not. */
    return (flags & SV_GEN_PH_GLOTTAL) ? SV_GEN_CLASS_GLOTTAL_STOP
                                       : SV_GEN_CLASS_SILENCE;
}

/* ------------------------------------------------------------------------ */
/* TIENG32!FUN_1c203870, pass 1 — the crossfade ramp.                        */
/*                                                                           */
/* `0x1c20387b..0x1c203923`. Writes 256 for the first `hold` frames, then a   */
/* linear fall toward 0, then forces the last frame to 0.                    */
/* ------------------------------------------------------------------------ */
static int sv_gen_ramp(const sv_gen_track *t, int frame_count,
                       int16_t *ramp, size_t capacity, int32_t *hold_out)
{
    int32_t hold, fall, span, step, acc;
    int32_t i;

    /* 0x1c203882..0x1c203897: (hold_percent * n) / 100 + 1. The multiply is
     * `imul` and wraps; the divide is `idiv`, which truncates toward zero,
     * the same way C does. */
    hold = sv_wrap32((int64_t)sv_mul32(t->hold_percent, frame_count) / 100);
    hold = sv_wrap32((int64_t)hold + 1);

    /* 0x1c20389a..0x1c2038a6: (fall_percent * n + 50) / 100 — the +50 is a
     * round-to-nearest that the first expression deliberately does not get. */
    fall = sv_wrap32((int64_t)sv_mul32(t->fall_percent, frame_count) + 50);
    fall = (int32_t)((int64_t)fall / 100);

    /* 0x1c2038a8..0x1c2038b1 */
    span = sv_wrap32((int64_t)fall - (int64_t)hold);
    if (span <= 1) {
        span = 1;
    }

    /* 0x1c2038b4..0x1c2038c0: a Q15 accumulator that starts one step below
     * full scale and is stored shifted down by 7, so the held value 0x8000>>7
     * and the stored ceiling 256 agree. */
    step = 0x8000 / span;
    acc = 0x8000 - step;

    /* DIVERGENCE: with hold <= 0 the original skips the fill (0x1c2038c9) but
     * still counts the second loop from `hold`, so it writes more entries than
     * it indexes and runs off the far end of its buffer. That needs a negative
     * hold_percent, which no track the generator sets up has. Refused rather
     * than reproduced. */
    if (hold <= 0) {
        return SV_GEN_E_ARG;
    }
    if ((size_t)hold > capacity || (size_t)frame_count > capacity) {
        return SV_GEN_E_SCRATCH;
    }

    /* 0x1c2038cb..0x1c2038de: `rep stosl` of 0x01000100 then a trailing
     * `stosw` — 256 in every entry, written two at a time. */
    for (i = 0; i < hold; ++i) {
        ramp[i] = 256;
    }

    /* 0x1c2038f1..0x1c203910. The clamp happens BEFORE the store and the
     * decrement AFTER it, so the first stored value is (0x8000-step)>>7 and
     * the sequence can sit at 0 for the rest of the phoneme. */
    for (i = hold; i < frame_count; ++i) {
        if (acc < 0) {
            acc = 0;
        }
        ramp[i] = (int16_t)sv_sar32(acc, 7);
        acc = sv_wrap32((int64_t)acc - (int64_t)step);
    }

    /* 0x1c20391a: `mov word [0x1c250a4e + n*2], 0` — that address is the ramp
     * base minus 2, so this is ramp[n-1], not ramp[n]. The last frame always
     * belongs entirely to the backward pass. */
    ramp[frame_count - 1] = 0;

    if (hold_out != 0) {
        *hold_out = hold;
    }
    return SV_GEN_OK;
}

int sv_gen_crossfade_ramp(const sv_gen_track *track, int frame_count,
                          int16_t *ramp, size_t capacity)
{
    if (track == 0 || ramp == 0) {
        return SV_GEN_E_ARG;
    }
    if (frame_count < 1 || frame_count > SV_GEN_MAX_FRAMES) {
        return SV_GEN_E_FRAMES;
    }
    return sv_gen_ramp(track, frame_count, ramp, capacity, 0);
}

/* ------------------------------------------------------------------------ */
/* TIENG32!FUN_1c203870 — the whole track.                                   */
/* ------------------------------------------------------------------------ */
int sv_gen_track_contour(const sv_gen_track *track,
                         int frame_count,
                         const sv_gen_rates *rates,
                         int16_t *contour,
                         const sv_gen_scratch *scratch)
{
    int32_t row, fwd_index, bwd_index;
    int32_t fwd_rate, bwd_rate;
    int32_t value;
    int i;
    int status;
    int16_t *ramp;
    int16_t *forward;

    if (track == 0 || rates == 0 || rates->table == 0 || contour == 0
        || scratch == 0 || scratch->ramp == 0 || scratch->forward == 0) {
        return SV_GEN_E_ARG;
    }
    if (frame_count < 1 || frame_count > SV_GEN_MAX_FRAMES) {
        return SV_GEN_E_FRAMES;
    }
    if (scratch->capacity < (size_t)frame_count) {
        return SV_GEN_E_SCRATCH;
    }

    ramp = scratch->ramp;
    forward = scratch->forward;

    /* 0x1c203924..0x1c203937: the row is clamped to 0..8 — `cmp ecx,8; jl`
     * then `test ecx,ecx; jg` — and scaled by 16, the row stride. The COLUMN
     * is not clamped, here or anywhere: the original would read past the end
     * of the table. Refused instead.
     *
     * The original does this lookup between the ramp and the forward pass; it
     * is hoisted above the ramp here so that a refused call leaves the scratch
     * buffers alone as well as the output. That is the only reordering in
     * this function, and it is unobservable to a call that succeeds. */
    row = track->rate_row;
    if (row >= 8) {
        row = 8;
    }
    if (row <= 0) {
        row = 0;
    }
    row = sv_wrap32((int64_t)row * 16);

    fwd_index = sv_wrap32((int64_t)track->fwd_rate_col + (int64_t)row);
    bwd_index = sv_wrap32((int64_t)track->bwd_rate_col + (int64_t)row);
    if (fwd_index < 0 || (size_t)fwd_index >= rates->count
        || bwd_index < 0 || (size_t)bwd_index >= rates->count) {
        return SV_GEN_E_RANGE;
    }
    /* 0x1c20394e and 0x1c2039cb: `mov ebx,[0x1c24c1b0 + eax*4]`. */
    fwd_rate = rates->table[fwd_index];
    bwd_rate = rates->table[bwd_index];

    /* Pass 1 — the crossfade ramp. 0x1c20387b..0x1c203923. */
    status = sv_gen_ramp(track, frame_count, ramp, scratch->capacity, 0);
    if (status != SV_GEN_OK) {
        return status;
    }

    /* Pass 2 — forward. 0x1c203969..0x1c203999.
     * The step toward the target is taken BEFORE the frame is emitted, so
     * forward[0] already reflects one step and fwd_start is never stored. */
    value = track->fwd_start;
    for (i = 0; i < frame_count; ++i) {
        value = sv_approach(value, track->fwd_target, fwd_rate);
        forward[i] = (int16_t)(uint16_t)(uint32_t)
                     sv_sar32(sv_mul32((int32_t)ramp[i], value), 8);
    }

    /* Pass 3 — backward. 0x1c2039e1..0x1c203a1e. Walks from the last frame to
     * the first, which is why the two passes cannot be fused: the backward
     * pass's accumulator advances in the opposite direction.
     *
     * 0x1c2039fc is `addw 0x2(%edi), %ax` — a 16-BIT add into AX whose result
     * is then stored as a word. A 32-bit add followed by a narrowing store
     * gives the same bits here, but only because nothing reads the high half;
     * it is written narrow to keep the source honest about the width. */
    value = track->bwd_start;
    for (i = frame_count - 1; i >= 0; --i) {
        int32_t weight = sv_wrap32(256 - (int64_t)ramp[i]);
        int32_t mixed = sv_sar32(sv_mul32(weight, value), 8);
        uint16_t sum = (uint16_t)((uint32_t)mixed + (uint32_t)(uint16_t)forward[i]);
        contour[i] = (int16_t)sum;
        value = sv_approach(value, track->bwd_target, bwd_rate);
    }

    return SV_GEN_OK;
}
