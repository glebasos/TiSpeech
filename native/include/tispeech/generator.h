/*
 * generator.h — two stages of the phoneme -> parameter-frame generator.
 *
 * PROVENANCE
 * ----------
 * The missing middle of the pipeline is the language module's vtable slot
 * +0x08 (TIENG32 `0x1C204690`), driven by `TIBASE32!FUN_1c005840`. That entry
 * point is ~6.4 KB of code over 17 global parameter tracks and is NOT
 * reconstructed here. What is reconstructed are two self-contained leaf
 * functions it calls, both of which carry real work rather than bookkeeping:
 *
 *   TIENG32!FUN_1c203010   phoneme -> manner class      (sv_gen_phoneme_class)
 *   TIENG32!FUN_1c203870   one parameter track -> a     (sv_gen_track_contour)
 *                          per-frame int16 contour
 *
 * Both are leaves: they call nothing. Both are differentially verified against
 * the original under Unicorn by `tools/verify_generator.py`, comparing the
 * whole output AND every byte of the module's `.data` that the original
 * touches, per the discipline in REVERSING.md.
 *
 * Everything else in `0x1C204690` — which tracks exist, what each one means,
 * how the window of five phoneme records at `ctx+0x20..+0x30` sets each
 * track's endpoints, and the four other helpers (`0x1c201b80`, `0x1c2030b0`,
 * `0x1c203870`'s caller loop, `0x1c249310`) — is open. See REVERSING.md.
 *
 *
 * THE CALLER'S SHAPE, as far as it is established
 * -----------------------------------------------
 * `TIBASE32!FUN_1c005840` walks an array of 26-byte phoneme records whose base
 * is at `ctx+0x1c`, maintaining a five-entry sliding window:
 *
 *   ctx+0x20  record[i-2]      ctx+0x2c  record[i+1]
 *   ctx+0x24  record[i-1]      ctx+0x30  record[i+2]
 *   ctx+0x28  record[i]        (both ends clamp rather than run off)
 *
 * and calls `record->language->vtable[+0x08](ctx)` once per record
 * (`0x1c005882`, `0x1c0058b9`). Record fields established so far:
 *
 *   +0x04  const uint8_t *   base of the 26-byte phoneme DEFINITION table
 *   +0x08  sv_language *     the module descriptor (its +0x08 is the generator)
 *   +0x0c  uint16            phoneme code; 0x00FF terminates the array
 *   +0x0e  uint16            duration, in units of 1/8 frame
 *
 * `(duration + 4) / 8` is the record's frame count — the same expression the
 * frame allocator uses (`TIBASE32!FUN_1c00c420`, see frames.h) and the value
 * that reaches this file as `frame_count`.
 */

#ifndef TISPEECH_GENERATOR_H
#define TISPEECH_GENERATOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes. Negative values are ours: the original validates nothing and
 * reads or writes past its tables instead. Every divergence is commented at
 * its site in src/generator.c. */
#define SV_GEN_OK          0
#define SV_GEN_E_ARG      (-1)   /* null pointer, or a field outside the
                                  * domain in which the original's own
                                  * arithmetic stays inside its buffers */
#define SV_GEN_E_RANGE    (-2)   /* rate-table index outside the table */
#define SV_GEN_E_FRAMES   (-3)   /* frame_count outside 1..SV_GEN_MAX_FRAMES */
#define SV_GEN_E_SCRATCH  (-4)   /* scratch capacity too small */

/* The original writes each track's contour into a fixed 0x400-byte buffer
 * (`0x1C250EF0`, `0x1C2512F0`, ... stride 0x400), so a contour cannot exceed
 * 512 int16 samples. */
#define SV_GEN_MAX_FRAMES  512

/* ------------------------------------------------------------------------ */
/* Phoneme definition table.                                                 */
/*                                                                           */
/* 26 bytes per entry. The stride is confirmed by the index arithmetic at     */
/* 0x1c203028..0x1c20302e, which computes 26*code with two `lea x,(x,x,4)`    */
/* and an add rather than a multiply. Two such tables are published by the    */
/* module descriptor, at +0x14 and +0x18 (TIENG32 `0x1C24D720` and            */
/* `0x1C24E250`); they hold the same names in the same order with different   */
/* parameter values, and a phoneme record selects one through its +0x04.      */
/*                                                                           */
/*   +0x00  char[2]   phoneme name, stored low byte first ("IY" is 'Y','I')   */
/*   +0x02  uint32    attribute flags; the only field this file reads         */
/*   +0x06..+0x19     per-phoneme parameter targets (not reconstructed)       */
/* ------------------------------------------------------------------------ */
#define SV_GEN_PHONEME_STRIDE  26
#define SV_GEN_PHONEME_FLAGS   2

