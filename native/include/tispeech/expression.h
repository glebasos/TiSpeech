/*
 * expression.h — the voice-expression pass, TIBASE32!FUN_1c00be40.
 *
 * PROVENANCE
 * ----------
 * The last of the five calls the engine makes after generating parameter
 * frames (0x1c003a13 in the sequence at 0x1c0039ec; see smoothing.h). It walks
 * the phoneme record array, applies the inline control commands each record
 * carries, and then rewrites every frame that record covers: rescaling pitch,
 * adding vibrato, tremolo and flutter, gliding toward the commanded pitch, and
 * choosing the glottal source waveform pair.
 *
 * It is not a smoothing pass, which an earlier reading of the call sequence
 * assumed. It is where a "voice" — Male, Female, Robotoid, The Fly — actually
 * reaches the frames.
 *
 * WHAT DRIVES IT
 * --------------
 * Each 26-byte phoneme record (see generator.h) carries at +0x00 a pointer to
 * a command list, read at 0x1c00beb7. That field was unidentified until this
 * pass was reconstructed. A command is 6 bytes:
 *
 *   +0x00  int16   command code; 0x1e terminates the list
 *   +0x02  int16   value
 *   +0x04          two bytes this pass never reads
 *
 * Codes outside 0x32..0x104 are ignored, and inside that range all but nine
 * are ignored too: the byte table at 0x1c00c2e0 maps 202 of the 211 codes to
 * the "next command" label. The nine that do something are SV_EXPR_CMD_*
 * below. A NULL command pointer and a list whose first code is 0x1e are both
 * handled at 0x1c00bebb..0x1c00bec5.
 *
 * THE VOICE TABLE
 * ---------------
 * SV_EXPR_CMD_VOICE selects a row of the table at 0x1c013600 — 74 bytes per
 * row, terminated by a row whose name pointer is NULL, which is what bounds it
 * at 20 rows. Only two of its fields reach this pass:
 *
 *   +0x0c -> state->source_class    (1..8 across the 20 voices)
 *   +0x16 -> state->flutter_depth   (0, 10, 15, 20, 30, 90 or 120)
 *
 * The rows' first dword is a pointer to the voice name, which is how the row
 * count was established: Male, Female, Fast Female, Child, Giant Male, Mellow
 * Female, Choir Girl, Amazon, The Fly, Robotoid, Martian, Colossus, Fast Fred,
 * Old Woman, Munchkin, Troll, Nerd, Milktoast, Tipsy, Choir Boy. The names are
 * not extracted or shipped; they are evidence for the bound, recorded here.
 */

#ifndef TISPEECH_EXPRESSION_H
#define TISPEECH_EXPRESSION_H

#include <stddef.h>
#include <stdint.h>

#include "tispeech/frames.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes. Negative values are ours. The original validates nothing: it
 * divides by caller-supplied words without checking them for zero, indexes the
 * voice table unbounded, and walks frames until the record array says stop.
 * Every divergence is commented at its site in src/expression.c. */
#define SV_EXPR_OK          0
#define SV_EXPR_E_ARG      (-1)  /* null pointer, or a divisor of zero where
                                  * the original faults on `div` */
#define SV_EXPR_E_RANGE    (-2)  /* a table index outside the extracted table */
#define SV_EXPR_E_FRAMES   (-3)  /* the records ask for more frames than the
                                  * caller supplied */

/* Command codes, from the dispatch table at 0x1c00c2b8/0x1c00c2e0. */
#define SV_EXPR_CMD_VOICE        0x32   /* 0x1c00beec, voice table row       */
#define SV_EXPR_CMD_VIBRATO      0xa0   /* 0x1c00bf21, state->vibrato_depth  */
#define SV_EXPR_CMD_TREMOLO      0xa2   /* 0x1c00bf2e, state->tremolo_depth  */
#define SV_EXPR_CMD_SOURCE_MOD   0xa4   /* 0x1c00bf3b, state->source_depth   */
#define SV_EXPR_CMD_VIBRATO_RATE 0xaa   /* 0x1c00bf48, state->vibrato_rate   */
#define SV_EXPR_CMD_PITCH        0xac   /* 0x1c00bf6c, state->pitch_percent  */
#define SV_EXPR_CMD_RATE         0xae   /* 0x1c00bf9f, state->glide_rate     */
#define SV_EXPR_CMD_SOURCE_CLASS 0xd2   /* 0x1c00bfb8, state->source_class   */
#define SV_EXPR_CMD_FLUTTER      0x104  /* 0x1c00bfc2, state->flutter_depth  */
#define SV_EXPR_CMD_END          0x1e   /* 0x1c00bec1, terminates the list   */

