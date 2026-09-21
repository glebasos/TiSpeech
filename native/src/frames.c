#include "tispeech/frames.h"

/* Reconstruction of TIBASE32!FUN_1c00402c, the parameter-frame stage.
 *
 * Every store below cites the address of the instruction it came from. The
 * arithmetic is 16-bit with 32-bit DX:AX intermediates throughout, exactly as
 * the original; widening it changes results, so the narrowing is written out
 * rather than left to the compiler's promotions. */

/* ------------------------------------------------------------------------ */
/* 16-bit arithmetic helpers, in the style of src/dsp.c.                     */
/* ------------------------------------------------------------------------ */

static int16_t narrow(uint32_t value)
{
    uint32_t low = value & 0xffffu;
    return (int16_t)((int32_t)low - ((low & 0x8000u) ? 65536 : 0));
}

static uint16_t high(uint32_t value)
{
    return (uint16_t)(value >> 16);
}

/* `ror eax, 0x10` after a `movzx eax, word ...`: the table word becomes the
 * high half of a 32-bit ramp value and the fraction is cleared. */
static uint32_t widen(uint16_t value)
{
    return (uint32_t)value << 16;
}

/* `sub ax, <current high word>` then `imul word ptr [ebx + 0x2fa]`:
 * a signed 16x16 product placed in DX:AX and stored as one dword.
 * 0x1c004637, 0x1c00465a, 0x1c004698, ... */
static uint32_t ramp_step(uint16_t target, uint32_t current, int16_t scale)
{
    int16_t delta = narrow((uint32_t)target - (uint32_t)high(current));
    return (uint32_t)((int32_t)delta * (int32_t)scale);
}

/* ------------------------------------------------------------------------ */
/* Table reads.                                                              */
/*                                                                           */
/* The coefficient tables and the amplitude table are held as byte blobs (see */
/* sv_frame_tables) and read here at the byte offset the original computes,   */
/* so that the original's unscaled amplitude index and its unbounded row      */
/* indices are reproduced rather than corrected.                              */
/* ------------------------------------------------------------------------ */

static uint16_t read_u16(const uint8_t *blob, unsigned offset)
{
    return (uint16_t)((uint16_t)blob[offset]
                      | ((uint16_t)blob[offset + 1] << 8));
}

/* `movzx edi, byte ptr [esi+N]` then `mov ax, word ptr [edi + 0x1c001308]`
 * with NO scaling of edi. 0x1c00485b, 0x1c004882, 0x1c0048a2, 0x1c0048c4. */
static uint16_t amplitude_at(const sv_frame_tables *t, uint8_t index)
{
    return read_u16(t->amplitude, (unsigned)index);
}

/* `edi = nibble << 1`, then `ax = frequency << 5`, then `or di, ax`: a byte
 * offset of frequency * 32 + nibble * 2, i.e. a 16-word row per frequency.
 * The OR is an ADD because the fields are disjoint (nibble < 16).
 * 0x1c00468f/0x1c0046b8 and the five lookups that copy it. */
static unsigned row_offset(unsigned base, unsigned frequency, unsigned nibble)
{
    return base + (frequency << 5) + (nibble << 1);
}

/* ------------------------------------------------------------------------ */
/* Frame timing. TIBASE32 0x1c0044f8..0x1c00452f.                            */
/*                                                                           */
/*   mov  eax, [ebx+0x48]     ; only AX participates                         */
/*   mul  word [0x1c0120e8]   ; DX:AX = AX * 150                             */
/*   div  word [0x1c0120ea]   ; AX    = DX:AX / 60                           */
/*   xor  dx, dx                                                             */
/*   movzx cx, byte [esi+0xa] ; shl cx, 1                                    */
/*   div  cx                                                                 */
/*   shr  ax, 1                                                              */
/*                                                                           */
/* The two constants live in .data at VA 0x1c0120e8 and 0x1c0120ea and are    */
/* 150 and 60 in this build; they are inlined here because they are scalars,  */
/* not a table, and the build-time extractor checks them.                     */
/* ------------------------------------------------------------------------ */

#define SV_FRAME_RATE_NUMERATOR 150u
#define SV_FRAME_RATE_DENOMINATOR 60u