typedef struct sv_gen_phonemes {
    const uint8_t *entries;   /* count * SV_GEN_PHONEME_STRIDE bytes */
    size_t         count;
} sv_gen_phonemes;

/* Attribute-flag bits, read off the decision tree at 0x1c203037..0x1c2030aa.
 * The phonetic names are not guesses: they are what the flag actually selects
 * across the English inventory (see the table below). */
#define SV_GEN_PH_VOWEL      0x10000000u  /* 0x1c203037 */
#define SV_GEN_PH_GLIDE      0x00002000u  /* 0x1c203042, `test ah,0x20` */
#define SV_GEN_PH_NASAL      0x00008000u  /* 0x1c20304c, `test ah,0x80` */
#define SV_GEN_PH_FRICATIVE  0x00000040u  /* 0x1c203056, `test al,0x40` */
#define SV_GEN_PH_STOP       0x00040000u  /* 0x1c20306c */
#define SV_GEN_PH_ASPIRATE   0x00000004u  /* 0x1c203085, `test al,0x04` */
#define SV_GEN_PH_SILENCE    0x20000000u  /* 0x1c20308e */
#define SV_GEN_PH_GLOTTAL    0x00000100u  /* 0x1c20309a */
#define SV_GEN_PH_VOICED     0x08000000u  /* 0x1c20305a and 0x1c203073; splits
                                           * fricatives and stops only */

/* Manner classes, in the original's numbering. Which phonemes land in each is
 * not inferred from the bit names — it is the observed partition of the
 * English table at `0x1C24D720`, which is what justifies the names:
 *
 *   0  every vowel and diphthong: IY IH EH AE AA AH AO UH AX IX ER EY AY
 *                                 OY AW OW UW YU
 *   1  approximants:              RX LX WH R L W Y LU ~R
 *   2  nasals:                    M N NX MU NU
 *   3  voiced fricatives:         Z ZH V DH
 *   4  voiced stops/affricate:    J B D G GX
 *   5  voiceless fricatives:      S SH F TH
 *   6  voiceless stops/affricate: CH P T K KX
 *   7  aspirate:                  /H
 *   8  glottal stop:              Q
 *   9  silence and punctuation:   space . ? , - ( ) : and the digit entries
 */
#define SV_GEN_CLASS_VOWEL              0
#define SV_GEN_CLASS_APPROXIMANT        1
#define SV_GEN_CLASS_NASAL              2
#define SV_GEN_CLASS_FRICATIVE_VOICED   3
#define SV_GEN_CLASS_STOP_VOICED        4
#define SV_GEN_CLASS_FRICATIVE_VOICELESS 5
#define SV_GEN_CLASS_STOP_VOICELESS     6
#define SV_GEN_CLASS_ASPIRATE           7
#define SV_GEN_CLASS_GLOTTAL_STOP       8
#define SV_GEN_CLASS_SILENCE            9

/* TIENG32!FUN_1c203010. `code` is a phoneme record's +0x0c.
 *
 * Returns 0..9, or SV_GEN_E_* on a bad argument. The original takes the
 * record pointer and reads +0x0c and +0x04 itself; splitting them makes the
 * function callable without modelling the whole record, and changes nothing
 * about the arithmetic. */
int sv_gen_phoneme_class(const sv_gen_phonemes *table, int code);

