/*
 * narrate.h — the phoneme-string -> parameter-frame pipeline of SVNarrate.
 *
 * PROVENANCE
 * ----------
 * TIBASE32!_SVNarrate@20 (0x1c0036f0) hands a phoneme string to
 * FUN_1c003870, which generates EVERY frame of one sentence up front and only
 * then starts the incremental renderer. The generation half, in call order:
 *
 *   0x1c00c500  next sentence: advance the text cursor, measure the chunk
 *   0x1c004a10  parse the phoneme string into 26-byte records
 *   0x1c003e40  apply language / phoneme-table switches carried by commands
 *   0x1c00c550  mark onsets of stressed syllables
 *   0x1c005910  phonological rewrite rules (language table +0x24)
 *   0x1c00c6d0  phrase and word grouping
 *   0x1c003de0  mark records after a pause
 *   0x1c00cc10  allocate the eight per-syllable intonation arrays
 *   0x1c00c8c0  classify syllables
 *   0x1c00cdc0  compute the intonation targets, phrase by phrase
 *   0x1c00c3e0  per-record durations       (language vtable +0x04)
 *   0x1c00f7c0  rate and emphasis commands
 *   0x1c00c420  drop silent records, allocate the frame array
 *   0x1c005840  per-record frame generation (language vtable +0x08)
 *
 * followed by the passes in smoothing.h and expression.h. Every function
 * cites its address in src/narrate.c.
 *
 * The engine state lives in the original at handle+0x13a, a block that the
 * renderer (frames.h) also addresses. Fields here carry that block's offsets
 * in their comments. Only fields some reconstructed code reads or writes are
 * modelled.
 */

#ifndef TISPEECH_NARRATE_H
#define TISPEECH_NARRATE_H

#include <stddef.h>
#include <stdint.h>

#include "tispeech/expression.h"
#include "tispeech/frames.h"

