#ifndef TISPEECH_FRAMES_H
#define TISPEECH_FRAMES_H

#include <stdint.h>

#include "tispeech/dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native reconstruction of TIBASE32's parameter-frame stage: the code that
 * turns a stream of 32-byte parameter frames into the coefficients, ramps and
 * excitation-table selections that `sv_dsp_sample()` consumes, and drives the
 * kernel for a requested number of output samples.
 *
 * Source: TIBASE32!FUN_1c00402c, the function that contains the already
 * verified sample loop at 0x1c004120..0x1c004493. Its parts are
 *
 *   0x1c00402c..0x1c00405d   entry: latch the sample count, test `restart`
 *   0x1c00405d..0x1c00411b   the once-per-utterance reset
 *   0x1c004120..0x1c004493   the sample loop  (src/dsp.c, already verified)
 *   0x1c004493..0x1c0044cb   per-sample bookkeeping: counters, noise wrap
 *   0x1c0044cb..0x1c004969   per-frame state fill
 *   0x1c00496e..0x1c00498e   end of utterance: pad the rest with silence
 *
 * This stage does NOT produce the frames. Frame generation is done by the
 * LANGUAGE module, through vtable slot +0x08 of the `sv_language` descriptor
 * (TIENG32 0x1C204690), driven by TIBASE32!FUN_1c005840. That is not
 * reconstructed; see REVERSING.md. */

/* ------------------------------------------------------------------------ */
/* The parameter frame.                                                      */
/*                                                                           */
/* 32 bytes. Confirmed from TIBASE32!FUN_1c00c420 @0x1c00c49b, which sizes    */
/* the array with `calloc(nframes, 0x20)` after computing `nframes` as        */
/* 4 + sum over phonemes of (duration + 4) / 8, and from the field reads in   */
/* 0x1c0044cb..0x1c004969. Fields marked "unread" are never touched by the    */
/* renderer; they may still be used by the frame generator or by the one       */
/* smoothing pass still unreconstructed (0x1c00be40). The two pitch passes,    */
/* 0x1c00cd40 and 0x1c00de60, are in src/smoothing.c and touch only `pitch`.   */
/* ------------------------------------------------------------------------ */

typedef struct {
    /* +0x00..+0x04  formant frequency indices F1..F5.
     * F1/F2/F3 index the cascade table, F4/F5 the parallel one; both are row
     * indices into a 16-word-per-row table whose column is the bandwidth
     * nibble. 0x1c0046b4, 0x1c00470c, 0x1c004769, 0x1c0047b3, 0x1c0047e6. */
    uint8_t formant_freq[5];
    uint8_t unread_05;
    /* +0x06 high nibble F1 bandwidth, low nibble F2 bandwidth. 0x1c004686 */
    uint8_t bandwidth_f1f2;
    /* +0x07 high nibble F3 bandwidth, low nibble glottal-source filter index
     * (which selects straight into source_a/source_b). 0x1c004627,
     * 0x1c00473b */
    uint8_t bandwidth_f3_source;
    /* +0x08 high nibble F4 bandwidth, low nibble F5 bandwidth. 0x1c004798 */
    uint8_t bandwidth_f4f5;
    /* +0x09 frication filter frequency, a parallel-table row. 0x1c00481d */
    uint8_t frication_freq;
    /* +0x0a frame length divisor; see sv_frame_length(). 0x1c00450c */
    uint8_t duration;
    /* +0x0b marker byte. Reported with event 0x3ee (0x1c004580); values 1 and
     * 2 stop the utterance when seen two frames back (0x1c0044de). */
    uint8_t marker;
    uint8_t unread_0c;
    /* +0x0d,+0x0e glottal waveform selectors, indexing the pointer table at
     * VA 0x1c013bd0. 0x1c004934, 0x1c004947 */
    uint8_t source_index_a;
    uint8_t source_index_b;
    /* +0x0f crossfade between the two glottal waveforms. 0x1c004839 */
    uint8_t source_mix;
    /* +0x10..+0x13 amplitude indices: voicing (ramped), aspiration, frication,
     * and the frication shaping term. See sv_frame_tables.amplitude — these
     * are BYTE offsets into that table, not element indices.
     * 0x1c004857, 0x1c00487e, 0x1c00489e, 0x1c0048be */
    uint8_t amp_voicing;
    uint8_t amp_aspiration;
    uint8_t amp_frication;
    uint8_t frication_shape;
    /* +0x14 high nibble frication filter bandwidth. 0x1c004802 */
    uint8_t bandwidth_frication;
    uint8_t unread_15;
    uint8_t unread_16;
    /* +0x17 event bits, gated by sv_frame_state.flags. 0x1c00457a.. */
    uint8_t events;
    /* +0x18 fundamental; phase_step = pitch << 9. 0x1c004676 */
    uint16_t pitch;
    /* +0x1a phoneme id, reported with event 0x3f0. 0x1c00454e */
    uint8_t phoneme_id;
    /* +0x1b event 0x3ef parameter. 0x1c0045d9 */
    uint8_t event_param;
    /* +0x1c event 0x3eb parameter. 0x1c0045f4 */
    uint16_t event_word;
    uint8_t unread_1e;
    uint8_t unread_1f;
} sv_frame;

