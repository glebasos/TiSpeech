/*
 * textphon_span_oracle.c — tiny ctypes shim for verify_narrate.py's
 * --language span --frontend-only mode.
 *
 * Builds the Spanish sv_langmod the same way tools/svphon_span.c does (the
 * merged generator/front-end image from tools/extract_span_frontend.py, plus
 * TISPAN32's own real .data image for the LoadLanguage descriptor), then
 * calls sv_text_to_phon_span_ex() -- the direct equivalent of the single
 * _SVTextToPhon@24 call tools/sv_emu.py's Emu.text_to_phon() makes against
 * the original, not the chunked SVTTS retry wrapper. `dict_bytes` is
 * optional (NULL skips the user dictionary entirely), mirroring
 * tools/userdict_oracle.c's shape so the same differential machinery can
 * exercise both. Development/test tool only; never linked into the shipped
 * library.
 */
#include "tispeech/textphon_span.h"
#include "tispeech/userdict.h"

#include <string.h>

extern const uint8_t sv_span_image[];
extern const uint32_t sv_span_image_va, sv_span_image_size;
extern const sv_image sv_span_image_text[];
extern const size_t sv_span_image_text_count;
extern const uint32_t sv_span_desc_image_desc[10];

/*
 * Converts `text` through the reconstructed Spanish front end, optionally
 * consulting a user dictionary parsed from `dict_bytes[0, dict_len)`
 * (skipped when dict_bytes is NULL). Returns what sv_text_to_phon_span_ex()
 * returns, or a value at or below -9000 for a setup failure (module init)
 * unrelated to the text itself.
 */
int32_t textphon_span_oracle_text_to_phon(const uint8_t *dict_bytes, size_t dict_len,
                                          const char *text, char *out,
                                          int32_t out_size, uint32_t flags)
{
    sv_image li = {sv_span_image, sv_span_image_va, sv_span_image_size};
    sv_langmod span;
    sv_userdict_t *dict = NULL;
    int32_t rc;

    memset(&span, 0, sizeof span);
    if (dict_bytes) {
        sv_userdict_status_t st = sv_userdict_parse(dict_bytes, dict_len, &dict);
        if (st != SV_USERDICT_OK)
            return (int32_t)st - 9000;
    }
    if (sv_langmod_init(&span, &li, sv_span_desc_image_desc)) {
        sv_userdict_free(dict);
        return -9001;
    }
    if (sv_langgen_attach(&span, &li, sv_span_image_text, sv_span_image_text_count)) {
        sv_userdict_free(dict);
        return -9002;
    }
    rc = sv_text_to_phon_span_ex(&span, text, out, out_size, flags, dict);
    sv_langgen_detach(&span);
    sv_userdict_free(dict);
    return rc;
}
