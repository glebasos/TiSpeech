/*
 * capi.c — implementation of the managed-facing ABI declared in capi.h.
 *
 * This file contains no reconstructed SoftVoice logic. It is glue: it reports
 * what the build actually contains and forwards text conversion and synthesis
 * to the reconstructed engine. Stages that are not
 * reconstructed return TISPEECH_E_NOTIMPL here and are not emulated, faked or
 * substituted.
 */

#include "tispeech/capi.h"
#include "tispeech/narrate.h"
#include "tispeech/ruleset.h"
#include "tispeech/userdict.h"
#ifdef TISPEECH_HAVE_FRONTEND_SPAN
#  include "tispeech/textphon_span.h"
#endif

#include <stdlib.h>
#include <string.h>

#ifdef TISPEECH_HAVE_ENG
#  define TISPEECH_ENG_BUILT TISPEECH_LANG_ENGLISH
#else
#  define TISPEECH_ENG_BUILT 0u
#endif
#ifdef TISPEECH_HAVE_SPAN
#  define TISPEECH_SPAN_BUILT TISPEECH_LANG_SPANISH
#else
#  define TISPEECH_SPAN_BUILT 0u
#endif
#define TISPEECH_LANGS_BUILT (TISPEECH_ENG_BUILT | TISPEECH_SPAN_BUILT)

uint32_t tispeech_capabilities(void)
{
    uint32_t caps = 0;
    if (TISPEECH_LANGS_BUILT != 0)
        caps |= TISPEECH_CAP_TEXT_TO_PHONEMES;
#ifdef TISPEECH_HAVE_SYNTH_ENG
    /* Phoneme string -> PCM, verified sample-exact against the original
     * engine (tools/verify_narrate.py, tools/verify_spanish.py). Which
     * languages: tispeech_synthesis_languages(). */
    caps |= TISPEECH_CAP_SYNTHESIS;
#endif
    return caps;
}

uint32_t tispeech_languages(void)
{
    return (uint32_t)TISPEECH_LANGS_BUILT;
}

#ifdef TISPEECH_HAVE_SYNTH_SPAN
#  define TISPEECH_SYNTH_SPAN_BUILT TISPEECH_LANG_SPANISH
#else
#  define TISPEECH_SYNTH_SPAN_BUILT 0u
#endif
#ifdef TISPEECH_HAVE_SYNTH_ENG
#  define TISPEECH_SYNTH_BUILT (TISPEECH_LANG_ENGLISH | TISPEECH_SYNTH_SPAN_BUILT)
#else
#  define TISPEECH_SYNTH_BUILT 0u
#endif

uint32_t tispeech_synthesis_languages(void)
{
    return (uint32_t)TISPEECH_SYNTH_BUILT;
}

#if defined(TISPEECH_HAVE_SYNTH_SPAN)
#  define SYNTH_INFO "synthesis: English, Spanish"
#elif defined(TISPEECH_HAVE_SYNTH_ENG)
#  define SYNTH_INFO "synthesis: English"
#else
#  define SYNTH_INFO "synthesis: not built (needs TIBASE32 and TIENG32)"
#endif

const char *tispeech_build_info(void)
{
#if defined(TISPEECH_HAVE_FRONTEND_SPAN)
    return "tispeech native reconstruction; text front end: English, Spanish; "
           SYNTH_INFO;
#elif defined(TISPEECH_HAVE_ENG) && defined(TISPEECH_HAVE_SPAN)
    return "tispeech native reconstruction; text front end: English; letter-to-sound: Spanish; "
           SYNTH_INFO;
#elif defined(TISPEECH_HAVE_ENG)
    return "tispeech native reconstruction; text front end: English; "
           SYNTH_INFO;
#elif defined(TISPEECH_HAVE_SPAN)
    return "tispeech native reconstruction; letter-to-sound: Spanish; "
           SYNTH_INFO;
#else
    return "tispeech native reconstruction; letter-to-sound: no language data "
           "compiled in; " SYNTH_INFO;
#endif
}