/* A frame whose formant_freq[0] is 0xff terminates the stream.
 * TIBASE32!FUN_1c005840 @0x1c0058c5 writes that terminator; the renderer
 * tests it at 0x1c0044d5. */
#define SV_FRAME_END 0xffu

/* ------------------------------------------------------------------------ */
/* Static tables, all TIBASE32 data. Extracted at build time by              */
/* tools/extract_base.py; never committed.                                   */
/*                                                                           */
/* Two of these are deliberately modelled as untyped byte blobs rather than   */
/* as arrays of the type they hold. Every index the frame stage uses is a     */
/* uint8 taken straight out of the frame, and the original bounds none of     */
/* them: it scales the index by the instruction's operand size and reads. For */
/* the amplitude table it does not scale at all. Declaring these as int16_t   */
/* arrays and indexing them by element would quietly correct the original     */
/* rather than reproduce it, and the correction is not observable in the      */
/* output of a single frame — exactly the class of error the whole-state      */
/* differential in tools/verify_frames.py exists to catch.                    */
/* ------------------------------------------------------------------------ */

enum {
    /* The six resonator-coefficient tables are contiguous in TIBASE32 at
     * 0x1c002570..0x1c0036f0 and are extracted as one blob, because a formant
     * frequency past the end of its own table reads into the next one.
     * Offsets below are byte offsets into that blob. */
    SV_RESONATOR_BYTES = 0x1180,
    SV_CASCADE_A_OFF = 0x0000,  /* 0x1c002570  F1..F3 numerator, by [row][bw] */
    SV_CASCADE_B_OFF = 0x0b00,  /* 0x1c003070  F1..F3 denominator, by [bw]    */
    SV_PARALLEL_A_OFF = 0x0b20, /* 0x1c003090  F4/F5/frication, by [row][bw]  */
    SV_PARALLEL_B_OFF = 0x1120, /* 0x1c003690  F4/F5/frication, by [bw]       */
    SV_SOURCE_A_OFF = 0x1140,   /* 0x1c0036b0  glottal shaping, by [nibble]   */
    SV_SOURCE_B_OFF = 0x1160,   /* 0x1c0036d0  glottal shaping, by [nibble]   */

    SV_BANDWIDTHS = 16,
    SV_CASCADE_ROW_BYTES = 32,

    /* Rows the table actually holds ... */
    SV_CASCADE_ROWS = 88,  /* (0x1c003070 - 0x1c002570) / 32 */
    SV_PARALLEL_ROWS = 48, /* (0x1c003690 - 0x1c003090) / 32 */
    /* ... and rows still inside the extracted blob, which is as far as the
     * reconstruction can follow the original. Past these the original reads
     * its own .text, which is not data by any reading, so sv_frame_apply()
     * returns SV_FRAMES_E_RANGE instead of inventing a value. */
    SV_CASCADE_ROWS_ADDRESSABLE = 140,
    SV_PARALLEL_ROWS_ADDRESSABLE = 51,

    /* 0x1c001308. 80 uint16 levels, but see `amplitude` below: the index is a
     * byte offset, so 258 bytes are extracted to cover every uint8 value. */
    SV_AMPLITUDE_BYTES = 0x102,
    SV_AMPLITUDE_ENTRIES = 80,

    SV_GLOTTAL_TABLES = 12,  /* 0x1c013bd0, 12 pointers; entry 12 is a string */
    SV_GLOTTAL_SAMPLES = 1024,
    SV_NOISE_SAMPLES = 2048, /* 0x1c001570; only 0..2045 are ever reached */
    SV_OUTPUT_CURVE_BYTES = 401 /* 0x1c0010fe, as in sv_dsp_tables */
};

