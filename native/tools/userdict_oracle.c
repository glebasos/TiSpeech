/*
 * userdict_oracle.c — tiny ctypes shim for verify_narrate.py's --user-dict
 * mode.
 *
 * Builds the same English sv_langmod src/capi.c's text_to_phonemes_eng()
 * does (the extracted TIENG32 .data image plus rule tables), then calls
 * sv_text_to_phon_ex() with a dictionary parsed from caller-supplied bytes.
 * Development/test tool only, mirroring tools/narrate_oracle.c's role for
 * the rest of the pipeline: never linked into the shipped library.
 */
#include "tispeech/userdict.h"

#include <string.h>

extern const uint8_t sv_eng_image[];
extern const uint32_t sv_eng_image_va, sv_eng_image_size;
extern const uint32_t sv_eng_image_desc[10];
extern const sv_image sv_eng_image_text[];
extern const size_t sv_eng_image_text_count;

/*
 * Parses `dict_bytes[0, dict_len)` (or skips the dictionary entirely when
 * dict_bytes is NULL) and converts `text` the same way libtispeech's public
 * tispeech_text_to_phonemes() does for English, through it. Returns what
 * sv_text_to_phon_ex() returns, or a value at or below -9000 for a setup
 * failure (module init) unrelated to the dictionary or the text.
 */
int32_t userdict_oracle_text_to_phon(const uint8_t *dict_bytes, size_t dict_len,
                                     const char *text, char *out,
                                     int32_t out_size, uint32_t flags)
{
    sv_image li = {sv_eng_image, sv_eng_image_va, sv_eng_image_size};
    sv_langmod eng;
    sv_userdict_t *dict = NULL;
    int32_t rc;

    memset(&eng, 0, sizeof eng);
    if (dict_bytes) {
        sv_userdict_status_t st = sv_userdict_parse(dict_bytes, dict_len, &dict);
        if (st != SV_USERDICT_OK)
            return (int32_t)st - 9000;
    }
    if (sv_langmod_init(&eng, &li, sv_eng_image_desc)) {
        sv_userdict_free(dict);
        return -9001;
    }
    if (sv_langgen_attach(&eng, &li, sv_eng_image_text, sv_eng_image_text_count)) {
        sv_langgen_detach(&eng);
        sv_userdict_free(dict);
        return -9002;
    }
    rc = sv_text_to_phon_ex(&eng, text, out, out_size, flags, dict);
    sv_langgen_detach(&eng);
    sv_userdict_free(dict);
    return rc;
}
