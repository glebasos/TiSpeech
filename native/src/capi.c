/*
 * capi.c — implementation of the managed-facing ABI declared in capi.h.
 *
 * This file contains no reconstructed SoftVoice logic. It is glue: it reports
 * what the build actually contains and forwards the one pipeline stage that is
 * finished (letter-to-sound) to src/ruleset.c. Stages that are not
 * reconstructed return TISPEECH_E_NOTIMPL here and are not emulated, faked or
 * substituted.
 */

#include "tispeech/capi.h"
#include "tispeech/ruleset.h"

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
    /* TISPEECH_CAP_SYNTHESIS is deliberately never set: the phoneme-to-frame
     * stage is not reconstructed. See tispeech_synthesize(). */
    return caps;
}

uint32_t tispeech_languages(void)
{
    return (uint32_t)TISPEECH_LANGS_BUILT;
}

const char *tispeech_build_info(void)
{
#if defined(TISPEECH_HAVE_ENG) && defined(TISPEECH_HAVE_SPAN)
    return "tispeech native reconstruction; letter-to-sound: English, Spanish; "
           "synthesis: not implemented";
#elif defined(TISPEECH_HAVE_ENG)
    return "tispeech native reconstruction; letter-to-sound: English; "
           "synthesis: not implemented";
#elif defined(TISPEECH_HAVE_SPAN)
    return "tispeech native reconstruction; letter-to-sound: Spanish; "
           "synthesis: not implemented";
#else
    return "tispeech native reconstruction; letter-to-sound: no language data "
           "compiled in; synthesis: not implemented";
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

/* Decode the ABI's UTF-8 into the matcher's Latin-1 bytes, upper-case them,
 * and add word boundaries. This is boundary glue, not the original engine's
 * number/abbreviation normaliser. Case conversion is locale-independent. */
static int32_t normalise(const char *text, char **out)
{
    size_t n = strlen(text);
    char *buf;
    size_t i = 0, used = 1;

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
        if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xf6)
            || (c >= 0xf8 && c <= 0xfe))
            c -= 0x20;
        /* Treat control characters and non-breaking spaces as separators. */
        buf[used++] = (c < 0x20 || (c >= 0x7f && c <= 0xa0)) ? ' ' : (char)c;
    }
    buf[used++] = ' ';
    buf[used] = '\0';
    *out = buf;
    return TISPEECH_OK;
}

int32_t tispeech_text_to_phonemes(uint32_t language, const char *text,
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

    status = normalise(text, &work);
    if (status != TISPEECH_OK)
        return status;

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

int32_t tispeech_synthesize(uint32_t language, const char *phonemes,
                            uint8_t **out_samples, int32_t *out_count,
                            int32_t *out_sample_rate)
{
    (void)language;
    (void)phonemes;
    /* Report nothing rather than an empty-but-plausible buffer: a caller that
     * ignores the status code must not mistake this for silence it can play. */
    if (out_samples != NULL)
        *out_samples = NULL;
    if (out_count != NULL)
        *out_count = 0;
    if (out_sample_rate != NULL)
        *out_sample_rate = 0;
    return TISPEECH_E_NOTIMPL;
}

void tispeech_free_samples(uint8_t *samples)
{
    free(samples);
}