typedef struct {
    /* 0x1c002570, SV_RESONATOR_BYTES bytes. Read through the SV_*_OFF offsets
     * above as little-endian int16. `a` is the numerator of the y[n-1] term
     * scaled by 2^14, `b` the y[n-2] term scaled by 2^15 and negated, matching
     * sv_dsp_sample()'s shifts. */
    const uint8_t *resonator;

    /* 0x1c001308, SV_AMPLITUDE_BYTES bytes, read as an UNALIGNED little-endian
     * uint16 at the byte offset held in the frame. The original is
     *
     *     movzx edi, byte ptr [esi + 0x10]     ; 0x1c004857
     *     mov   ax,  word ptr [edi + 0x1c001308]
     *
     * with no `shl edi, 1` — unlike every other table lookup in this function,
     * which does scale. An odd index therefore reads a word straddling two
     * entries, and index 80 and up runs past the 80 real levels into the zero
     * padding and then into the table that follows. Both are reproduced. */
    const uint8_t *amplitude;

    /* SV_GLOTTAL_TABLES waveforms of SV_GLOTTAL_SAMPLES samples each, from the
     * pointer table at 0x1c013bd0. */
    const int16_t *const *glottal;
    const int16_t *noise;            /* SV_NOISE_SAMPLES */
    const uint8_t *output_curve;     /* SV_OUTPUT_CURVE_BYTES, as sv_dsp_tables */
} sv_frame_tables;

/* ------------------------------------------------------------------------ */
/* Renderer state.                                                           */
/*                                                                           */
/* The original keeps everything in one ~0x400-byte block addressed through   */
/* EBX. `dsp` covers the part sv_dsp_sample() reads; the rest are the fields  */
/* the frame stage owns. Original offsets are given for each.                 */
/* ------------------------------------------------------------------------ */

typedef struct {
    sv_dsp_state dsp;

    /* Filter slots the reset zeroes but nothing ever reads: original offsets
     * 0x22a and 0x252. Modelled only so a whole-block differential is clean.
     * 0x1c00408c, 0x1c004098 */
    sv_dsp_filter unused_filters[2];

    /* The original stores the aspiration, frication and frication-shape
     * levels as 32-bit ramps whose HIGH word is the live value, laid out
     * exactly like sv_dsp_ramp but 16 bits apart from the fields sv_dsp_state
     * already names. The step halves are zeroed by the reset and never
     * written again, so these three never actually ramp.
     *
     *   0x27e frac / 0x280 value / 0x282 step   aspiration
     *   0x286 frac / 0x288 value / 0x28a step   frication
     *   0x28e frac / 0x290 value / 0x292 step   frication shape
     *
     * 0x290 is sv_dsp_state.frication_shape, so only its fraction appears
     * here. 0x280 and 0x288 are seeded by the frame stage and then used as
     * per-sample scratch by the kernel (see REVERSING.md). */
    uint16_t aspiration_frac;   /* 0x27e */
    uint16_t aspiration_level;  /* 0x280 */
    uint32_t aspiration_step;   /* 0x282 */
    uint16_t frication_frac;    /* 0x286 */
    uint16_t frication_level;   /* 0x288 */
    uint32_t frication_step;    /* 0x28a */
    uint16_t shape_frac;        /* 0x28e */
    uint32_t shape_step;        /* 0x292 */

    /* Fractions of the four level words sv_dsp_state holds at 0x298, 0x29c,
     * 0x2a0 and 0x2a4: the frame stage writes each as one dword and so zeroes
     * the two bytes below. Nothing ever reads them. */
    uint16_t level_frac[4]; /* 0x296, 0x29a, 0x29e, 0x2a2 */

    /* Fraction of sv_dsp_state.previous_voice, which lives at 0x2ac as the
     * high half of the dword at 0x2aa. The reset clears the whole dword
     * (0x1c0040b0), so the fraction has to be modelled to reproduce it. */
    uint16_t voice_frac; /* 0x2aa */

    /* The two glottal waveforms this frame crossfades. The original keeps
     * them in the state block at 0x2d8 and 0x2dc; sv_frame_tables carries them
     * instead, so sv_frame_render() copies them across per sample. */
    const int16_t *source_a; /* 0x2d8 */
    const int16_t *source_b; /* 0x2dc */

    uint16_t sample_rate;  /* 0x48,  words; 11025 by default (0x1c00df5c) */
    uint32_t flags;        /* 0x4e,  bit 6 suppresses all event reporting */
    uint32_t frame_index;  /* 0xfa,  as an index; original holds a pointer */
    uint32_t elapsed;      /* 0xfe,  64000ths of a second since the reset */
    uint16_t frame_length; /* 0x2bc, samples in the current frame */
    int16_t interp_scale;  /* 0x2fa, 0x10000 / frame_length */
    uint32_t scratch_2c0;  /* 0x2c0, copied to 0x2e8 by the reset; unread */
    uint32_t scratch_2e8;  /* 0x2e8 */
    uint32_t noise_index;  /* 0x2e4, as an index; original holds a pointer */
    uint16_t noise_left;   /* 0x2ee, samples before the noise table rewinds */
    uint16_t samples_left; /* 0x2f4, output samples still owed this call */
    uint16_t frame_left;   /* 0x30c, samples still owed by this frame */
    uint16_t restart;      /* 0x30e, non-zero asks for the full reset */
    uint16_t scratch_310;  /* 0x310, set to 0x65 by 0x1c00d396 */
    uint8_t last_phoneme;  /* 0x312, last id reported with event 0x3f0 */
    uint16_t speaking;     /* 0x314, cleared when the frame stream ends */
} sv_frame_state;