uint16_t sv_frame_length(uint16_t sample_rate, uint8_t duration)
{
    uint32_t scaled;
    uint16_t quotient;
    uint16_t divisor = (uint16_t)((uint16_t)duration << 1);

    if (duration == 0) {
        return 0; /* the original divides by zero here */
    }
    scaled = (uint32_t)sample_rate * SV_FRAME_RATE_NUMERATOR;
    quotient = (uint16_t)(scaled / SV_FRAME_RATE_DENOMINATOR);
    quotient = (uint16_t)(quotient / divisor);
    return (uint16_t)(quotient >> 1);
}

/* ------------------------------------------------------------------------ */
/* The once-per-utterance reset. TIBASE32 0x1c00405d..0x1c00411b.            */
/* ------------------------------------------------------------------------ */

void sv_frame_reset(sv_frame_state *s)
{
    int i;

    s->restart = 0; /* 0x1c00405d */

    /* 0x1c004068..0x1c004098: the nine filter slots at 0x1b2 + 20*n get their
     * two history words cleared -- one dword store each, so the `a` and `b`
     * ramps are deliberately left alone. Seven of the slots are
     * sv_dsp_state's; the other two are 0x22a and 0x252, which nothing ever
     * reads. */
    for (i = 0; i < SV_DSP_FILTER_COUNT; ++i) {
        s->dsp.filters[i].previous = 0;
        s->dsp.filters[i].previous2 = 0;
    }
    for (i = 0; i < 2; ++i) {
        s->unused_filters[i].previous = 0;
        s->unused_filters[i].previous2 = 0;
    }

    s->frication_step = 0;  /* 0x1c00409e, [0x28a] */
    s->aspiration_step = 0; /* 0x1c0040a4, [0x282] */
    s->shape_step = 0;      /* 0x1c0040aa, [0x292] */
    /* 0x1c0040b0 clears the whole dword at 0x2aa, both halves. */
    s->voice_frac = 0;
    s->dsp.previous_voice = 0;
    s->dsp.phase = 0; /* 0x1c0040b6, [0x2fc] */

    s->noise_left = 0x3ff;        /* 0x1c0040bc */
    s->dsp.phase_step = 0x20000u; /* 0x1c0040c5 */

    /* 0x1c0040d5: [0xfa] = [0xf6] + 0x80, i.e. four frames into the array.
     * sv_frame_apply() steps once more before applying, so the first frame
     * that ever reaches the kernel is frames[5]; the lead-in is written by the
     * language module's frame emitter.
     *
     * The scan at 0x1c0040e8, which would walk forward to the first frame with
     * event bit 1 set, is dead code: 0x1c0040e6 is an unconditional `jmp`
     * over it and nothing else targets it. */
    s->frame_index = 4;

    s->elapsed = 0x40;      /* 0x1c0040f9 */
    s->noise_index = 0;     /* 0x1c004103, [0x2e4] = 0x1c001570 */
    s->scratch_2e8 = s->scratch_2c0; /* 0x1c00410f */
}

/* ------------------------------------------------------------------------ */
/* Per-frame state fill. TIBASE32 0x1c0044cb..0x1c004969.                    */
/* ------------------------------------------------------------------------ */

/* Every table index the frame carries is a uint8 that the original scales and
 * uses unchecked. Inside the extracted blobs that is harmless and reproduced
 * exactly; past them the original would read its own .text or the string
 * constant that follows the glottal pointer table, which is not data under
 * any reading. Checking up front, before any state is written, means a
 * rejected frame leaves the state alone -- the original would have written
 * frame_length, interp_scale and elapsed before faulting, but a fault is not
 * a behaviour worth reproducing. */
static int frame_in_range(const sv_frame *f)
{
    int n;

    for (n = 0; n < 3; ++n) { /* F1..F3, cascade rows */
        if (f->formant_freq[n] >= SV_CASCADE_ROWS_ADDRESSABLE) {
            return 0;
        }
    }
    if (f->formant_freq[3] >= SV_PARALLEL_ROWS_ADDRESSABLE
        || f->formant_freq[4] >= SV_PARALLEL_ROWS_ADDRESSABLE
        || f->frication_freq >= SV_PARALLEL_ROWS_ADDRESSABLE) {
        return 0;
    }
    if (f->source_index_a >= SV_GLOTTAL_TABLES
        || f->source_index_b >= SV_GLOTTAL_TABLES) {
        return 0;
    }
    return 1;
}

