/*
 * svphon_span — run the reconstructed Spanish text front end
 * (src/textphon_span.c, TIBASE32's SVTextToPhon + TISPAN32's word
 * translator) over a line of Spanish text.
 *
 * Development harness, mirroring tools/svphon.c: it exercises exactly the
 * stages reconstructed so far (exception dictionary, letter-to-sound rules,
 * default stress) and nothing else. Numeric tokens return
 * SV_NAR_E_NOTIMPL, reported rather than guessed at -- see
 * src/textphon_span.c's header for exactly what is and is not ported.
 *
 *   svphon_span "hola mundo"
 *   svphon_span "el niño canta"
 */

#include "tispeech/narrate.h"
#include "tispeech/textphon_span.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const uint8_t sv_span_image[];
extern const uint32_t sv_span_image_va, sv_span_image_size;
extern const sv_image sv_span_image_text[];
extern const size_t sv_span_image_text_count;
extern const uint32_t sv_span_desc_image_desc[10];

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: svphon_span <text>\n");
        return 2;
    }

    sv_image li = {sv_span_image, sv_span_image_va, sv_span_image_size};
    sv_langmod span = {0};
    char *phonemes = NULL;
    int32_t rc;

    if (sv_langmod_init(&span, &li, sv_span_desc_image_desc)) {
        fprintf(stderr, "svphon_span: sv_langmod_init failed\n");
        return 1;
    }
    if (sv_langgen_attach(&span, &li, sv_span_image_text, sv_span_image_text_count)) {
        fprintf(stderr, "svphon_span: sv_langgen_attach failed\n");
        return 1;
    }

    rc = sv_tts_phonemes_span(&span, argv[1], 0, &phonemes);
    if (rc == SV_NAR_E_NOTIMPL) {
        fprintf(stderr, "svphon_span: not implemented for this input "
                       "(numeric token, or another unreconstructed path)\n");
        sv_langgen_detach(&span);
        return 1;
    }
    if (rc != 0) {
        fprintf(stderr, "svphon_span: error %d\n", (int)rc);
        sv_langgen_detach(&span);
        return 1;
    }
    printf("%s\n", phonemes);
    free(phonemes);
    sv_langgen_detach(&span);
    return 0;
}