/* Bit 6 of `flags`: with it set the original skips 0x1c004543..0x1c004605
 * entirely, which is the whole event-reporting block. That block is NOT
 * reconstructed, so sv_frame_apply() requires the bit and returns
 * SV_FRAMES_E_EVENTS without it rather than silently dropping events. */
#define SV_FRAME_FLAG_NO_EVENTS 0x40u

enum {
    SV_FRAMES_END = 0,       /* the frame stream terminated */
    SV_FRAMES_OK = 1,
    SV_FRAMES_E_EVENTS = -1, /* flags lacks SV_FRAME_FLAG_NO_EVENTS */
    SV_FRAMES_E_LENGTH = -2, /* a duration the original would #DE on */
    SV_FRAMES_E_RANGE = -3,  /* a table index past the end of the extract */
    SV_FRAMES_E_COUNT = -4   /* a zero sample count, which the original wraps */
};

/* Samples occupied by one frame, and the matching interpolation scale.
 * TIBASE32 0x1c0044f8..0x1c00452f. All of it is 16-bit arithmetic with
 * 32-bit DX:AX intermediates; widening it changes the result. */
uint16_t sv_frame_length(uint16_t sample_rate, uint8_t duration);

/* The once-per-utterance reset, TIBASE32 0x1c00405d..0x1c00411b. Leaves the
 * state ready for sv_frame_apply() to pick up at frame index 4: the original
 * starts from `frames + 0x80` and then steps one frame before applying, so
 * the first frame the kernel ever sees is frames[5]. */
void sv_frame_reset(sv_frame_state *state);

/* Step to the next frame and load it into the DSP state.
 * TIBASE32 0x1c0044cb..0x1c004969. Reads `frames[state->frame_index + 1]` and,
 * if it is not a terminator, advances `state->frame_index` past it — in that
 * order, which is the original's (0x1c0044cc, 0x1c0044f2).
 *
 * `state->frame_index` must be at least 2: the original reads the previous
 * frame's voicing and aspiration levels through [esi-0x10]/[esi-0xf] and the
 * frame before that one's marker through [esi-0x35].
 *
 * Returns SV_FRAMES_OK, SV_FRAMES_END when the utterance ends here, or a
 * negative SV_FRAMES_E_*. */
int sv_frame_apply(sv_frame_state *state, const sv_frame_tables *tables,
                   const sv_frame *frames);

/* Render `count` unsigned 8-bit PCM samples into `out`, which must have room
 * for `count` bytes. `frames` is the whole array; state->frame_index walks it.
 * When the stream ends the remainder of `out` is filled with 0x80 and
 * state->speaking is cleared, exactly as 0x1c00496e.
 *
 * A call with state->restart set runs the reset first; a call without it
 * resumes mid-frame, and resuming is NOT the same as starting — the original
 * re-enters at 0x1c0044a0, one instruction past the sample-count decrement,
 * so the noise and frame counters owed by the previous call's last sample are
 * paid on entry to this one. TIBASE32!FUN_1c00402c as a whole. */
int sv_frame_render(sv_frame_state *state, const sv_frame_tables *tables,
                    const sv_frame *frames, uint8_t *out, uint16_t count);

/* ------------------------------------------------------------------------ */
/* Phoneme names.                                                            */
/*                                                                           */
/* TIBASE32 0x1c004c85..0x1c004d29, inside the phoneme-string parser          */
/* FUN_1c004a10. Two-level: the first character selects a list through the    */
/* pointer table at VA 0x1c012a68, then the list is scanned for the second    */
/* character. A list entry is a (second character, code) byte pair; the       */
/* terminating pair has a zero second character and carries the code for the  */
/* one-character name.                                                       */
/* ------------------------------------------------------------------------ */

typedef struct {
    /* One list per first character, ' ' (0x20) through '[' (0x5b). A NULL
     * entry is a first character with no list at all -- the original
     * dereferences a null pointer there, see extract_base.py. */
    const uint8_t *lists[0x3c];
} sv_phoneme_names;

#define SV_PHONEME_INVALID 0xffu

/* Reads one phoneme name from `text` and returns its code, or
 * SV_PHONEME_INVALID. `*consumed` receives the number of characters taken.
 * Reproduces the original's handling of runs of 'H': the second character is
 * only offered to the list when the number of consecutive 'H's starting at
 * text[1] is odd, which is what makes "HH" a single name. */
uint8_t sv_phoneme_lookup(const sv_phoneme_names *names, const char *text,
                          unsigned *consumed);

#ifdef __cplusplus
}
#endif
#endif
