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
 * language data cannot convert text, and synthesis needs base plus English data.
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
#define TISPEECH_E_DICTSHORT    0x1b6b /* user dictionary truncated */
#define TISPEECH_E_DICTFORMAT   0x1b6c /* not an "SVXF" user dictionary */
#define TISPEECH_E_OUTOFMEMORY  0x1b5a
#define TISPEECH_E_NOTIMPL      0xF001
#define TISPEECH_E_BUFFERFULL   0xF002

/* Language selector; same bit values as SV_LANG_* in svapi.h. */
#define TISPEECH_LANG_ENGLISH   0x1u
#define TISPEECH_LANG_SPANISH   0x2u
#define TISPEECH_LANG_GERMAN    0x4u

/* Capability bits returned by tispeech_capabilities(). */
#define TISPEECH_CAP_TEXT_TO_PHONEMES 0x1u /* text conversion is built */
#define TISPEECH_CAP_SYNTHESIS        0x2u /* phoneme -> PCM works end to end */

/* Bitmask of TISPEECH_CAP_*. Cheap, side-effect free, safe to call first. */
TISPEECH_API uint32_t tispeech_capabilities(void);

/* Bitmask of TISPEECH_LANG_* whose rule data was compiled into this build.
 * Zero when the build was configured without an original language DLL. */
TISPEECH_API uint32_t tispeech_languages(void);

/* Bitmask of TISPEECH_LANG_* that tispeech_synthesize() accepts. Zero unless
 * TISPEECH_CAP_SYNTHESIS is set; Spanish needs TISPAN32 as well as TIBASE32
 * and TIENG32 at build time. */
TISPEECH_API uint32_t tispeech_synthesis_languages(void);

/* Human-readable build description, for logs and the about box. Static
 * storage; the caller must not free it. */
TISPEECH_API const char *tispeech_build_info(void);

/*
 * Grapheme-to-phoneme conversion. `text` is NUL-terminated UTF-8 restricted to
 * Latin-1 (the rule tables are 8-bit). Normalisation is locale-independent.
 * Invalid UTF-8 and characters above U+00FF return TISPEECH_E_BADPARAM. The result is written
 * to `out` as a NUL-terminated phoneme string.
 *
 * Returns TISPEECH_OK, or TISPEECH_E_NOLANGUAGE when this build has no data
 * for `language`, TISPEECH_E_BUFFERFULL when `out` is too small, or
 * TISPEECH_E_BADPARAM / TISPEECH_E_NULLTEXT for bad arguments.
 *
 * English uses SVTextToPhon's normalisation, exception dictionary, number
 * expansion, letter-to-sound and default-stress stages, retaining original
 * spacing. English input is limited to 514 decoded Latin-1 bytes; longer
 * input returns TISPEECH_E_BADPARAM instead of the original's silent empty
 * output. Spanish is TISPAN32's front end, numbers included, when the build
 * has TIENG32 as well as TISPAN32, and letter-to-sound rules only when it has
 * TISPAN32 alone. For user dictionaries see tispeech_text_to_phonemes_ex(). On failure, a valid output buffer is
 * cleared; callers can grow it and retry TISPEECH_E_BUFFERFULL.
 */
TISPEECH_API int32_t tispeech_text_to_phonemes(uint32_t language,
                                               const char *text,
                                               char *out, int32_t out_size);

/*
 * User dictionaries (SVLoadUserDictionary's "SVXF" files). `bytes` is the
 * whole file, already read by the caller: the library does no file I/O.
 * Returns TISPEECH_OK with `*out_dict` set, or TISPEECH_E_DICTSHORT /
 * TISPEECH_E_DICTFORMAT (the original's 0x1b6b / 0x1b6c), TISPEECH_E_BADPARAM
 * or TISPEECH_E_OUTOFMEMORY with `*out_dict` NULL. A dictionary is immutable
 * once loaded and may be shared between threads. Free it with
 * tispeech_userdict_free(); NULL is a no-op.
 */
typedef struct tispeech_userdict tispeech_userdict;

TISPEECH_API int32_t tispeech_userdict_load(const uint8_t *bytes, int32_t size,
                                            tispeech_userdict **out_dict);
TISPEECH_API void tispeech_userdict_free(tispeech_userdict *dict);

/*
 * As tispeech_text_to_phonemes(), consulting `dict` (may be NULL) before the
 * built-in exceptions, numbers and rules, exactly where the original does.
 * A dictionary with a language whose full front end this build lacks (Spanish
 * without TIENG32) returns TISPEECH_E_NOTIMPL.
 */
TISPEECH_API int32_t tispeech_text_to_phonemes_ex(uint32_t language,
                                                  const char *text,
                                                  const tispeech_userdict *dict,
                                                  char *out, int32_t out_size);

/*
 * SVTextToPhon's flag 8: put "@w<offset>" before each word, where <offset> is
 * the word's index in `text` counted in decoded Latin-1 characters (so, since
 * only Latin-1 is accepted, in UTF-16 code units too). Synthesis turns each
 * mark into a TISPEECH_EVENT_WORD carrying that offset. The original's audio
 * is identical with and without the marks.
 */
#define TISPEECH_TEXT_WORD_MARKS 0x8u

