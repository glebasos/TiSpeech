/*
 * textphon_span.h — public entry points of src/textphon_span.c, the Spanish
 * text front end (TIBASE32's SVTextToPhon + TISPAN32's word translator,
 * module vtable +0x0C, 0x1C406250).
 *
 * Declared separately from narrate.h (owned elsewhere) and mirrors
 * include/tispeech/userdict.h's sv_text_to_phon_ex()/sv_tts_phonemes_ex()
 * shape for the optional user dictionary. See src/textphon_span.c's own
 * header comment for exactly what is and is not reconstructed, and cited
 * TISPAN32 addresses for every function.
 */

#ifndef TISPEECH_TEXTPHON_SPAN_H
#define TISPEECH_TEXTPHON_SPAN_H

#include <stdint.h>

#include "tispeech/narrate.h"
#include "tispeech/userdict.h"

#ifdef __cplusplus
extern "C" {
#endif

/* As sv_text_to_phon() (narrate.h), but TISPAN32's word translator instead
 * of TIENG32's. `dict` may be NULL (sv_text_to_phon_span() below). */
int32_t sv_text_to_phon_span_ex(const sv_langmod *m, const char *text, char *out,
                                int32_t out_size, uint32_t flags,
                                const sv_userdict_t *dict);

/* sv_text_to_phon_span_ex(m, text, out, out_size, flags, NULL). */
int32_t sv_text_to_phon_span(const sv_langmod *m, const char *text, char *out,
                             int32_t out_size, uint32_t flags);

/* As sv_tts_phonemes() (narrate.h): SVTTS's buffer-retry loop around
 * sv_text_to_phon_span_ex(), with every call consulting `dict`. */
int32_t sv_tts_phonemes_span_ex(const sv_langmod *m, const char *text,
                                uint32_t flags, const sv_userdict_t *dict,
                                char **phonemes);

/* sv_tts_phonemes_span_ex(m, text, flags, NULL, phonemes). */
int32_t sv_tts_phonemes_span(const sv_langmod *m, const char *text, uint32_t flags,
                             char **phonemes);

#ifdef __cplusplus
}
#endif
#endif /* TISPEECH_TEXTPHON_SPAN_H */