#define SV_EXPR_CMD_STRIDE  6

/* ------------------------------------------------------------------------ */
/* Tables. All TIBASE32 data, extracted at build time by tools/extract_base.py */
/* and never committed. Untyped blobs for the same reason frames.h uses them:  */
/* the original computes byte offsets, and declaring an element type here      */
/* would quietly correct indexing the differential is meant to catch.          */
/* ------------------------------------------------------------------------ */

enum {
    /* 0x1c001470, read one signed byte per frame and wrapped every 256
     * (0x1c00c1a8). Sits immediately before the noise table at 0x1c001570. */
    SV_EXPR_FLUTTER_BYTES = 0x100,

    /* 0x1c013430, read as an int16 at an EVEN byte offset derived from a
     * 13-bit phase (0x1c00c02f: `shr ax,5` then `and eax,-2`), so the reachable
     * offsets are 0x00..0xfe and the table is exactly 128 entries. */
    SV_EXPR_LFO_BYTES = 0x100,
    SV_EXPR_LFO_ENTRIES = 0x80,

    /* 0x1c013600, 74 bytes per row, 20 rows (see above). */
    SV_EXPR_VOICE_STRIDE = 74,
    SV_EXPR_VOICE_ROWS = 20,
    SV_EXPR_VOICE_SOURCE_CLASS = 0x0c,
    SV_EXPR_VOICE_FLUTTER = 0x16,

    /* 0x1c012ee8, five bytes per pitch, reachable only for pitch < 0x10e
     * (0x1c00c1ee). 0x10e * 5 == 0x546 bytes, which ends exactly two bytes
     * before the LFO table — the tables are adjacent, which is what bounds
     * this one. */
    SV_EXPR_SOURCE_MAP_PITCHES = 0x10e,
    SV_EXPR_SOURCE_MAP_STRIDE = 5,
    SV_EXPR_SOURCE_MAP_BYTES = 0x10e * 5
};

typedef struct sv_expr_tables {
    const uint8_t *flutter;     /* SV_EXPR_FLUTTER_BYTES, read as int8   */
    const uint8_t *lfo;         /* SV_EXPR_LFO_BYTES, read as LE int16   */
    const uint8_t *voices;      /* SV_EXPR_VOICE_ROWS rows               */
    const uint8_t *source_map;  /* SV_EXPR_SOURCE_MAP_BYTES              */
} sv_expr_tables;

/* ------------------------------------------------------------------------ */
/* The record array.                                                         */
/*                                                                           */
/* The original walks 26-byte records starting at state+0x1c PLUS ONE RECORD  */
/* (0x1c00bea0 adds 0x1a before the first test), and stops at a record whose  */
/* +0x0c is 0x00ff. Only three fields are read, so this takes them as a       */
/* translated array rather than modelling all 26 bytes — the same choice      */
/* generator.c makes for the two fields its stages need.                      */
/* ------------------------------------------------------------------------ */
typedef struct sv_expr_record {
    /* +0x00. NULL is allowed and means "no commands" (0x1c00bebb). */
    const uint8_t *commands;
    /* +0x0c. SV_EXPR_RECORD_END terminates the array. */
    uint16_t code;
    /* +0x0e, in eighths of a frame. The record covers (duration + 4) / 8
     * frames (0x1c00bfe1..0x1c00bfea), the same expression the frame
     * allocator and the generator use. */
    uint16_t duration;
} sv_expr_record;

#define SV_EXPR_RECORD_END 0x00ffu