/*
 * As tispeech_text_to_phonemes_ex(), with SVTextToPhon flags (only
 * TISPEECH_TEXT_WORD_MARKS). With word marks the text is converted in one
 * SVTextToPhon call, grown until it fits, rather than through SVTTS's chunk
 * loop: the original restarts offsets at each chunk and can drop a word at a
 * seam (REVERSING.md, "Spanish numbers"). Needs a language's full front end;
 * a matcher-only build returns TISPEECH_E_NOTIMPL for a nonzero `flags`.
 */
TISPEECH_API int32_t tispeech_text_to_phonemes_flags(uint32_t language,
                                                     const char *text,
                                                     const tispeech_userdict *dict,
                                                     uint32_t flags,
                                                     char *out, int32_t out_size);

/*
 * Phoneme-to-PCM synthesis: SVNarrate's pipeline, reconstructed. `phonemes`
 * is a SoftVoice phoneme string (the alphabet SVTextToPhon produces, e.g.
 * " /HEH5LOW WER5LD"). The output is unsigned 8-bit mono PCM at
 * `*out_sample_rate` (11025), laid out exactly as the original's waveOut
 * stream: each sentence starts with 0x2000 samples, then 0x1000 per buffer,
 * the last one padded with silence (0x80).
 *
 * Verified sample-for-sample against the original engine run under an
 * emulator (tools/verify_narrate.py, tools/verify_spanish.py). Languages:
 * tispeech_synthesis_languages(); the voice is SVOpenSpeech's default (row 0).
 *
 * Returns TISPEECH_OK, TISPEECH_E_NOTIMPL when this build has no synthesis
 * data (see tispeech_capabilities()) or the string uses an inline-command
 * form not yet reconstructed ("{...}"), TISPEECH_E_NOLANGUAGE for a language
 * not in tispeech_synthesis_languages(), TISPEECH_E_BADPARAM for an unknown
 * phoneme name, or
 * TISPEECH_E_OUTOFMEMORY. On success `*out_samples` is owned by the library;
 * release it with tispeech_free_samples().
 */
TISPEECH_API int32_t tispeech_synthesize(uint32_t language,
                                         const char *phonemes,
                                         uint8_t **out_samples,
                                         int32_t *out_count,
                                         int32_t *out_sample_rate);

/* Voice controls. -1 preserves the selected personality's value. Personality
 * is 0..19; pitch 10..2000, rate 20..500, voicing 0..2, F0 style 0..4,
 * F0 range/perturb 0..500, vowel factor 0..65535, glottal source 0..8.
 * The default ABI above remains unchanged; NULL options selects voice row 0. */
typedef struct tispeech_voice_options {
    int32_t personality, pitch, rate, voicing, f0_style;
    int32_t f0_range, f0_perturb, vowel_factor, glottal_source;
} tispeech_voice_options;

TISPEECH_API int32_t tispeech_synthesize_ex(uint32_t language,
    const char *phonemes, const tispeech_voice_options *options,
    uint8_t **out_samples, int32_t *out_count, int32_t *out_sample_rate);

TISPEECH_API void tispeech_free_samples(uint8_t *samples);

/*
 * Events: what the original's renderer reports while it speaks
 * (TIBASE32 0x1c004543..0x1c004605), which it posts to the application window
 * as the audio plays. Here each carries the sample it belongs to, so a player
 * can raise it at the right moment.
 */
#define TISPEECH_EVENT_WORD      0x3ebu /* an @w mark or {wordsync n}: value = offset / n */
#define TISPEECH_EVENT_SENTENCE  0x3ecu /* first phoneme after a pause */
#define TISPEECH_EVENT_SYLLABLE  0x3edu /* syllable start */
#define TISPEECH_EVENT_PHONEME   0x3eeu /* phoneme start: value = phoneme code */
#define TISPEECH_EVENT_USYNC     0x3efu /* {usync n}: value = n & 0xff */
#define TISPEECH_EVENT_MOUTH     0x3f0u /* mouth shape changed: value = shape, 1..10 */

/* Which optional events to report (SVNarrate's flags 1/2/4/8). Word and
 * usync events need no enable bit; they come from the phoneme string. For
 * the sentence and syllable events the original's value is whatever the
 * report before it left behind, and is reproduced as such. */
#define TISPEECH_EVENTS_SENTENCE 0x1u
#define TISPEECH_EVENTS_SYLLABLE 0x2u
#define TISPEECH_EVENTS_PHONEME  0x4u
#define TISPEECH_EVENTS_MOUTH    0x8u

typedef struct tispeech_event {
    int32_t sample;   /* index in the returned PCM where it takes effect */
    int32_t time_ms;  /* the original's own timestamp: ms since the sentence began */
    uint16_t code;    /* TISPEECH_EVENT_* */
    uint16_t value;
} tispeech_event;

/*
 * tispeech_synthesize_ex() that also returns the events, in order, as an
 * array the caller releases with tispeech_free_events() (NULL when there
 * are none). `events` is a mask of TISPEECH_EVENTS_*; other bits are
 * TISPEECH_E_BADPARAM. The PCM is identical to tispeech_synthesize_ex()'s.
 * Verified event for event against the original (verify_narrate.py --events).
 */
TISPEECH_API int32_t tispeech_synthesize_events(uint32_t language,
    const char *phonemes, const tispeech_voice_options *options, uint32_t events,
    uint8_t **out_samples, int32_t *out_count, int32_t *out_sample_rate,
    tispeech_event **out_events, int32_t *out_event_count);

TISPEECH_API void tispeech_free_events(tispeech_event *events);

#ifdef __cplusplus
}
#endif
#endif /* TISPEECH_CAPI_H */