#ifdef __cplusplus
extern "C" {
#endif

struct sv_engine;
struct sv_langmod;

/* ------------------------------------------------------------------------ */
/* A phoneme record. 26 bytes in the original; the stride appears as          */
/* `add esi, 0x1a` throughout.                                               */
/* ------------------------------------------------------------------------ */
typedef struct sv_record {
    int16_t *commands;                /* +0x00  inline commands, 3 words each,
                                       *        terminated by SV_CMD_END      */
    const uint8_t *phonemes;          /* +0x04  26-byte phoneme definitions  */
    const struct sv_langmod *lang;    /* +0x08                               */
    uint16_t code;                    /* +0x0c  0x00ff terminates the array  */
    uint16_t duration;                /* +0x0e  in 1/8 frame                 */
    uint16_t stress;                  /* +0x10                               */
    uint32_t attrs;                   /* +0x12  copy of definition +0x02     */
    uint32_t flags;                   /* +0x16                               */
} sv_record;

#define SV_RECORD_END     0x00ffu
#define SV_CMD_END        0x1e

/* Phoneme definition entries, 26 bytes, from a language table (+0x14/+0x18).
 *   +0x00 name, +0x02 attributes, +0x12 minimum duration, +0x14 inherent
 *   duration (both int16, read by the English duration rules). */
#define SV_PH_STRIDE      26

/* ------------------------------------------------------------------------ */
/* A language module: the descriptor LoadLanguage publishes.                 */
/* ------------------------------------------------------------------------ */
typedef struct sv_langmod {
    int (*duration)(struct sv_engine *);   /* +0x04, per record   */
    int (*generate)(struct sv_engine *);   /* +0x08, per record   */
    int id;                                 /* +0x10               */
    const uint8_t *phonemes_a;              /* +0x14               */
    const uint8_t *phonemes_b;              /* +0x18               */
    const uint8_t *classes;                 /* +0x1c  LE uint32 per code */
    const uint8_t *voices;                  /* +0x20  74-byte rows       */
    const uint8_t *rules;                   /* +0x24  16-byte rules      */
    void *priv;                             /* language-private state    */
} sv_langmod;

static inline uint32_t sv_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
static inline uint16_t sv_rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/* Class word for a phoneme code: `mov edx,[lang+0x1c]; [edx+code*4]`, code
 * sign-extended (movsx). */
static inline uint32_t sv_class(const sv_langmod *l, uint16_t code)
{
    return sv_rd32(l->classes + (ptrdiff_t)(int16_t)code * 4);
}
static inline uint32_t sv_rec_class(const sv_record *r)
{
    return sv_class(r->lang, r->code);
}

/* ------------------------------------------------------------------------ */
/* TIBASE32 tables the pipeline reads.                                       */
/* ------------------------------------------------------------------------ */
typedef struct sv_nar_tables {
    /* 0x1c012a68: 60 pointers, by (char - 0x20), to lists of
     * (second char, code) byte pairs. Resolved to host pointers. */
    const uint8_t *phoneme_names[60];
    /* 0x1c013600: the 20-row voice table, 74 bytes per row. */
    const uint8_t *voices;
} sv_nar_tables;

/* ------------------------------------------------------------------------ */
/* The engine state, handle+0x13a in the original.                           */
/* ------------------------------------------------------------------------ */
typedef struct sv_engine {
    const sv_nar_tables *base;
    const sv_frame_tables *frame_tables;  /* renderer tables (extract_base.py) */
    const sv_expr_tables *expr_tables;    /* expression-pass tables          */
    sv_frame_state render;                /* the renderer's view of the block */
    /* The loaded modules, TIBASE32 0x1c012038/3c/40 (English, Spanish,
     * German). Command 0x41 selects among them. */
    const sv_langmod *modules[3];

    /* The voice block, handle+0xd0: 0x4a bytes of words, indexed here by
     * byte offset / 2. Filled from a voice-table row by 0x1c00e120. */
    uint16_t voice[0x25];

    const char *text;             /* +0x08 current sentence       */
    uint16_t record_capacity;     /* +0x0c                        */
    uint16_t record_count;        /* +0x0e                        */
    const char *text_end;         /* +0x10                        */
    int32_t chunk_length;         /* +0x14                        */
    int32_t chunk_phonemes;       /* +0x18                        */
    sv_record *records;           /* +0x1c                        */
    sv_record *window[5];         /* +0x20..+0x30, [2] is current */
    int16_t *commands;            /* +0x34 command pool           */
    size_t command_capacity;      /* entries, ours                */
    const sv_langmod *lang;       /* +0x38                        */
    const sv_langmod *lang_primary; /* +0x3c                      */
    const uint8_t *phonemes;      /* +0x40 current definition table */

    uint16_t st_44;               /* +0x44 voice +0x06 */
    uint16_t st_46;               /* +0x46 voice +0x04 */
    uint16_t sample_rate;         /* +0x48 always 11025 */
    uint16_t st_4c;               /* +0x4c voice +0x10 */
    uint32_t flags;               /* +0x4e */
    uint16_t st_52;               /* +0x52 voice +0x08 */
    uint32_t rate;                /* +0x56 voice +0x22, percent */
    uint16_t st_5a;               /* +0x5a voice +0x0a */
    uint16_t st_5c;               /* +0x5c voice +0x24 */
    uint16_t st_5e;               /* +0x5e voice +0x26 */
    uint16_t st_60;               /* +0x60 voice +0x12 */
    uint16_t st_62;               /* +0x62 voice +0x14 */
    uint16_t slew_rise;           /* +0x64 voice +0x18 */
    uint16_t slew_fall;           /* +0x66 voice +0x1a */
    uint16_t st_68;               /* +0x68 vibrato rate */
    int16_t st_6a;                /* +0x6a commanded pitch, scaled */
    uint16_t st_6c;               /* +0x6c glide rate */
    uint32_t st_6e;               /* +0x6e */
    uint16_t st_72;               /* +0x72 */
    uint32_t st_74;               /* +0x74 glide accumulator */
    int16_t st_78, st_7a, st_7c;  /* +0x78..+0x7c vibrato/tremolo/source depth */
    uint16_t st_7e, st_80, st_82; /* +0x7e..+0x82 voice +0x2e..+0x32 */
    uint16_t st_84, st_86, st_88; /* +0x84..+0x88 voice +0x28..+0x2c */
    uint16_t st_8a;               /* +0x8a voice +0x0e */
    uint16_t st_8c, st_8e, st_90; /* voice +0x3a..+0x3e */
    uint16_t st_92, st_94, st_96; /* voice +0x34..+0x38 */
    uint16_t st_98, st_9a, st_9c, st_9e; /* voice +0x40, +0x42, +0x44, +0x46 */
    uint16_t st_a0, st_a2, st_a4, st_a6; /* +0xa0..+0xa6, set by the generator */
    uint16_t st_a8;               /* +0xa8 */
    uint16_t st_aa;               /* +0xaa */
    uint16_t st_ac;               /* +0xac voice +0x48 */
    uint16_t st_ae;               /* +0xae */
    uint16_t pause_short;         /* +0xb0 */
    uint16_t pause_comma;         /* +0xb2 */
    uint16_t pause_long;          /* +0xb4 */

    /* Intonation: eight byte arrays, one entry per syllable group, laid out
     * in one allocation by 0x1c00cc10 and advanced phrase by phrase. */
    uint16_t groups;              /* +0xbe */
    uint8_t *into_base;           /* +0xc0 */
    uint8_t *into_c4;             /* +0xc4 */
    uint8_t *into_c8;             /* +0xc8 */
    uint8_t *into_cc;             /* +0xcc */
    uint8_t *into_d0;             /* +0xd0 */
    uint8_t *into_d4;             /* +0xd4 */
    uint8_t *into_d8;             /* +0xd8 */
    uint8_t *into_dc;             /* +0xdc */
    uint8_t *into_e0;             /* +0xe0 */
    uint16_t phrase_index;        /* +0xe4 */
    uint16_t phrase_stressed;     /* +0xe6 */
    uint16_t phrase_length;       /* +0xe8 */
    uint16_t phrase_first;        /* +0xec */
    uint16_t phrase_last;         /* +0xee */
    uint16_t phrase_accent;       /* +0xf0 */
    sv_record *phrase_record;     /* +0xf2 */

    sv_frame *frames;             /* +0xf6 */
    sv_frame *frame_cursor;       /* +0xfa */
    size_t frame_count;           /* allocated frames, ours */
    uint16_t restart;             /* +0x30e */
    uint16_t st_310;              /* +0x310 */
    uint16_t st_312;              /* +0x312 */
    uint16_t speaking;            /* +0x314 */
    uint16_t st_2b8;              /* +0x2b8 */
    uint16_t st_2f2;              /* +0x2f2 */
} sv_engine;

/* ------------------------------------------------------------------------ */
/* Extracted data. tools/extract_images.py emits each DLL's .data section    */
/* whole; tables are found at their original virtual addresses inside it.   */
/* ------------------------------------------------------------------------ */
typedef struct sv_image {
    const uint8_t *bytes;
    uint32_t va;
    uint32_t size;
} sv_image;

/* Host pointer for an original VA, or NULL outside the image. */
const uint8_t *sv_image_ptr(const sv_image *im, uint32_t va);

/* Resolve TIBASE32's tables. Returns 0, or -1 if the image is not the
 * expected .data section. */
int sv_nar_tables_init(sv_nar_tables *t, const sv_image *base);

/* Fill a module descriptor from a language image and the descriptor that
 * LoadLanguage publishes (+0x00..+0x24, as extracted). */
int sv_langmod_init(sv_langmod *m, const sv_image *lang, const uint32_t desc[10]);

/* English vtable +0x04, src/duration_eng.c. */
int sv_eng_duration(struct sv_engine *e);

/* Vtable +0x08, src/langgen.c: give a module its generator. `data` is the
 * module's .data image (copied: the generator's state lives in it) and
 * `text` the `ntext` segments of tables embedded in its .text. */
int sv_langgen_attach(sv_langmod *m, const sv_image *data, const sv_image *text,
                      size_t ntext);
void sv_langgen_detach(sv_langmod *m);
int sv_lang_generate(struct sv_engine *e);

/* Return codes, in the original's numbering. */
#define SV_NAR_OK          0
#define SV_NAR_DONE        0x3e8   /* nothing left to say */
#define SV_NAR_E_PHONEME   0x1b59  /* unknown phoneme name */
#define SV_NAR_E_NOMEM     0x1b5a
#define SV_NAR_E_COMMAND   0x1b67  /* malformed inline command */
#define SV_NAR_E_NOTIMPL   0xf001  /* ours: a path not yet reconstructed */

/* English text front end, TIBASE32 0x1c00fc50 / TIENG32 0x1c2067c0.
 * Input is Latin-1; flags use SVTextToPhon's original bits. Negative returns
 * encode a buffer-full input offset (-1 - offset). */
#define SV_TP_E_BADARG 0x1b62
int32_t sv_text_to_phon(const sv_langmod *m, const char *text, char *out,
                        int32_t out_size, uint32_t flags);

/* SVTTS's retry/chunk loop (0x1c00f970). Allocates a complete phoneme string;
 * the caller frees it. Returns zero or a positive error code. */
int32_t sv_tts_phonemes(const sv_langmod *m, const char *text, uint32_t flags,
                        char **phonemes);

/* 0x1c00e120: load voice-table row `row` into the voice block. */
void sv_voice_load(sv_engine *e, const uint8_t *voices, unsigned row);

/* 0x1c00df20 (the part that reaches the generation pipeline): copy the voice
 * block into the engine fields. `text` is the phoneme string. */
void sv_narrate_begin(sv_engine *e, const char *text, uint32_t flags);

/* 0x1c00c500. Returns nonzero while there is another sentence. */
int sv_next_sentence(sv_engine *e);

/* 0x1c003870 up to 0x1c003a21: records and frames for the current sentence.
 * Returns SV_NAR_OK, SV_NAR_DONE or an error. `stop_after` (a TIBASE32
 * address from the list above, or 0) ends the pipeline early, for the
 * differential. */
int sv_narrate_sentence(sv_engine *e, uint32_t stop_after);

/* Render the current sentence as the original's waveOut loop does: 0x2000
 * samples when the sentence starts, then 0x1000 per completed buffer while
 * the renderer reports speech (0x1c00f5af), the last buffer padded with
 * silence. `emit` receives each buffer. Returns 0 or a negative
 * SV_FRAMES_E_* code. */
int sv_narrate_render(sv_engine *e, void (*emit)(void *ctx, const uint8_t *pcm, size_t n),
                      void *ctx);

/* Releases what sv_narrate_sentence allocated. */
void sv_narrate_free(sv_engine *e);

#ifdef __cplusplus
}
#endif

#endif /* TISPEECH_NARRATE_H */