/* ------------------------------------------------------------------------ */
/* Engine state.                                                             */
/*                                                                           */
/* The original keeps these in its 0x400-byte state block; the offset of each */
/* is given so the differential can marshal between the two. Fields this pass */
/* reads but never writes are marked. Nothing outside this set is touched:    */
/* tools/verify_expression.py asserts that over the whole block.              */
/* ------------------------------------------------------------------------ */
typedef struct sv_expr_state {
    /* +0x48. Divides the pitch of every frame (0x1c00c00e). Read-only here;
     * zero makes the original fault on `div`. */
    uint16_t pitch_divisor;
    /* +0x52. Read-only, and only in source class 0 (0x1c00c1e3): nonzero
     * suppresses the automatic glottal-source choice entirely. */
    uint16_t source_lock;
    /* +0x5a. 0..8 select a fixed source pair; anything else zeroes both
     * selectors (0x1c00c1d6). */
    uint16_t source_class;
    /* +0x62. Depth of the flutter added from the table at 0x1c001470. */
    uint16_t flutter_depth;
    /* +0x68. Vibrato rate; the per-frame phase step is
     * (int16)vibrato_rate * 832 / 127 (0x1c00be70..0x1c00be7d). */
    int16_t vibrato_rate;
    /* +0x6a. The commanded pitch, already scaled: SV_EXPR_CMD_PITCH stores
     * value * 256 / 100 here (0x1c00bf6c), so the command's value is a
     * percentage. */
    int16_t pitch_scaled;
    /* +0x6c. Divides 0x10000 to give the per-frame glide step (0x1c00be97).
     * Zero makes the original fault on `idiv`. */
    int16_t glide_rate;
    /* +0x6e. Set by SV_EXPR_CMD_PITCH: 1 when the new pitch is above the
     * previous one, 0 otherwise. Selects which way the glide runs. */
    int32_t glide_rising;
    /* +0x72. The previous commanded pitch, for that comparison. */
    int16_t pitch_previous;
    /* +0x74. The glide accumulator, in the same Q8 units as pitch_scaled
     * shifted left 8. Carried across calls; the pass both reads and writes it. */
    int32_t glide;
    /* +0x78, +0x7a, +0x7c. Depths for the three modulations, each gated on
     * itself being nonzero AND the glide being nonzero. */
    int16_t vibrato_depth;
    int16_t tremolo_depth;
    int16_t source_depth;
} sv_expr_state;

/* TIBASE32!FUN_1c00be40, `0x1c00be40..0x1c00c2b5`.
 *
 * Applies `records` to `frames`, updating `state` as the records' commands
 * say. `frame_capacity` bounds the walk: the original has no such argument and
 * runs off the end of the array if the records ask for more frames than were
 * allocated, which is not reproducible as a return value.
 *
 * The per-frame work, in order (0x1c00bffb..0x1c00c293):
 *
 *   1. pitch = (pitch << 17) / state->pitch_divisor, unsigned, and the shift
 *      wraps at 32 bits, so a pitch of 0x8000 or more loses its top bit.
 *   2. The LFO phase advances by the vibrato step and wraps at 13 bits; the
 *      sample is table[(phase >> 5) & ~1] read as int16.
 *   3. The glide moves toward pitch_scaled << 8 — up by 0x10000/glide_rate
 *      when glide_rising is 1, down by a fixed 0x1999 otherwise — and clamps
 *      at the target.
 *   4. Vibrato scales the frame's pitch, tremolo shifts amp_voicing and
 *      amp_aspiration (clamped to 0..0x80), and source_depth shifts the low
 *      nibble of bandwidth_f3_source (clamped to 0..0xf). Each is gated on the
 *      glide being nonzero, and if none of the three fired the LFO phase is
 *      reset to zero (0x1c00c176).
 *   5. Flutter adds table[i] * flutter_depth * the PRE-RESCALE pitch >> 13.
 *      The table index advances once per frame across the whole utterance and
 *      wraps at 256.
 *   6. source_mix is set to 0xff and the source pair is chosen: fixed for
 *      classes 1..8, and for class 0 (unless source_lock is set) by scanning
 *      source_map[pitch * 5 + k] for the first nonzero byte.
 *
 * Returns SV_EXPR_OK, or one of the codes above. On a refusal nothing has
 * been written that the caller can observe except frames already completed —
 * the original has no failure path at all, so there is no behaviour to match.
 */
int sv_expression_apply(sv_expr_state *state,
                        sv_frame *frames,
                        size_t frame_capacity,
                        const sv_expr_record *records,
                        const sv_expr_tables *tables);

#ifdef __cplusplus
}
#endif

#endif /* TISPEECH_EXPRESSION_H */