static const sv_ruleset_t *ruleset_for(uint32_t language)
{
#ifdef TISPEECH_HAVE_ENG
    if (language == TISPEECH_LANG_ENGLISH)
        return &sv_lang_data_eng;
#endif
#ifdef TISPEECH_HAVE_SPAN
    if (language == TISPEECH_LANG_SPANISH)
        return &sv_lang_data_span;
#endif
    (void)language;
    return NULL;
}

/* Decode UTF-8 to Latin-1. The English front end owns normalisation; the
 * Spanish matcher still needs upper-casing and word boundaries here. */
static int32_t normalise(const char *text, char **out, int frontend)
{
    size_t n = strlen(text);
    char *buf;
    size_t i = 0, used = frontend ? 0 : 1;

    if (n > SIZE_MAX - 3)
        return TISPEECH_E_OUTOFMEMORY;
    buf = (char *)malloc(n + 3);
    if (buf == NULL)
        return TISPEECH_E_OUTOFMEMORY;
    buf[0] = ' ';
    while (i < n) {
        unsigned char c = (unsigned char)text[i++];
        if (c >= 0x80) {
            unsigned char next;
            /* U+0080..U+00FF have exactly these two-byte UTF-8 encodings.
             * Refuse truncated, overlong, non-Latin-1 and raw 8-bit input. */
            if ((c != 0xc2 && c != 0xc3) || i == n) {
                free(buf);
                return TISPEECH_E_BADPARAM;
            }
            next = (unsigned char)text[i++];
            if ((next & 0xc0) != 0x80) {
                free(buf);
                return TISPEECH_E_BADPARAM;
            }
            c = (unsigned char)(((c & 3u) << 6) | (next & 0x3fu));
        }
        if (frontend) {
            buf[used++] = (char)c;
            continue;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xf6)
            || (c >= 0xf8 && c <= 0xfe))
            c -= 0x20;
        /* Treat control characters and non-breaking spaces as separators. */
        buf[used++] = (c < 0x20 || (c >= 0x7f && c <= 0xa0)) ? ' ' : (char)c;
    }
    if (!frontend)
        buf[used++] = ' ';
    buf[used] = '\0';
    *out = buf;
    return TISPEECH_OK;
}

#ifdef TISPEECH_HAVE_ENG
extern const uint8_t sv_eng_image[];
extern const uint32_t sv_eng_image_va, sv_eng_image_size;
extern const uint32_t sv_eng_image_desc[10];
extern const sv_image sv_eng_image_text[];
extern const size_t sv_eng_image_text_count;
#endif
#ifdef TISPEECH_HAVE_FRONTEND_SPAN
extern const uint8_t sv_span_image[];
extern const uint32_t sv_span_image_va, sv_span_image_size;
extern const uint32_t sv_span_desc_image_desc[10];
extern const sv_image sv_span_image_text[];
extern const size_t sv_span_image_text_count;
#endif

/* Languages with a reconstructed front end (SVTextToPhon plus the module's
 * word translator); the others fall back to the letter-to-sound matcher. */
static int has_frontend(uint32_t language)
{
#ifdef TISPEECH_HAVE_ENG
    if (language == TISPEECH_LANG_ENGLISH)
        return 1;
#endif
#ifdef TISPEECH_HAVE_FRONTEND_SPAN
    if (language == TISPEECH_LANG_SPANISH)
        return 1;
#endif
    (void)language;
    return 0;
}

#if defined(TISPEECH_HAVE_ENG) || defined(TISPEECH_HAVE_FRONTEND_SPAN)
/* LoadLanguage for one module, with its generator and duration rules.
 * sv_langgen_detach() releases it. */
static int32_t lang_open(uint32_t language, sv_langmod *m)
{
    memset(m, 0, sizeof *m);
#ifdef TISPEECH_HAVE_ENG
    if (language == TISPEECH_LANG_ENGLISH) {
        sv_image li = {sv_eng_image, sv_eng_image_va, sv_eng_image_size};
        if (sv_langmod_init(m, &li, sv_eng_image_desc))
            return TISPEECH_E_NOLANGUAGE;
        m->duration = sv_eng_duration;
        return sv_langgen_attach(m, &li, sv_eng_image_text, sv_eng_image_text_count)
            ? TISPEECH_E_OUTOFMEMORY : TISPEECH_OK;
    }
#endif
#ifdef TISPEECH_HAVE_FRONTEND_SPAN
    if (language == TISPEECH_LANG_SPANISH) {
        sv_image li = {sv_span_image, sv_span_image_va, sv_span_image_size};
        if (sv_langmod_init(m, &li, sv_span_desc_image_desc))
            return TISPEECH_E_NOLANGUAGE;
        m->duration = sv_span_duration;
        return sv_langgen_attach(m, &li, sv_span_image_text, sv_span_image_text_count)
            ? TISPEECH_E_OUTOFMEMORY : TISPEECH_OK;
    }
#endif
    return TISPEECH_E_NOLANGUAGE;
}