/* ------------------------------------------------------------------------ */
/* Parameter tracks.                                                         */
/*                                                                           */
/* The generator keeps 17 of these, in a pointer array at `0x1C250E90`, and   */
/* calls FUN_1c203870 once per track (the loop at 0x1c204cad..0x1c204cc6).    */
/* Each is a 0x40-byte struct; the fields below are every field FUN_1c203870  */
/* reads, at the offsets it reads them from. The remaining words (+0x00,      */
/* +0x10..+0x1c, +0x28) are written by the generator's one-time init at       */
/* 0x1c2049a9..0x1c2049e1 but never read here.                               */
/*                                                                           */
/* Each track produces one int16 per frame. What the 17 tracks MEAN — which   */
/* is F1, which is voicing amplitude — is not established, so nothing here    */
/* names them.                                                               */
/* ------------------------------------------------------------------------ */
typedef struct sv_gen_track {
    int32_t fwd_target;    /* +0x08  value the forward pass approaches  */
    int32_t bwd_target;    /* +0x0c  value the backward pass approaches */
    int32_t fwd_start;     /* +0x20  forward pass starts here           */
    int32_t bwd_start;     /* +0x24  backward pass starts here          */
    int32_t fwd_rate_col;  /* +0x2c  column into the rate table         */
    int32_t bwd_rate_col;  /* +0x30  column into the rate table         */
    int32_t rate_row;      /* +0x34  row, clamped to 0..8 at 0x1c203927 */
    int32_t hold_percent;  /* +0x38  crossfade holds for this % of the
                            *        frames before it starts to fall    */
    int32_t fall_percent;  /* +0x3c  and reaches zero at this %         */
} sv_gen_track;

/* The rate table at `0x1C24C1B0`: 9 rows of 16 int32, each row a geometric
 * decay in Q8 (row 0 starts at 120 and is spent by column 10; row 8 is all
 * 256, i.e. "snap to the target immediately"). The row is the track's
 * `rate_row` clamped to 0..8; the column is `fwd_rate_col` / `bwd_rate_col`,
 * which the original does NOT bounds-check. Extracted by
 * tools/extract_generator.py; never committed. */
#define SV_GEN_RATE_ROWS  9
#define SV_GEN_RATE_COLS  16

typedef struct sv_gen_rates {
    const int32_t *table;  /* count entries, row-major, SV_GEN_RATE_COLS wide */
    size_t         count;  /* SV_GEN_RATE_ROWS * SV_GEN_RATE_COLS */
} sv_gen_rates;

/* Two scratch buffers, each at least `frame_count` int16 — and `ramp` at
 * least `hold` entries, which exceeds `frame_count` when `hold_percent` does.
 * The original uses two fixed globals (`0x1C250A50` and `0x1C250150`) shared
 * by all 17 tracks and rewritten on every call, so they carry nothing between
 * calls and are exposed here rather than hidden in statics. */
typedef struct sv_gen_scratch {
    int16_t *ramp;      /* the crossfade weight, 256 down to 0 */
    int16_t *forward;   /* the forward pass, before the backward pass mixes in */
    size_t   capacity;  /* entries in each */
} sv_gen_scratch;

/* TIENG32!FUN_1c203870, `0x1c203870..0x1c203a27`. Produces one int16 per
 * frame into `contour`, and leaves the two intermediate contours in
 * `scratch` (which is what makes each stage separately testable).
 *
 * The shape, in three passes:
 *
 *   ramp[i]     = 256 for i < hold, then a linear fall to 0 at `fall`
 *                 (Q15 internally, stored >>7); ramp[frame_count-1] is
 *                 forced to 0 at 0x1c20391a
 *   forward[i]  = (ramp[i] * v) >> 8, where v starts at fwd_start and moves
 *                 toward fwd_target by (target - v) * rate >> 8 each frame
 *   contour[i]  = forward[i] + ((256 - ramp[i]) * w) >> 8, walked BACKWARDS
 *                 from i = frame_count-1, where w starts at bwd_start and
 *                 moves toward bwd_target the same way
 *
 * i.e. a crossfade from a contour anchored at the start of the phoneme to one
 * anchored at its end. Every multiply, add and subtract wraps at 32 bits and
 * the final add wraps at 16 (`addw` at 0x1c2039fc), which src/generator.c
 * reproduces explicitly — the same trap that dsp.c documents. */
int sv_gen_track_contour(const sv_gen_track *track,
                         int frame_count,
                         const sv_gen_rates *rates,
                         int16_t *contour,
                         const sv_gen_scratch *scratch);

/* The first pass alone, `0x1c20387b..0x1c203923`. Exposed because it is the
 * part with the awkward arithmetic and it is worth pinning on its own. */
int sv_gen_crossfade_ramp(const sv_gen_track *track,
                          int frame_count,
                          int16_t *ramp,
                          size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* TISPEECH_GENERATOR_H */
