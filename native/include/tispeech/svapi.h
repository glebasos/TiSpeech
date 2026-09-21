/*
 * svapi.h — public API surface of the SoftVoice speech engine (TIBASE32.DLL).
 *
 * PROVENANCE
 * ----------
 * Every declaration below is derived from the PE export directory and the
 * stdcall name decoration of the original TIBASE32.DLL (79,872 bytes, built
 * 1996-11-18, i386, ImageBase 0x1C000000). The decoration suffix `@N` gives
 * the exact callee-popped argument byte count, which pins down each arity.
 *
 * No SoftVoice code is reproduced here. This header describes an interface.
 *
 * STATUS: interface reconstruction complete (30/30 exports accounted for).
 *         Implementations live in src/ and are NOT complete — see REVERSING.md
 *         for the per-function status table. Nothing here is stubbed to fake
 *         success; unimplemented entry points return SV_E_NOTIMPL.
 */

#ifndef TISPEECH_SVAPI_H
#define TISPEECH_SVAPI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sv_speech sv_speech_t;
typedef uint32_t sv_error_t;

/* ---------------------------------------------------------------------------
 * Error codes.
 *
 * Values and message text were recovered from the literal string table inside
 * TIBASE32.DLL together with the numeric constants already confirmed against
 * the running engine by the TiSpeech wrapper (TiSpeech/TiSpeechEngine.cs).
 * The ordering of the string table matches the numeric ordering.
 * ------------------------------------------------------------------------- */
#define SV_OK                    0x0000u
#define SV_E_OUTOFMEMORY         0x1b5au /* "Internal Error - Can't allocate ..."  */
#define SV_E_BUSY                0x1b61u
#define SV_E_BADPARAM            0x1b62u /* "Error index out of range"             */
#define SV_E_NODEVICE            0x1b63u /* "No waveOut devices installed"         */
#define SV_E_DEVICEOPEN          0x1b64u /* "Can't open waveOut device"            */
#define SV_E_ALREADYSPEAKING     0x1b66u
#define SV_E_WINDOWCREATE        0x1b69u /* "Internal Error - Can't create window" */
#define SV_E_NOLANGUAGE          0x1b6eu /* language DLL / language module missing */
#define SV_E_NULLTEXT            0x1b70u

/* Reserved by this reimplementation; never returned by the original engine.
 * Signals "this code path has not been reverse-engineered yet". */
#define SV_E_NOTIMPL             0xF001u

/* Language bitmask, as passed to sv_open_speech(). Confirmed from
 * TiSpeech/TiEnums.cs (TiLanguageFlags) and from the SVSetLanguage decompile:
 * SVSetLanguage takes the SAME bitmask, not a zero-based index. */
#define SV_LANG_ENGLISH  0x1u
#define SV_LANG_SPANISH  0x2u
#define SV_LANG_GERMAN   0x4u

/* sv_narrate() flags. 0x20 = interrupt speech in progress. */
#define SV_NARRATE_INTERRUPT     0x20u
#define SV_NARRATE_PREPHONEMISED 0x40000000u

/* ---------------------------------------------------------------------------
 * Lifecycle. Native export equivalents are named in each comment.
 * ------------------------------------------------------------------------- */

/* _SVOpenSpeech@20 */
sv_error_t sv_open_speech(sv_speech_t **out, void *notify, uint32_t device_id,
                          uint32_t language_flags, uint32_t reserved);
/* _SVCloseSpeech@4  */ sv_error_t sv_close_speech(sv_speech_t *s);
/* _SVAbort@4        */ sv_error_t sv_abort(sv_speech_t *s);
/* _SVPause@4        */ sv_error_t sv_pause(sv_speech_t *s);
/* _SVResume@4       */ sv_error_t sv_resume(sv_speech_t *s);

/* ---------------------------------------------------------------------------
 * Text and phoneme paths.
 * ------------------------------------------------------------------------- */