int sv_frame_apply(sv_frame_state *s, const sv_frame_tables *t,
                   const sv_frame *frames)
{
    uint32_t index = s->frame_index + 1; /* 0x1c0044cc: esi = [0xfa] + 0x20 */
    const sv_frame *f = &frames[index];
    const sv_frame *back1 = f - 1; /* [esi-0x10], [esi-0xf] */
    const sv_frame *back2 = f - 2; /* [esi-0x35] */
    uint16_t level;
    unsigned bandwidth;
    unsigned row;
    uint16_t elapsed_step;

    if ((s->flags & SV_FRAME_FLAG_NO_EVENTS) == 0) {
        /* 0x1c004536..0x1c004605 report frame events through FUN_1c00498f.
         * That block runs only when flags bit 6 is clear and is not
         * reconstructed; refusing is better than dropping the events. */
        return SV_FRAMES_E_EVENTS;
    }
    if (f->formant_freq[0] == SV_FRAME_END) { /* 0x1c0044d5 */
        return SV_FRAMES_END;
    }
    if (back2->marker == 1 || back2->marker == 2) { /* 0x1c0044de, 0x1c0044e8 */
        return SV_FRAMES_END;
    }
    s->frame_index = index; /* 0x1c0044f2, before anything else is computed */

    if (!frame_in_range(f)) {
        return SV_FRAMES_E_RANGE;
    }

    /* 0x1c0044f8..0x1c00452f. The original divides by zero for a duration of
     * zero and overflows `idiv` for a frame shorter than three samples; both
     * raise #DE rather than producing a value, so neither is translated. */
    if (f->duration == 0) {
        return SV_FRAMES_E_LENGTH;
    }
    s->frame_length = sv_frame_length(s->sample_rate, f->duration);
    if (s->frame_length < 3) {
        return SV_FRAMES_E_LENGTH;
    }
    s->interp_scale = (int16_t)(0x10000 / (int32_t)(int16_t)s->frame_length);

    /* 0x1c004609: elapsed += 64000 * frame_length / rate, sign-extended
     * from AX. The `div` faults if the quotient will not fit in AX, which for
     * a frame_length under the sample rate it always does. */
    elapsed_step = (uint16_t)(((uint32_t)64000u * s->frame_length)
                              / (uint32_t)s->sample_rate);
    s->elapsed += (uint32_t)(int32_t)narrow(elapsed_step);

    /* 0x1c004627..0x1c00466f: the glottal source shaping filter at 0x23e.
     * Its two coefficients come straight from a nibble, with no frequency
     * row, and both are ramped. */
    bandwidth = f->bandwidth_f3_source & 0x0fu;
    {
        sv_dsp_filter *filter = &s->dsp.filters[SV_DSP_SOURCE_FILTER];
        filter->a.step = ramp_step(
            read_u16(t->resonator, SV_SOURCE_A_OFF + (bandwidth << 1)),
            filter->a.value, s->interp_scale);
        filter->b.step = ramp_step(
            read_u16(t->resonator, SV_SOURCE_B_OFF + (bandwidth << 1)),
            filter->b.value, s->interp_scale);
    }

    /* 0x1c004676: phase_step = pitch << 9, via `ror eax,16; shr eax,7`. */
    s->dsp.phase_step = widen(f->pitch) >> 7;

    /* 0x1c004686..0x1c004791: F1, F2, F3. `b` is a function of bandwidth
     * alone; `a` is indexed [frequency][bandwidth] with a 16-word row. Both
     * are ramped over the frame, and `b` is computed first in each triple. */
    {
        static const int cascade_filter[3] = {
            SV_DSP_FORMANT1, SV_DSP_FORMANT2, SV_DSP_FORMANT3
        };
        unsigned nibble[3];
        int n;

        nibble[0] = (unsigned)f->bandwidth_f1f2 >> 4;        /* 0x1c004686 */
        nibble[1] = (unsigned)f->bandwidth_f1f2 & 0x0fu;     /* 0x1c0046e2 */
        nibble[2] = (unsigned)f->bandwidth_f3_source >> 4;   /* 0x1c00473b */
        for (n = 0; n < 3; ++n) {
            sv_dsp_filter *filter = &s->dsp.filters[cascade_filter[n]];
            bandwidth = nibble[n];
            row = f->formant_freq[n];
            filter->b.step = ramp_step(
                read_u16(t->resonator, SV_CASCADE_B_OFF + (bandwidth << 1)),
                filter->b.value, s->interp_scale);
            filter->a.step = ramp_step(
                read_u16(t->resonator,
                         row_offset(SV_CASCADE_A_OFF, row, bandwidth)),
                filter->a.value, s->interp_scale);
        }
    }

    /* 0x1c004798..0x1c004839: F4, F5 and the frication filter. These are
     * assigned, not ramped -- the sample loop is told not to interpolate
     * them, which matches sv_dsp_sample() passing interpolate = 0 for
     * SV_DSP_FORMANT4, SV_DSP_ASPIRATION_FILTER and
     * SV_DSP_FRICATION_FILTER. `b` is again written first. */
    {
        static const int parallel_filter[3] = {
            SV_DSP_FORMANT4,
            SV_DSP_ASPIRATION_FILTER, /* 0x202: the fifth cascade resonator */
            SV_DSP_FRICATION_FILTER
        };
        unsigned nibble[3];
        unsigned freq[3];
        int n;

        nibble[0] = (unsigned)f->bandwidth_f4f5 >> 4;         /* 0x1c004798 */
        nibble[1] = (unsigned)f->bandwidth_f4f5 & 0x0fu;      /* 0x1c0047cf */
        nibble[2] = (unsigned)f->bandwidth_frication >> 4;    /* 0x1c004802 */
        freq[0] = f->formant_freq[3];
        freq[1] = f->formant_freq[4];
        freq[2] = f->frication_freq;
        for (n = 0; n < 3; ++n) {
            sv_dsp_filter *filter = &s->dsp.filters[parallel_filter[n]];
            bandwidth = nibble[n];
            filter->b.value = widen(
                read_u16(t->resonator, SV_PARALLEL_B_OFF + (bandwidth << 1)));
            filter->a.value = widen(
                read_u16(t->resonator,
                         row_offset(SV_PARALLEL_A_OFF, freq[n], bandwidth)));
        }
    }

    /* 0x1c004839: crossfade between the two glottal waveforms. 0x7f80 is
     * 0xff << 7, so the two weights always sum to a full scale. */
    s->dsp.mix_a = narrow((uint32_t)f->source_mix << 7);
    s->dsp.mix_b = narrow(0x7f80u - ((uint32_t)f->source_mix << 7));

    /* 0x1c004857: voicing is the only level that ramps. */
    level = amplitude_at(t, f->amp_voicing);
    s->dsp.voicing.step = ramp_step(level, s->dsp.voicing.value,
                                    s->interp_scale);

    /* 0x1c00487e: aspiration. One dword, `level << 16`, stored three times:
     * to the per-sample scratch slot at 0x27e and to both half-cycle levels
     * at 0x296 and 0x29a, each of which zeroes its own fraction. */
    level = amplitude_at(t, f->amp_aspiration);
    s->aspiration_frac = 0;
    s->aspiration_level = level;
    s->level_frac[0] = 0;
    s->dsp.aspiration_negative = narrow(level);
    s->level_frac[1] = 0;
    s->dsp.aspiration_positive = narrow(level);

    /* 0x1c00489e: frication, likewise. */
    level = amplitude_at(t, f->amp_frication);
    s->frication_frac = 0;
    s->frication_level = level;
    s->level_frac[2] = 0;
    s->dsp.frication_negative = narrow(level);
    s->level_frac[3] = 0;
    s->dsp.frication_positive = narrow(level);

    /* 0x1c0048be: the frication shaping term is an offset from 0x101d, not a
     * level. `xor eax,eax` then `sub ax, ...` then `add ax, 0x101d` keeps the
     * whole computation in AX with EAX's high half already zero, so the
     * `ror eax,16` that follows widens it with a zero fraction. */
    s->shape_frac = 0;
    s->dsp.frication_shape =
        narrow(0x101du - (uint32_t)amplitude_at(t, f->frication_shape));

    /* 0x1c0048d8: above a voicing level of 0x3c the positive half-cycle gets
     * much less aspiration and frication than the negative one, which is how
     * the original modulates noise with the glottal cycle. The shifts act on
     * the widened 32-bit values, so they are exact on the level word. */
    if (f->amp_voicing >= 0x3c) {
        uint32_t widened;

        widened = (widen((uint16_t)s->dsp.aspiration_negative)
                   | s->level_frac[0]) >> 3;
        s->level_frac[1] = (uint16_t)widened;
        s->dsp.aspiration_positive = narrow(high(widened));

        widened = (widen((uint16_t)s->dsp.frication_negative)
                   | s->level_frac[2]) >> 4;
        s->level_frac[3] = (uint16_t)widened;
        s->dsp.frication_positive = narrow(high(widened));
    }

    /* 0x1c0048fc: when this frame and the one before it are both essentially
     * silent, the resonator histories are cleared so that the next voiced
     * frame does not ring with the previous one's decay. One dword store per
     * filter, so only the history words. The frication filter and the fifth
     * resonator are deliberately left alone. */
    if (f->amp_voicing <= 0x0e && f->amp_aspiration <= 0x0e
        && back1->amp_voicing <= 0x0e && back1->amp_aspiration <= 0x0e) {
        static const int cleared[5] = {
            SV_DSP_FORMANT1, SV_DSP_FORMANT2, SV_DSP_FORMANT3,
            SV_DSP_FORMANT4, SV_DSP_SOURCE_FILTER
        };
        int n;
        for (n = 0; n < 5; ++n) {
            s->dsp.filters[cleared[n]].previous = 0;
            s->dsp.filters[cleared[n]].previous2 = 0;
        }
    }

    /* 0x1c004934: the two glottal waveforms this frame mixes. */
    s->source_a = t->glottal[f->source_index_a];
    s->source_b = t->glottal[f->source_index_b];

    /* 0x1c00495b */
    s->frame_left = s->frame_length;
    return SV_FRAMES_OK;
}

