/*
 * narrate_oracle.c — ctypes entry points for tools/verify_narrate.py.
 *
 * Development only; never part of libtispeech. Builds an English engine on
 * the extracted images and exposes the pipeline stage by stage, with its
 * records, intonation arrays and frames flattened into plain integer arrays
 * the Python side can compare with the original's memory.
 */

#include "tispeech/narrate.h"

#include <stdlib.h>
#include <string.h>

extern const uint8_t sv_base_image[];
extern const uint32_t sv_base_image_va, sv_base_image_size;
extern const uint8_t sv_eng_image[];
extern const uint32_t sv_eng_image_va, sv_eng_image_size;
extern const uint32_t sv_eng_image_desc[10];
extern const sv_frame_tables sv_base_tables;
extern const sv_expr_tables sv_base_tables_expression;
extern const sv_image sv_eng_image_text[];
extern const size_t sv_eng_image_text_count;

#ifdef TISPEECH_ORACLE_SPAN
extern const uint8_t sv_span_image[];
extern const uint32_t sv_span_image_va, sv_span_image_size;
extern const uint32_t sv_span_desc_image_desc[10];
extern const sv_image sv_span_image_text[];
extern const size_t sv_span_image_text_count;
#define sv_eng_image sv_span_image
#define sv_eng_image_va sv_span_image_va
#define sv_eng_image_size sv_span_image_size
#define sv_eng_image_desc sv_span_desc_image_desc
#define sv_eng_image_text sv_span_image_text
#define sv_eng_image_text_count sv_span_image_text_count
#define sv_eng_duration sv_span_duration
#endif

static sv_nar_tables base;
static sv_langmod eng;
static sv_engine engine;

sv_engine *oracle_new(unsigned voice_row)
{
    sv_image bi = {sv_base_image, sv_base_image_va, sv_base_image_size};
    sv_image li = {sv_eng_image, sv_eng_image_va, sv_eng_image_size};
    if (sv_nar_tables_init(&base, &bi) || sv_langmod_init(&eng, &li, sv_eng_image_desc))
        return NULL;
    eng.duration = sv_eng_duration;
    sv_langgen_detach(&eng);
    if (sv_langgen_attach(&eng, &li, sv_eng_image_text, sv_eng_image_text_count))
        return NULL;
    sv_narrate_free(&engine);
    memset(&engine, 0, sizeof engine);
    engine.base = &base;
    engine.frame_tables = &sv_base_tables;
    engine.expr_tables = &sv_base_tables_expression;
    engine.modules[eng.id - 1] = &eng;   /* TIBASE32 0x1c012038: English 0, Spanish 1 */
    engine.lang = &eng;
    engine.lang_primary = &eng;
    engine.phonemes = eng.phonemes_a;
    sv_voice_load(&engine, base.voices, voice_row);
    return &engine;
}

int oracle_begin(sv_engine *e, const char *phon, uint32_t flags)
{
    static char *copy;
    free(copy);
    copy = strdup(phon);
    sv_narrate_begin(e, copy, flags);
    return 0;
}

/* 0x1c00ecd0, _SVSetSpeakingMode@8: e->handle_flags persists across
 * oracle_begin/oracle_sentence calls, matching handle+0xcc in the original.
 * Call this before oracle_begin so sv_narrate_begin folds it into e->flags. */
int oracle_set_speaking_mode(sv_engine *e, uint32_t value)
{
    return sv_engine_set_speaking_mode(e, value);
}

int oracle_sentence(sv_engine *e, uint32_t stop_after)
{
    return sv_narrate_sentence(e, stop_after);
}

/* 16 int32 per record, through the terminator:
 *   code, duration, stress, attrs, flags, language id, table (0 = +0x14,
 *   1 = +0x18, 2 = other), command word count, then up to two commands as
 *   (code, value, extra). */
int oracle_records(sv_engine *e, int32_t *out, int max)
{
    int n = 0;
    if (!e->records)
        return 0;
    for (const sv_record *r = e->records; n < max; r++, n++) {
        int32_t *o = out + 16 * n;
        memset(o, 0, 16 * sizeof *o);
        o[0] = r->code;
        o[1] = r->duration;
        o[2] = r->stress;
        o[3] = (int32_t)r->attrs;
        o[4] = (int32_t)r->flags;
        o[5] = r->lang ? r->lang->id : -1;
        o[6] = !r->lang ? 3 : r->phonemes == r->lang->phonemes_a ? 0
             : r->phonemes == r->lang->phonemes_b ? 1 : 2;
        int k = 0;
        if (r->commands)
            for (const int16_t *c = r->commands; c[0] != SV_CMD_END && k + 3 <= 8; c += 3) {
                o[8 + k++] = c[0];
                o[8 + k++] = c[1];
                o[8 + k++] = c[2];
            }
        o[7] = k;
        if (r->code == SV_RECORD_END) {
            n++;
            break;
        }
    }
    return n;
}

/* The eight intonation arrays as one block, 8 * (groups + 3) bytes. */
int oracle_intonation(sv_engine *e, uint8_t *out, int max)
{
    int n = 8 * (e->groups + 3);
    if (!e->into_base || n > max)
        return -1;
    memcpy(out, e->into_base, (size_t)n);
    return n;
}

int oracle_frames(sv_engine *e, uint8_t *out, int max)
{
    size_t n = e->frame_count * sizeof(sv_frame);
    if (!e->frames || (int)n > max)
        return -1;
    memcpy(out, e->frames, n);
    return (int)n;
}

int oracle_count(sv_engine *e)
{
    return e->record_count;
}

struct pcm_sink {
    uint8_t *out;
    size_t n, max;
};

static void sink(void *ctx, const uint8_t *pcm, size_t n)
{
    struct pcm_sink *p = ctx;
    for (size_t k = 0; k < n && p->n < p->max; k++)
        p->out[p->n++] = pcm[k];
}

/* The whole utterance, every sentence, as the original's waveOutWrite
 * stream. Returns the byte count, or a negative error. */
int oracle_pcm(sv_engine *e, const char *phon, uint8_t *out, int max)
{
    struct pcm_sink p = {out, 0, (size_t)max};
    oracle_begin(e, phon, 0);
    for (;;) {
        int rc = sv_narrate_sentence(e, 0);
        if (rc == SV_NAR_DONE)
            break;
        if (rc)
            return -rc;
        rc = sv_narrate_render(e, sink, &p);
        if (rc)
            return rc;
    }
    return (int)p.n;
}
