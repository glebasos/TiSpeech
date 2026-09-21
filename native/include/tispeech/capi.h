/*
 * capi.h — stable C ABI for managed (P/Invoke) callers.
 *
 * SCOPE
 * -----
 * This is OUR interface, not a reconstruction of SoftVoice's. It exists so the
 * .NET side can consume whatever the reconstruction has actually finished,
 * without guessing at internal layouts. The reconstructed SoftVoice surface
 * lives in svapi.h and keeps its original shapes.
 *
 * HONESTY CONTRACT
 * ----------------
 * tispeech_capabilities() reports what this build can really do. A caller must
 * gate on it rather than on the host operating system: a build without
 * language data cannot convert text, and no build can synthesise audio yet.
 * Entry points for stages that are not reconstructed return
 * TISPEECH_E_NOTIMPL. Nothing here returns success it did not earn.
 */

#ifndef TISPEECH_CAPI_H
#define TISPEECH_CAPI_H

#include <stdint.h>

#if defined(_WIN32)
#  define TISPEECH_API __declspec(dllexport)
#else
#  define TISPEECH_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes. 0 is success; the SoftVoice-derived codes keep the values in
 * svapi.h so a single managed enum covers both backends. */
#define TISPEECH_OK             0
#define TISPEECH_E_BADPARAM     0x1b62
#define TISPEECH_E_NOLANGUAGE   0x1b6e
#define TISPEECH_E_NULLTEXT     0x1b70
#define TISPEECH_E_OUTOFMEMORY  0x1b5a
#define TISPEECH_E_NOTIMPL      0xF001
#define TISPEECH_E_BUFFERFULL   0xF002

/* Language selector; same bit values as SV_LANG_* in svapi.h. */
#define TISPEECH_LANG_ENGLISH   0x1u
#define TISPEECH_LANG_SPANISH   0x2u
#define TISPEECH_LANG_GERMAN    0x4u

/* Capability bits returned by tispeech_capabilities(). */
#define TISPEECH_CAP_TEXT_TO_PHONEMES 0x1u /* sv_rules_apply is linked in */
#define TISPEECH_CAP_SYNTHESIS        0x2u /* phoneme -> PCM works end to end */

/* Bitmask of TISPEECH_CAP_*. Cheap, side-effect free, safe to call first. */
TISPEECH_API uint32_t tispeech_capabilities(void);

/* Bitmask of TISPEECH_LANG_* whose rule data was compiled into this build.
 * Zero when the build was configured without an original language DLL. */
TISPEECH_API uint32_t tispeech_languages(void);

/* Human-readable build description, for logs and the about box. Static
 * storage; the caller must not free it. */
TISPEECH_API const char *tispeech_build_info(void);

/*
 * Grapheme-to-phoneme conversion. `text` is NUL-terminated UTF-8 restricted to
 * Latin-1 (the rule tables are 8-bit). UTF-8 is decoded and Latin-1 letters
 * are upper-cased independently of the process locale. Invalid UTF-8 and
 * characters above U+00FF return TISPEECH_E_BADPARAM. The result is written
 * to `out` as a NUL-terminated phoneme string.
 *
 * Returns TISPEECH_OK, or TISPEECH_E_NOLANGUAGE when this build has no data
 * for `language`, TISPEECH_E_BUFFERFULL when `out` is too small, or
 * TISPEECH_E_BADPARAM / TISPEECH_E_NULLTEXT for bad arguments.
 *
 * This covers the letter-to-sound rules only. Text normalisation (numbers,
 * abbreviations) and the user dictionary run ahead of this stage in the
 * original and are NOT reconstructed, so input containing digits or
 * abbreviations will not match the original engine's output.
 */
TISPEECH_API int32_t tispeech_text_to_phonemes(uint32_t language,
                                               const char *text,
                                               char *out, int32_t out_size);

/*
 * Phoneme-to-PCM synthesis.
 *
 * NOT IMPLEMENTED. The waveform kernel is reconstructed and verified
 * bit-exact, but the stage that turns a phoneme string into the parameter
 * frames that drive it is not. This entry point exists so callers can link and
 * gate against a stable signature; it returns TISPEECH_E_NOTIMPL until the
 * frame generator lands, and must never be changed to return silence or
 * substitute audio instead.
 *
 * On success (once implemented) `*out_samples` receives a buffer owned by the
 * library, to be released with tispeech_free_samples().
 */
TISPEECH_API int32_t tispeech_synthesize(uint32_t language,
                                         const char *phonemes,
                                         uint8_t **out_samples,
                                         int32_t *out_count,
                                         int32_t *out_sample_rate);

TISPEECH_API void tispeech_free_samples(uint8_t *samples);

#ifdef __cplusplus
}
#endif
#endif /* TISPEECH_CAPI_H */