/* ------------------------------------------------------------------------ */
/* The render loop. TIBASE32!FUN_1c00402c as a whole.                        */
/* ------------------------------------------------------------------------ */

int sv_frame_render(sv_frame_state *s, const sv_frame_tables *t,
                    const sv_frame *frames, uint8_t *out, uint16_t count)
{
    sv_dsp_tables kernel;
    uint16_t written = 0;
    /* Where the next iteration picks up. The original expresses this as three
     * entry points into one block of code; naming them is the only way to
     * write it as structured C without changing the order of the counters. */
    enum { RESUME_TICK, LOAD_FRAME, SAMPLE } next;

    if ((s->flags & SV_FRAME_FLAG_NO_EVENTS) == 0) {
        return SV_FRAMES_E_EVENTS;
    }
    if (count == 0) {
        /* The original's `dec word [ebx+0x2f4]` at 0x1c004493 wraps a zero
         * count to 0xffff and writes 65536 samples into the caller's buffer.
         * That is a defect in its contract, not a behaviour to reproduce. */
        return SV_FRAMES_E_COUNT;
    }

    kernel.output_curve = t->output_curve;
    s->samples_left = count; /* 0x1c00403f */

    /* 0x1c004053: a non-zero restart flag takes the reset path, which ends
     * with a jump straight to the frame loader. A zero one re-enters at
     * 0x1c0044a0 -- NOT at the sample loop. That is one instruction past the
     * sample-count decrement, so a continuation call first pays the noise and
     * frame counters owed by the previous call's last sample, which the
     * previous call skipped when it returned out of 0x1c00449a. Starting a
     * continuation at the sample loop instead would silently stretch every
     * buffer boundary by one sample. */
    if (s->restart != 0) {
        sv_frame_reset(s);  /* 0x1c00405d..0x1c00411b */
        next = LOAD_FRAME;  /* 0x1c00411b: jmp 0x1c0044cb */
    } else {
        next = RESUME_TICK; /* 0x1c004057: je 0x1c0044a0 */
    }

    for (;;) {
        if (next == LOAD_FRAME) {
            int applied = sv_frame_apply(s, t, frames); /* 0x1c0044cb */
            if (applied < 0) {
                return applied;
            }
            if (applied == SV_FRAMES_END) {
                /* 0x1c00496e: pad the rest of the buffer with silence. The
                 * `rep stosb` counts down a copy of [0x2f4] in CX, so the
                 * field itself keeps its value. */
                uint16_t remaining = s->samples_left;
                while (remaining-- != 0) {
                    out[written++] = 0x80;
                }
                s->speaking = 0; /* 0x1c00497c */
                return SV_FRAMES_END;
            }
            next = SAMPLE; /* 0x1c004969: jmp 0x1c004120 */
        }

        if (next == SAMPLE) {
            /* 0x1c004120..0x1c004493, already verified as src/dsp.c. The
             * kernel consumes exactly two noise samples per output sample;
             * the original advances [0x2e4] by four bytes inside the loop. */
            int16_t noise_a = t->noise[s->noise_index];
            int16_t noise_b = t->noise[s->noise_index + 1];
            s->noise_index += 2;
            kernel.source_a = s->source_a;
            kernel.source_b = s->source_b;
            out[written++] = sv_dsp_sample(&s->dsp, &kernel, noise_a, noise_b);

            if (--s->samples_left == 0) { /* 0x1c004493 */
                return SV_FRAMES_OK;
            }
        }

        /* 0x1c0044a0: the resume point. */
        if (--s->noise_left == 0) {
            s->noise_left = 0x3ff; /* 0x1c0044a9 */
            s->noise_index = 0;    /* 0x1c0044b2 */
        }
        next = (--s->frame_left == 0) ? LOAD_FRAME : SAMPLE; /* 0x1c0044be */
    }
}

