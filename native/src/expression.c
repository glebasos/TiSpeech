/*
 * expression.c — TIBASE32!FUN_1c00be40, the voice-expression pass.
 *
 * See include/tispeech/expression.h for the derivation, the command set and
 * the table shapes. Every address cited here is a VA in the original
 * TIBASE32.DLL. tools/verify_expression.py proves this against it.
 */
#include "tispeech/expression.h"

/* ------------------------------------------------------------------------ */
/* x86 arithmetic, spelled out, exactly as src/generator.c and src/dsp.c do:  */
/* `imul r32,r32` keeps the low dword, `sar` is an arithmetic shift, and      */
/* several of the stores here are 8 or 16 bits wide. This function leans on   */
/* the narrowing more than most -- the pitch rescale at 0x1c00c00b shifts a   */
/* 16-bit value left by 17 and keeps 32 bits, which loses the top bit of any  */
/* pitch at or above 0x8000.                                                  */
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

static int32_t sv_read_i16(const uint8_t *p)
{
    return (int32_t)(int16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint16_t sv_read_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

/* The vibrato phase step, 0x1c00be70..0x1c00be7d and again at 0x1c00bf52.
 * `lea (eax,eax,2)` then `lea (eax,edx,4)` is a multiply by 13, `shl 6` makes
 * it 832, and the divide is UNSIGNED (`div` with edx zeroed at 0x1c00be76),
 * so a negative rate produces a very large step rather than a negative one.
 * Only the low word is kept. */
static uint16_t sv_vibrato_step(int16_t rate)
{
    int32_t v = (int32_t)rate;
    int32_t triple = sv_wrap32((int64_t)v * 3);

    v = sv_wrap32((int64_t)v + (int64_t)sv_wrap32((int64_t)triple * 4));
    v = sv_wrap32((int64_t)v * 64);
    return (uint16_t)((uint32_t)v / 127u);
}

/* ------------------------------------------------------------------------ */
/* The command list, 0x1c00becb..0x1c00bfd1.                                  */
/*                                                                            */
/* The original dispatches through a 211-byte case-index table at 0x1c00c2e0  */
/* and a 10-entry jump table at 0x1c00c2b8. Nine codes do something; the      */
/* other 202 in range, and everything out of range, fall through to the next  */
/* command. A switch reproduces that exactly, and the bounds test at          */
/* 0x1c00bed1 needs no separate expression because an out-of-range code takes */
/* the same path as an in-range unhandled one.                                */
/* ------------------------------------------------------------------------ */
static int sv_expr_commands(sv_expr_state *state,
                            const uint8_t *commands,
                            const sv_expr_tables *tables,
                            uint16_t *phase_step,
                            int32_t *glide_target,
                            int32_t *glide_step)
{
    /* 0x1c00bebb: a null list is not an error, it is the common case. */
    if (commands == NULL) {
        return SV_EXPR_OK;
    }

    /* 0x1c00bec1 tests the first code before the loop and 0x1c00bfcd tests
     * each following one, which is a plain while. */
    while (sv_read_i16(commands) != SV_EXPR_CMD_END) {
        const int32_t code = sv_read_i16(commands);
        const int32_t value = sv_read_i16(commands + 2);

        switch (code) {
        case SV_EXPR_CMD_VOICE: {
            /* 0x1c00beec. The original computes value * 37 and indexes by
             * word, i.e. value * 74 bytes, with no bounds check at all.
             * DIVERGENCE: a row outside the table is refused rather than
             * reading whatever follows it. */
            const uint8_t *row;

            if (value < 0 || value >= SV_EXPR_VOICE_ROWS) {
                return SV_EXPR_E_RANGE;
            }
            row = tables->voices + (size_t)value * SV_EXPR_VOICE_STRIDE;
            /* Order matters only for a differential that stops early, but it
             * is the original's: +0x16 first (0x1c00bef8), then +0x0c. */
            state->flutter_depth = sv_read_u16(row + SV_EXPR_VOICE_FLUTTER);
            state->source_class = sv_read_u16(row + SV_EXPR_VOICE_SOURCE_CLASS);
            break;
        }

        case SV_EXPR_CMD_VIBRATO:       /* 0x1c00bf21 */
            state->vibrato_depth = (int16_t)value;
            break;

        case SV_EXPR_CMD_TREMOLO:       /* 0x1c00bf2e */
            state->tremolo_depth = (int16_t)value;
            break;

        case SV_EXPR_CMD_SOURCE_MOD:    /* 0x1c00bf3b */
            state->source_depth = (int16_t)value;
            break;

        case SV_EXPR_CMD_VIBRATO_RATE:  /* 0x1c00bf48 */
            state->vibrato_rate = (int16_t)value;
            *phase_step = sv_vibrato_step((int16_t)value);
            break;

        case SV_EXPR_CMD_PITCH: {
            /* 0x1c00bf6c. value * 256 / 100 -- the command's value is a
             * percentage. The quotient is stored as a WORD, so a value past
             * about 12800 wraps; `idiv` truncates toward zero, as C does. */
            const int32_t scaled = sv_wrap32((int64_t)value * 256) / 100;
            const int16_t pitch = (int16_t)scaled;

            state->pitch_scaled = pitch;
            *glide_target = sv_wrap32((int64_t)pitch * 256);
            /* 0x1c00bf85..0x1c00bf9a: a signed 16-bit compare against the
             * PREVIOUS commanded pitch decides which way the glide runs. */
            state->glide_rising = (state->pitch_previous < pitch) ? 1 : 0;
            state->pitch_previous = pitch;
            break;
        }

        case SV_EXPR_CMD_RATE:
            /* 0x1c00bf9f. DIVERGENCE: zero faults on `idiv` in the original. */
            if (value == 0) {
                return SV_EXPR_E_ARG;
            }
            state->glide_rate = (int16_t)value;
            *glide_step = 0x10000 / value;
            break;

        case SV_EXPR_CMD_SOURCE_CLASS:  /* 0x1c00bfb8 */
            state->source_class = (uint16_t)value;
            break;

        case SV_EXPR_CMD_FLUTTER:       /* 0x1c00bfc2 */
            state->flutter_depth = (uint16_t)value;
            break;

        default:
            /* 0x1c00bfca, reached by 202 in-range codes and every code
             * outside 0x32..0x104. */
            break;
        }

        commands += SV_EXPR_CMD_STRIDE;
    }
    return SV_EXPR_OK;
}

/* ------------------------------------------------------------------------ */
/* The glottal source pair, 0x1c00c1c0..0x1c00c284.                           */
/* ------------------------------------------------------------------------ */
static void sv_expr_source(const sv_expr_state *state,
                           const sv_expr_tables *tables,
                           sv_frame *f,
                           uint16_t raw_pitch)
{
    /* 0x1c00c1c0: the mix is set before the switch and only the automatic
     * path overwrites it. */
    f->source_mix = 0xff;

    if (state->source_class > 8) {
        /* 0x1c00c1d6 */
        f->source_index_b = 0;
        f->source_index_a = 0;
        return;
    }

    /* 0x1c00c1e3: class 0 is the automatic choice too, unless this is locked,
     * in which case neither selector is touched. The original expresses that
     * as case 0 falling into case 1. */
    if (state->source_class == 0 && state->source_lock != 0) {
        return;
    }

    switch (state->source_class) {
    case 0:
    case 1:
        if (raw_pitch >= SV_EXPR_SOURCE_MAP_PITCHES) {
            /* 0x1c00c1f6 */
            f->source_index_b = 4;
            f->source_index_a = 4;
        } else {
            /* 0x1c00c258: the first of five candidate entries whose byte is
             * nonzero wins. Running out leaves the index at 5 and the mix at
             * the zero that was just read, which the original stores. */
            uint16_t k = 0;
            uint8_t mix = 0;

            for (;;) {
                mix = tables->source_map[(size_t)k
                        + (size_t)raw_pitch * SV_EXPR_SOURCE_MAP_STRIDE];
                if (mix > 0) {
                    break;
                }
                k = (uint16_t)(k + 1);
                if (k >= 5) {
                    break;
                }
            }
            f->source_index_a = (uint8_t)k;
            /* 0x1c00c276: the pair is (k, k+1) except at the top of the
             * range, where both are 4. */
            if (k != 4) {
                k = (uint16_t)(k + 1);
            }
            f->source_index_b = (uint8_t)k;
            f->source_mix = mix;
        }
        return;

    /* 0x1c00c206..0x1c00c256. Both selectors take the same constant, so the
     * pair degenerates and source_mix stays 0xff. */
    case 2: f->source_index_b = 5;  f->source_index_a = 5;  return;
    case 3: f->source_index_b = 6;  f->source_index_a = 6;  return;
    case 4: f->source_index_b = 7;  f->source_index_a = 7;  return;
    case 5: f->source_index_b = 10; f->source_index_a = 10; return;
    case 6: f->source_index_b = 8;  f->source_index_a = 8;  return;
    case 7: f->source_index_b = 9;  f->source_index_a = 9;  return;
    default: /* 8 */
        f->source_index_b = 11;
        f->source_index_a = 11;
        return;
    }
}

/* ------------------------------------------------------------------------ */

int sv_expression_apply(sv_expr_state *state,
                        sv_frame *frames,
                        size_t frame_capacity,
                        const sv_expr_record *records,
                        const sv_expr_tables *tables)
{
    uint16_t phase = 0;          /* [esp+0x16], zeroed at 0x1c00be4b */
    uint16_t phase_step;         /* [esp+0x12] */
    uint16_t flutter_index = 0;  /* [esp+0x14], zeroed at 0x1c00be52 */
    int32_t glide_target;        /* [esp+0x18] */
    int32_t glide_step;          /* [esp+0x2c] */
    size_t frame = 0;            /* edi, as an index rather than a pointer */
    const sv_expr_record *record;

    if (state == NULL || frames == NULL || records == NULL || tables == NULL
            || tables->flutter == NULL || tables->lfo == NULL
            || tables->voices == NULL || tables->source_map == NULL) {
        return SV_EXPR_E_ARG;
    }
    /* 0x1c00be97 divides by this before doing anything else. DIVERGENCE:
     * the original faults. */
    if (state->glide_rate == 0) {
        return SV_EXPR_E_ARG;
    }

    phase_step = sv_vibrato_step(state->vibrato_rate);
    glide_target = sv_wrap32((int64_t)state->pitch_scaled * 256); /* 0x1c00be8a */
    glide_step = 0x10000 / (int32_t)state->glide_rate;            /* 0x1c00be97 */

    /* 0x1c00bea0: the walk starts at the SECOND record. */
    for (record = records + 1; record->code != SV_EXPR_RECORD_END; ++record) {
        uint16_t count;
        int status = sv_expr_commands(state, record->commands, tables,
                                      &phase_step, &glide_target, &glide_step);
        if (status != SV_EXPR_OK) {
            return status;
        }

        /* 0x1c00bfe1..0x1c00bff0: (duration + 4) / 8, tested as a word. */
        count = (uint16_t)(((uint32_t)record->duration + 4u) >> 3);

        for (; count != 0; count = (uint16_t)(count - 1)) {
            sv_frame *f;
            uint16_t raw_pitch;   /* ecx, the pitch BEFORE the rescale */
            int32_t lfo;          /* [esp+0x24] */
            int32_t glide_whole;  /* ebx */
            int modulated = 0;    /* [esp+0x20] */

            /* DIVERGENCE: the original has no capacity argument and walks off
             * the end of the array if the records ask for more frames than
             * were allocated. */
            if (frame >= frame_capacity) {
                return SV_EXPR_E_FRAMES;
            }
            f = &frames[frame];

            /* 1. Rescale. 0x1c00c009: `shl eax,17` on a 32-bit register, so a
             * pitch of 0x8000 or more loses its top bit before the divide.
             * DIVERGENCE: a zero divisor faults in the original. */
            raw_pitch = f->pitch;
            if (state->pitch_divisor == 0) {
                return SV_EXPR_E_ARG;
            }
            f->pitch = (uint16_t)(((uint32_t)raw_pitch << 17)
                                  / (uint32_t)state->pitch_divisor);

            /* 2. The LFO. 0x1c00c014: a 16-bit add, a 13-bit wrap, and an
             * even byte offset into a 128-entry int16 table. */
            phase = (uint16_t)((uint16_t)(phase + phase_step) & 0x1fffu);
            lfo = sv_read_i16(tables->lfo + (((size_t)phase >> 5) & ~(size_t)1));

            /* 3. The glide. 0x1c00c039: up by a rate-derived step while the
             * last pitch command raised the pitch, down by a fixed 0x1999
             * otherwise, clamped at the target either way. */
            if (state->glide_rising == 1) {
                state->glide = sv_wrap32((int64_t)state->glide
                                         + (int64_t)glide_step);
                if (glide_target < state->glide) {
                    state->glide = glide_target;
                }
            } else {
                state->glide = sv_wrap32((int64_t)state->glide - 0x1999);
                if (glide_target > state->glide) {
                    state->glide = glide_target;
                }
            }
            glide_whole = sv_sar32(state->glide, 8);

            /* 4a. Vibrato, 0x1c00c074. Gated on the glide as well as the
             * depth, so a voice that has not started gliding has none. */
            if (state->vibrato_depth != 0 && glide_whole != 0) {
                const uint16_t pitch = f->pitch;
                int32_t v;

                modulated = 1;
                v = sv_sar32(sv_mul32((int32_t)pitch, lfo), 15);
                v = sv_sar32(sv_mul32(v, glide_whole), 8);
                v = sv_sar32(sv_mul32((int32_t)state->vibrato_depth, v), 10);
                /* 0x1c00c0ad: `addw`, 16 bits. */
                f->pitch = (uint16_t)((uint16_t)v + pitch);
            }

            /* 4b. Tremolo, 0x1c00c0b4. The LFO is re-centred on 0x7ff8 --
             * not 0x8000 -- before it scales the two amplitudes. */
            if (state->tremolo_depth != 0 && glide_whole != 0) {
                int32_t t;
                int32_t voicing;
                int32_t aspiration;

                modulated = 1;
                t = sv_wrap32((int64_t)lfo - 0x7ff8);
                t = sv_sar32(sv_mul32(t, (int32_t)state->tremolo_depth), 16);
                t = sv_sar32(sv_mul32(t, glide_whole), 8);
                t = sv_wrap32((int64_t)t * 2); /* 0x1c00c0e7, `lea (,edx,2)` */

                aspiration = sv_wrap32((int64_t)f->amp_aspiration + (int64_t)t);
                voicing = sv_wrap32((int64_t)f->amp_voicing + (int64_t)t);
                /* 0x1c00c0f7..0x1c00c11f: clamped to 0..0x80, voicing first. */
                if (voicing >= 0x80) {
                    voicing = 0x80;
                }
                if (voicing <= 0) {
                    voicing = 0;
                }
                f->amp_voicing = (uint8_t)voicing;
                if (aspiration >= 0x80) {
                    aspiration = 0x80;
                }
                if (aspiration <= 0) {
                    aspiration = 0;
                }
                f->amp_aspiration = (uint8_t)aspiration;
            }

            /* 4c. The glottal-source filter index, 0x1c00c122: the LOW nibble
             * of bandwidth_f3_source, clamped to 0..0xf, high nibble kept. */
            if (state->source_depth != 0 && glide_whole != 0) {
                const uint8_t packed = f->bandwidth_f3_source;
                int32_t s;

                modulated = 1;
                s = sv_sar32(sv_mul32((int32_t)state->source_depth, lfo), 15);
                s = sv_sar32(sv_mul32(s, glide_whole), 8);
                s = sv_wrap32((int64_t)s + (int64_t)(packed & 0x0f));
                if (s >= 0xf) {
                    s = 0xf;
                }
                if (s <= 0) {
                    s = 0;
                }
                f->bandwidth_f3_source = (uint8_t)((packed & 0xf0)
                                                   + (uint8_t)s);
            }

            /* 0x1c00c16f: if none of the three fired, the LFO goes back to
             * phase zero -- so the waveform restarts with the next voice that
             * has any modulation at all, rather than running free. */
            if (!modulated) {
                phase = 0;
            }

            /* 5. Flutter, 0x1c00c17d. Note the PRE-rescale pitch: `ecx` still
             * holds what the frame carried on entry. */
            if (state->flutter_depth != 0) {
                int32_t jitter = (int32_t)(int8_t)tables->flutter[flutter_index];

                jitter = sv_mul32(jitter, (int32_t)state->flutter_depth);
                jitter = sv_mul32(jitter, (int32_t)raw_pitch);
                jitter = sv_sar32(jitter, 13);
                f->pitch = (uint16_t)sv_wrap32((int64_t)f->pitch
                                               + (int64_t)jitter);
            }
            /* 0x1c00c1a3 and 0x1c00c28a: the original keeps a pointer and a
             * counter and resets both together, which one index reproduces.
             * Neither is reset between records: the flutter runs across the
             * whole utterance. */
            flutter_index = (uint16_t)(flutter_index + 1);
            if (flutter_index >= SV_EXPR_FLUTTER_BYTES) {
                flutter_index = 0;
            }

            /* 6. The source pair. */
            sv_expr_source(state, tables, f, raw_pitch);

            ++frame;
        }
    }
    return SV_EXPR_OK;
}