static int32_t text_to_phonemes_frontend(uint32_t language, const char *text,
                                         const sv_userdict_t *dict,
                                         char *out, int32_t size)
{
    /* SVTextToPhon silently emits nothing above 0x202 input bytes. Expose
     * an explicit limit rather than reporting a successful empty conversion. */
    if (strlen(text) > 0x202)
        return TISPEECH_E_BADPARAM;
    sv_langmod m;
    char *phonemes = NULL;
    int32_t rc = lang_open(language, &m);
    if (rc != TISPEECH_OK)
        return rc;
#ifdef TISPEECH_HAVE_FRONTEND_SPAN
    if (language == TISPEECH_LANG_SPANISH)
        rc = sv_tts_phonemes_span_ex(&m, text, 0, dict, &phonemes);
    else
#endif
        rc = sv_tts_phonemes_ex(&m, text, 0, dict, &phonemes);
    sv_langgen_detach(&m);
    if (rc == 0) {
        size_t n = strlen(phonemes);
        if (n >= (size_t)size)
            rc = TISPEECH_E_BUFFERFULL;
        else
            memcpy(out, phonemes, n + 1);
    }
    free(phonemes);
    return rc;
}
#endif

int32_t tispeech_userdict_load(const uint8_t *bytes, int32_t size,
                               tispeech_userdict **out_dict)
{
    sv_userdict_t *dict = NULL;
    if (out_dict == NULL)
        return TISPEECH_E_BADPARAM;
    *out_dict = NULL;
    if (bytes == NULL || size < 0)
        return TISPEECH_E_BADPARAM;
    switch (sv_userdict_parse(bytes, (size_t)size, &dict)) {
    case SV_USERDICT_OK:
        *out_dict = (tispeech_userdict *)dict;
        return TISPEECH_OK;
    case SV_USERDICT_E_SHORT: return TISPEECH_E_DICTSHORT;
    case SV_USERDICT_E_MAGIC: return TISPEECH_E_DICTFORMAT;
    case SV_USERDICT_E_NOMEM: return TISPEECH_E_OUTOFMEMORY;
    default: return TISPEECH_E_BADPARAM;
    }
}

void tispeech_userdict_free(tispeech_userdict *dict)
{
    sv_userdict_free((sv_userdict_t *)dict);
}

int32_t tispeech_text_to_phonemes(uint32_t language, const char *text,
                                  char *out, int32_t out_size)
{
    return tispeech_text_to_phonemes_ex(language, text, NULL, out, out_size);
}