/* _SVTextToPhon@24 — grapheme-to-phoneme only; no audio. This is the one
 * synthesis-pipeline stage that IS implemented (see src/ruleset.c). */
sv_error_t sv_text_to_phon(sv_speech_t *s, const char *text,
                           char **phon_out, int *phon_len_out,
                           uint32_t flags, uint32_t reserved);

/* _SVNarrate@20 */
sv_error_t sv_narrate(sv_speech_t *s, const char *phonemes, void *notify,
                      uint32_t flags, uint32_t reserved);

/* _SVTTS@32 — sv_text_to_phon() followed by sv_narrate(). */
sv_error_t sv_tts(sv_speech_t *s, const char *text,
                  char **phon_out, int *phon_len_out, void *notify,
                  uint32_t flags, uint32_t narrate_flags, uint32_t reserved);

/* ---------------------------------------------------------------------------
 * Voice parameters. All are `(handle, value)` with 8 bytes of arguments.
 * ------------------------------------------------------------------------- */
/* _SVSetPersonality@8   */ sv_error_t sv_set_personality(sv_speech_t *s, uint16_t idx);
/* _SVSetPitch@8         */ sv_error_t sv_set_pitch(sv_speech_t *s, int32_t v);
/* _SVSetRate@8          */ sv_error_t sv_set_rate(sv_speech_t *s, int32_t v);
/* _SVSetGender@8        */ sv_error_t sv_set_gender(sv_speech_t *s, int32_t v);
/* _SVSetLanguage@8      */ sv_error_t sv_set_language(sv_speech_t *s, int32_t lang_bit);
/* _SVSetF0Range@8       */ sv_error_t sv_set_f0_range(sv_speech_t *s, int32_t v);
/* _SVSetF0Style@8       */ sv_error_t sv_set_f0_style(sv_speech_t *s, int32_t v);
/* _SVSetF0Perturb@8     */ sv_error_t sv_set_f0_perturb(sv_speech_t *s, int32_t v);
/* _SVSetGlottalSource@8 */ sv_error_t sv_set_glottal_source(sv_speech_t *s, int32_t v);
/* _SVSetSpeakingMode@8  */ sv_error_t sv_set_speaking_mode(sv_speech_t *s, int32_t v);
/* _SVSetVoicingMode@8   */ sv_error_t sv_set_voicing_mode(sv_speech_t *s, int32_t v);
/* _SVSetVowelFactor@8   */ sv_error_t sv_set_vowel_factor(sv_speech_t *s, int32_t v);
/* _SVSetAFBias@8        */ sv_error_t sv_set_af_bias(sv_speech_t *s, int32_t v);
/* _SVSetAHBias@8        */ sv_error_t sv_set_ah_bias(sv_speech_t *s, int32_t v);
/* _SVSetAVBias@8        */ sv_error_t sv_set_av_bias(sv_speech_t *s, int32_t v);

/* ---------------------------------------------------------------------------
 * Info and dictionary.
 * ------------------------------------------------------------------------- */
/* _SVGetVersionInfo@4         */ sv_error_t sv_get_version_info(sv_speech_t *s);
/* _SVGetAvailableLanguages@8  */ sv_error_t sv_get_available_languages(sv_speech_t *s, uint32_t *out);
/* _SVGetErrorText@12          */ sv_error_t sv_get_error_text(sv_error_t code, char *buf, int buflen);
/* _SVGetVoiceInfo@16          */ sv_error_t sv_get_voice_info(sv_speech_t *s, int32_t a, void *b, int32_t c);
/* _SVSetVoiceInfo@12          */ sv_error_t sv_set_voice_info(sv_speech_t *s, int32_t a, void *b);
/* _SVLoadUserDictionary@8     */ sv_error_t sv_load_user_dictionary(sv_speech_t *s, const char *path);
/* _SVUnloadUserDictionary@8   */ sv_error_t sv_unload_user_dictionary(sv_speech_t *s, const char *path);

#ifdef __cplusplus
}
#endif
#endif /* TISPEECH_SVAPI_H */