/* ------------------------------------------------------------------------ */
/* Phoneme name lookup. TIBASE32 0x1c004c85..0x1c004d29.                     */
/* ------------------------------------------------------------------------ */

uint8_t sv_phoneme_lookup(const sv_phoneme_names *names, const char *text,
                          unsigned *consumed)
{
    int index;
    const uint8_t *list;
    uint8_t second;
    uint8_t probe;
    uint8_t key;
    unsigned taken = 1;
    unsigned parity;

    /* 0x1c004c85: first character selects the list; anything outside
     * ' '..'[' falls back to entry 1. The compare is signed, so a high-bit
     * character also lands on entry 1. */
    index = (int)(signed char)text[0] - 0x20;
    if (index < 0 || index > 0x3b) {
        index = 1;
    }
    list = names->lists[index]; /* 0x1c004c99 */
    if (list == 0) {
        /* Entry 0x3b ('[') is a null pointer in the original, which
         * dereferences it. Refusing is the only sane translation. */
        if (consumed) {
            *consumed = taken;
        }
        return SV_PHONEME_INVALID;
    }

    /* 0x1c004caa: a run of 'H' after the first character is folded down to
     * its parity, which is what lets "HH" name one phoneme while "HHH" does
     * not. `parity` is 1 when an odd number of 'H's follows. */
    second = (uint8_t)text[1];
    if (second != 'H') {
        parity = 1; /* 0x1c004cae: not an H at all, offer the character */
    } else {
        const char *scan = text + 2;
        parity = 0;
        for (;;) { /* 0x1c004cc3 */
            parity ^= 1u;
            if (*scan != 'H') {
                break;
            }
            ++scan;
        }
    }
    /* 0x1c004cd2: `cl = 0xff + carry`, then AND with the second character. */
    key = (uint8_t)(second & (parity ? 0xffu : 0x00u));

    /* 0x1c004cda: the first pair is checked before the loop, so a list whose
     * head matches wins even though the head is the one-character entry. */
    probe = list[0];
    if (key != probe) {
        while (list[0] != 0) { /* 0x1c004ce4 */
            list += 2;
            if (list[0] == key) {
                break;
            }
        }
    }

    if (list[1] == SV_PHONEME_INVALID) { /* 0x1c004cf5 */
        /* 0x1c004cfd: an unknown two-character name still swallows the
         * second character when the list head was not the empty entry. AL
         * still holds the ORIGINAL head byte here, not the byte the scan
         * stopped on, which is why this tests `probe`. */
        if (probe != 0) {
            taken = 2;
        }
    } else if (list[0] != 0) { /* 0x1c004d1c */
        taken = 2;
    }

    if (consumed) {
        *consumed = taken;
    }
    return list[1];
}