int32_t tispeech_text_to_phonemes_ex(uint32_t language, const char *text,
                                     const tispeech_userdict *dict,
                                     char *out, int32_t out_size)
{
    const sv_ruleset_t *rules;
    char *work;
    const char *p;
    size_t used = 0;
    int32_t status = TISPEECH_OK;

    if (out == NULL || out_size <= 0)
        return TISPEECH_E_BADPARAM;
    out[0] = '\0';
    if (text == NULL)
        return TISPEECH_E_NULLTEXT;

    rules = ruleset_for(language);
    if (rules == NULL)
        return TISPEECH_E_NOLANGUAGE;
    /* The dictionary lookup lives in the reconstructed front ends; the
     * matcher-only path has nowhere to consult it. */
    if (dict != NULL && !has_frontend(language))
        return TISPEECH_E_NOTIMPL;

    status = normalise(text, &work, has_frontend(language));
    if (status != TISPEECH_OK)
        return status;

#if defined(TISPEECH_HAVE_ENG) || defined(TISPEECH_HAVE_FRONTEND_SPAN)
    if (has_frontend(language)) {
        status = text_to_phonemes_frontend(language, work, (const sv_userdict_t *)dict,
                                           out, out_size);
        free(work);
        return status;
    }
#endif

    /* One call per word: the matcher stops at a space by design, so the caller
     * owns word iteration (the original front end does the same). */
    for (p = work + 1; *p != '\0'; ) {
        char word[4096];
        size_t n;

        if (*p == ' ') {
            p++;
            continue;
        }
        if (sv_rules_apply(rules, p, word, sizeof(word), 0) != 0) {
            status = TISPEECH_E_BUFFERFULL;
            break;
        }
        n = strlen(word);
        if (n != 0) {
            if (used != 0) {
                if (used + 1 >= (size_t)out_size) {
                    status = TISPEECH_E_BUFFERFULL;
                    break;
                }
                out[used++] = ' ';
            }
            if (used + n >= (size_t)out_size) {
                status = TISPEECH_E_BUFFERFULL;
                break;
            }
            memcpy(out + used, word, n);
            used += n;
        }
        while (*p != '\0' && *p != ' ')
            p++;
    }

    out[used] = '\0';
    free(work);
    if (status != TISPEECH_OK)
        out[0] = '\0';
    return status;
}

#ifdef TISPEECH_HAVE_SYNTH_ENG
extern const uint8_t sv_base_image[];
extern const uint32_t sv_base_image_va, sv_base_image_size;
extern const sv_frame_tables sv_base_tables;
extern const sv_expr_tables sv_base_tables_expression;

struct pcm_buffer {
    uint8_t *data;
    size_t n, cap;
    int failed;
};

static void pcm_append(void *ctx, const uint8_t *pcm, size_t n)
{
    struct pcm_buffer *b = ctx;
    if (b->failed)
        return;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 0x10000;
        while (cap < b->n + n)
            cap *= 2;
        uint8_t *d = realloc(b->data, cap);
        if (!d) {
            b->failed = 1;
            return;
        }
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->n, pcm, n);
    b->n += n;
}

static int32_t narrate_status(int rc)
{
    switch (rc) {
    case SV_NAR_E_PHONEME:
    case SV_NAR_E_COMMAND: return TISPEECH_E_BADPARAM;
    case SV_NAR_E_NOMEM: return TISPEECH_E_OUTOFMEMORY;
    default: return TISPEECH_E_NOTIMPL;
    }
}

/* One utterance on a fresh engine: SVOpenSpeech's defaults (voice row 0,
 * the language's primary phoneme table) and SVNarrate's sentence loop. */
static int32_t synthesize_lang(uint32_t language, const char *phonemes,
                               const tispeech_voice_options *options,
                               struct pcm_buffer *out)
{
    sv_image bi = {sv_base_image, sv_base_image_va, sv_base_image_size};
    sv_nar_tables tables;
    sv_langmod eng;
    int32_t status;
    if (sv_nar_tables_init(&tables, &bi))
        return TISPEECH_E_NOLANGUAGE;
    status = lang_open(language, &eng);
    if (status != TISPEECH_OK)
        return status;
    sv_engine *e = calloc(1, sizeof *e);
    if (!e) {
        sv_langgen_detach(&eng);
        return TISPEECH_E_OUTOFMEMORY;
    }
    e->base = &tables;
    e->frame_tables = &sv_base_tables;
    e->expr_tables = &sv_base_tables_expression;
    /* TIBASE32 0x1c012038: a module's slot is its language id - 1. */
    e->modules[eng.id - 1] = &eng;
    e->lang = &eng;
    e->lang_primary = &eng;
    e->phonemes = eng.phonemes_a;
    unsigned row = options ? (unsigned)options->personality : 0;
    sv_voice_load(e, tables.voices, row);
    /* SVSetPersonality, TIBASE32 0x1c00ec30, switch at 0x1c00ec78.
     * Only these four personalities select the alternate phoneme table. */
    if (row == 1 || row == 5 || row == 13 || row == 15)
        e->phonemes = eng.phonemes_b;
    if (options) {
        /* Original setters write handle+0xd0's voice block. Addresses:
         * rate e950, pitch e9a0, glottal ea30, F0 style ea70, perturb eab0,
         * range eaf0, vowel ebc0, voicing ebf0 (all TIBASE32 0x1c00xxxx). */
#define SET_VOICE(field, offset) \
        if (options->field >= 0) e->voice[(offset) / 2] = (uint16_t)options->field
        SET_VOICE(pitch, 0x04); SET_VOICE(rate, 0x06);
        SET_VOICE(glottal_source, 0x0a); SET_VOICE(voicing, 0x0c);
        SET_VOICE(f0_style, 0x10); SET_VOICE(f0_range, 0x12);
        SET_VOICE(f0_perturb, 0x14); SET_VOICE(vowel_factor, 0x22);
#undef SET_VOICE
    }
    sv_narrate_begin(e, phonemes, 0);
    for (;;) {
        int rc = sv_narrate_sentence(e, 0);
        if (rc == SV_NAR_DONE)
            break;
        if (rc != SV_NAR_OK) {
            status = narrate_status(rc);
            break;
        }
        if (sv_narrate_render(e, pcm_append, out) != 0) {
            status = TISPEECH_E_NOTIMPL;
            break;
        }
        if (out->failed) {
            status = TISPEECH_E_OUTOFMEMORY;
            break;
        }
    }
    sv_narrate_free(e);
    sv_langgen_detach(&eng);
    free(e);
    return status;
}
#endif

