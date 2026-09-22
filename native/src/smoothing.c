#include "tispeech/smoothing.h"

/* See include/tispeech/smoothing.h for the full derivation. Source:
 * TIBASE32 0x1c00cd40..0x1c00cdb3. */
void sv_smooth_pitch(sv_frame *frames)
{
    sv_frame *anchor = frames; /* 0x1c00cd51: ecx = esi = state->frames_ptr */

    for (;;) {
        sv_frame *scan = anchor + 1; /* 0x1c00cd5d */
        int32_t run = 0;

        /* 0x1c00cd63..0x1c00cd71: scan forward while pitch == 0. */
        while (scan->pitch == 0) {
            run++;
            scan++;
        }

        /* 0x1c00cd73..0x1c00cd76: the terminator check comes BEFORE the run
         * is used. A trailing run of zero-pitch frames that reaches the
         * terminator is left untouched. */
        if (scan->formant_freq[0] == SV_FRAME_END) {
            return;
        }

        /* 0x1c00cd78..0x1c00cd7a */
        if (run > 0) {
            /* 0x1c00cd59..0x1c00cd60 and 0x1c00cd81..0x1c00cd89: both
             * endpoints in Q24.8, formed from zero-extended uint16_t
             * pitches, so this never approaches int32_t overflow. */
            int32_t start = (int32_t)anchor->pitch << 8;
            int32_t end = (int32_t)scan->pitch << 8;
            /* 0x1c00cd7e, 0x1c00cd8c..0x1c00cd8f: signed divide, truncating
             * toward zero exactly as `idivl`. */
            int32_t step = (end - start) / (run + 1);
            int32_t value = start;
            sv_frame *f = anchor;
            int32_t i;

            /* 0x1c00cd97..0x1c00cda6: increment happens before the store,
             * so the anchor frame itself (i == 0) is never rewritten -- the
             * loop fills exactly the `run` interior frames. */
            for (i = 0; i < run; i++) {
                value += step;
                f++;
                /* 0x1c00cd9c..0x1c00cda2: `sarl $0x8,%edx` then store the
                 * low 16 bits. `value` is proven non-negative for every i
                 * reachable here (see the header comment), so the plain
                 * C `>>` below is well-defined and matches the arithmetic
                 * shift exactly. */
                f->pitch = (uint16_t)(value >> 8);
            }
        }

        anchor = scan; /* 0x1c00cda8 */
    }
}

/* ------------------------------------------------------------------------ */
/* x86 arithmetic, spelled out, for the pass below. Same reasoning as         */
/* src/generator.c and src/dsp.c: `imul r32,r32` keeps the low dword, `sar`   */
/* is an arithmetic shift, and the store is a `movw`. sv_smooth_pitch() above */
/* needs none of this because its accumulator is proven to stay inside the    */
/* range where plain C agrees; this one takes its rates from the caller, so   */
/* nothing bounds them and every step is spelled out.                         */
/* ------------------------------------------------------------------------ */

static int32_t sv_wrap32(int64_t v)
{
    return (int32_t)(uint32_t)(uint64_t)v;
}

static int32_t sv_sar32(int32_t v, int shift)
{
    uint32_t u = (uint32_t)v;
    if (v < 0) {
        return (int32_t)~((~u) >> shift);
    }
    return (int32_t)(u >> shift);
}

/* One step of the lag, identical code in both passes:
 *   0x1c00dea6..0x1c00deb4   (forward)
 *   0x1c00dee1..0x1c00deeb   (backward)
 * sub the accumulator's whole part from the frame's pitch, imul by the rate,
 * add the product to the accumulator. Note the shift is on the accumulator
 * BEFORE the subtract, not on the product after it -- unlike the generator's
 * sv_approach(), which multiplies first and shifts the product. */
static int32_t sv_slew_step(int32_t acc, int32_t pitch, int32_t rate)
{
    int32_t delta = sv_wrap32((int64_t)pitch - (int64_t)sv_sar32(acc, 8));
    delta = sv_wrap32((int64_t)delta * (int64_t)rate);
    return sv_wrap32((int64_t)acc + (int64_t)delta);
}

/* See include/tispeech/smoothing.h for the full derivation. Source:
 * TIBASE32 0x1c00de60..0x1c00df00. */
void sv_smooth_pitch_slew(sv_frame *frames, uint16_t rate_rise,
                          uint16_t rate_fall)
{
    const int32_t rise = (int32_t)rate_rise; /* 0x1c00de79: state+0x64 */
    const int32_t fall = (int32_t)rate_fall; /* 0x1c00de71: state+0x66 */
    sv_frame *f = frames;
    int32_t count = 0;
    int32_t i;

    /* 0x1c00de85..0x1c00de89: the accumulator is seeded from frame 0 before
     * the terminator is tested, so a terminator-only array reads that pitch
     * and then does nothing with it. */
    int32_t acc = (int32_t)((uint32_t)frames[0].pitch << 8);

    /* Forward. 0x1c00de8c tests the terminator before each frame, so the
     * terminator itself is never written. */
    while (f->formant_freq[0] != SV_FRAME_END) {
        int32_t pitch = (int32_t)f->pitch; /* 0x1c00de93, zero-extended */
        /* 0x1c00de9c..0x1c00dea2: signed compare of pitch<<8 against the
         * accumulator; strictly greater takes the rising rate. pitch<<8 is a
         * zero-extended uint16 shifted 8, so it cannot itself overflow. */
        int32_t rate = ((int32_t)((uint32_t)pitch << 8) > acc) ? rise : fall;

        acc = sv_slew_step(acc, pitch, rate);
        /* 0x1c00debb: `movw` of the accumulator's whole part. */
        f->pitch = (uint16_t)sv_sar32(acc, 8);
        f++;        /* 0x1c00dea8 */
        count++;    /* 0x1c00deae */
    }

    /* 0x1c00dec4..0x1c00dec7: one frame fewer than the forward pass covered,
     * and the accumulator carries over untouched. `count` is the terminator's
     * index, so this walks frames count-1 down to 1 and leaves frame 0 as the
     * forward pass left it. */
    for (i = count - 1; i > 0; --i) {
        int32_t pitch;
        int32_t rate;

        f--;                               /* 0x1c00dec9 */
        pitch = (int32_t)f->pitch;         /* 0x1c00dece */
        /* 0x1c00ded7..0x1c00dedf: `jge` here against `jg` above -- walking
         * backwards, a tie or a rise in THIS direction is a fall in forward
         * time, so both passes apply `rise` to the same edges. */
        rate = ((int32_t)((uint32_t)pitch << 8) >= acc) ? fall : rise;

        acc = sv_slew_step(acc, pitch, rate);
        f->pitch = (uint16_t)sv_sar32(acc, 8); /* 0x1c00def3 */
    }
}