int32_t tispeech_synthesize(uint32_t language, const char *phonemes,
                            uint8_t **out_samples, int32_t *out_count,
                            int32_t *out_sample_rate)
{
    return tispeech_synthesize_ex(language, phonemes, NULL, out_samples,
                                 out_count, out_sample_rate);
}

static int valid_override(int32_t value, int32_t lo, int32_t hi)
{
    return value == -1 || (value >= lo && value <= hi);
}

int32_t tispeech_synthesize_ex(uint32_t language, const char *phonemes,
    const tispeech_voice_options *options, uint8_t **out_samples,
    int32_t *out_count, int32_t *out_sample_rate)
{
    /* Report nothing rather than an empty-but-plausible buffer on any
     * failure: a caller that ignores the status must not mistake it for
     * silence it can play. */
    if (out_samples != NULL)
        *out_samples = NULL;
    if (out_count != NULL)
        *out_count = 0;
    if (out_sample_rate != NULL)
        *out_sample_rate = 0;
    if (phonemes == NULL)
        return TISPEECH_E_NULLTEXT;
    if (out_samples == NULL || out_count == NULL || out_sample_rate == NULL)
        return TISPEECH_E_BADPARAM;
    if (options && (options->personality < 0 || options->personality > 19
        || !valid_override(options->pitch, 10, 2000)
        || !valid_override(options->rate, 20, 500)
        || !valid_override(options->voicing, 0, 2)
        || !valid_override(options->f0_style, 0, 4)
        || !valid_override(options->f0_range, 0, 500)
        || !valid_override(options->f0_perturb, 0, 500)
        || !valid_override(options->vowel_factor, 0, 65535)
        || !valid_override(options->glottal_source, 0, 8)))
        return TISPEECH_E_BADPARAM;
#ifdef TISPEECH_HAVE_SYNTH_ENG
    if ((language & TISPEECH_SYNTH_BUILT) == 0 || (language & (language - 1)) != 0)
        return TISPEECH_E_NOLANGUAGE;
    /* The phoneme alphabet is 7-bit. */
    for (const char *p = phonemes; *p; p++)
        if ((unsigned char)*p >= 0x80)
            return TISPEECH_E_BADPARAM;
    struct pcm_buffer b = {NULL, 0, 0, 0};
    int32_t status = synthesize_lang(language, phonemes, options, &b);
    if (status != TISPEECH_OK || b.n > INT32_MAX) {
        free(b.data);
        return status != TISPEECH_OK ? status : TISPEECH_E_OUTOFMEMORY;
    }
    *out_samples = b.data;
    *out_count = (int32_t)b.n;
    *out_sample_rate = 11025;
    return TISPEECH_OK;
#else
    (void)language;
    return TISPEECH_E_NOTIMPL;
#endif
}

void tispeech_free_samples(uint8_t *samples)
{
    free(samples);
}
