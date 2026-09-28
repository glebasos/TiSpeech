#include "tispeech/capi.h"
#include "tispeech/ruleset.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#if defined(TISPEECH_HAVE_ENG) || defined(TISPEECH_HAVE_SPAN)
static int check_language(uint32_t language, const sv_ruleset_t *rules)
{
    const char accented[] = " CAF\xc9 ";
    char output[4096];
    char expected[4096];
    static const char *invalid[] = {
        "\xc3", "\xc3X", "\x80", "\xe9", "\xc0\xaf", "\xc4\x80",
        "\xe2\x82\xac", "\xf0\x9f\x98\x80"
    };
    size_t i;

    CHECK(tispeech_text_to_phonemes(language, "", output, sizeof(output)) == TISPEECH_OK);
    CHECK(output[0] == '\0');
    CHECK(tispeech_text_to_phonemes(language, "hello", output, 1) == TISPEECH_E_BUFFERFULL);
    CHECK(output[0] == '\0');
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        strcpy(output, "not cleared");
        CHECK(tispeech_text_to_phonemes(language, invalid[i], output, sizeof(output)) == TISPEECH_E_BADPARAM);
        CHECK(output[0] == '\0');
    }

    /* The ABI accepts UTF-8; the rule engine must receive a single Latin-1
     * byte for each accented letter, not the original UTF-8 byte pair. */
    CHECK(sv_rules_apply(rules, accented + 1, expected, sizeof(expected), 0) == 0);
    CHECK(tispeech_text_to_phonemes(language, "caf\xc3\xa9", output, sizeof(output)) == TISPEECH_OK);
    if (language == TISPEECH_LANG_ENGLISH)
        CHECK(strcmp(output, " KAEFEY4") == 0); /* Original SVTextToPhon. */
    else
        CHECK(strcmp(output, expected) == 0);
    CHECK(tispeech_text_to_phonemes(language, "CAF\xc3\x89", output, sizeof(output)) == TISPEECH_OK);
    if (language == TISPEECH_LANG_ENGLISH)
        CHECK(strcmp(output, " KAEFEY4") == 0); /* Original SVTextToPhon. */
    else
        CHECK(strcmp(output, expected) == 0);
    CHECK(tispeech_text_to_phonemes(language, "hello world", expected, sizeof(expected)) == TISPEECH_OK);
    CHECK(tispeech_text_to_phonemes(language, "\thello\xc2\xa0world\n", output, sizeof(output)) == TISPEECH_OK);
    if (language == TISPEECH_LANG_ENGLISH)
        CHECK(strcmp(output, "  /HEH5LOW WER5LD") == 0); /* Original keeps tab spacing. */
    else
        CHECK(strcmp(output, expected) == 0);
    return 0;
}
#endif

#ifdef TISPEECH_HAVE_SYNTH_ENG
#  define HAVE_SYNTH 1
#else
#  define HAVE_SYNTH 0
#endif

int main(void)
{
    char output[64];
    uint8_t *samples = (uint8_t *)output;
    int32_t count = 123, rate = 123;
    uint32_t languages = tispeech_languages();

    CHECK(!(tispeech_capabilities() & TISPEECH_CAP_SYNTHESIS) == !HAVE_SYNTH);
    CHECK(!!(tispeech_capabilities() & TISPEECH_CAP_TEXT_TO_PHONEMES) == !!languages);
    CHECK(tispeech_build_info() != NULL);
    CHECK(tispeech_text_to_phonemes(1, "hello", NULL, 1) == TISPEECH_E_BADPARAM);
    CHECK(tispeech_text_to_phonemes(1, "hello", output, 0) == TISPEECH_E_BADPARAM);
    CHECK(tispeech_text_to_phonemes(1, NULL, output, sizeof(output)) == TISPEECH_E_NULLTEXT);
    CHECK(output[0] == '\0');
    CHECK(tispeech_text_to_phonemes(3, "hello", output, sizeof(output)) == TISPEECH_E_NOLANGUAGE);
    CHECK(output[0] == '\0');
    CHECK(tispeech_synthesize(1, NULL, &samples, &count, &rate) == TISPEECH_E_NULLTEXT);
    CHECK(samples == NULL && count == 0 && rate == 0);
    CHECK(tispeech_synthesize(1, "HELLO", NULL, NULL, NULL) == TISPEECH_E_BADPARAM);
    tispeech_free_samples(NULL);
#if HAVE_SYNTH
    /* Sample-exact content is tools/verify_narrate.py's job; this checks the
     * ABI contract: buffer shape, rate, and the failure paths. */
    CHECK(tispeech_synthesize(TISPEECH_LANG_ENGLISH, " /HEH5LOW WER5LD", &samples, &count, &rate) == TISPEECH_OK);
    CHECK(samples != NULL && rate == 11025);
    CHECK(count >= 0x2000 && (count - 0x2000) % 0x1000 == 0);
    CHECK(samples[count - 1] == 0x80);
    tispeech_free_samples(samples);
    {
        tispeech_voice_options opts = {1, 200, 150, 0, 0, -1, -1, -1, -1};
        CHECK(tispeech_synthesize_ex(1, " /HEH5LOW WER5LD", &opts, &samples, &count, &rate) == TISPEECH_OK);
        CHECK(samples != NULL && count > 0 && rate == 11025);
        tispeech_free_samples(samples);
        opts.personality = 20;
        CHECK(tispeech_synthesize_ex(1, " /HEH5LOW", &opts, &samples, &count, &rate) == TISPEECH_E_BADPARAM);
        CHECK(samples == NULL && count == 0 && rate == 0);
    }
    CHECK(tispeech_synthesize(TISPEECH_LANG_SPANISH, " OHLAA", &samples, &count, &rate) == TISPEECH_E_NOLANGUAGE);
    CHECK(tispeech_synthesize(TISPEECH_LANG_ENGLISH, " XYZ", &samples, &count, &rate) == TISPEECH_E_BADPARAM);
    CHECK(samples == NULL && count == 0 && rate == 0);
    CHECK(tispeech_synthesize(TISPEECH_LANG_ENGLISH, " \xc3\xa9", &samples, &count, &rate) == TISPEECH_E_BADPARAM);
#else
    CHECK(tispeech_synthesize(1, "HELLO", &samples, &count, &rate) == TISPEECH_E_NOTIMPL);
    CHECK(samples == NULL && count == 0 && rate == 0);
#endif

#ifdef TISPEECH_HAVE_ENG
    CHECK((languages & TISPEECH_LANG_ENGLISH) != 0);
    CHECK(tispeech_text_to_phonemes(1, "hello world", output, sizeof output) == TISPEECH_OK);
    CHECK(strcmp(output, " /HEH5LOW WER5LD") == 0);
    CHECK(tispeech_text_to_phonemes(1, "$42.50", output, sizeof output) == TISPEECH_OK);
    CHECK(strcmp(output, "  FOH5RTIY TUW5 DAA5LERZ AEND FIH5FTIY SEH5NTS ") == 0);
    {
        char long_text[516];
        memset(long_text, 'a', sizeof long_text - 1);
        long_text[sizeof long_text - 1] = '\0';
        CHECK(tispeech_text_to_phonemes(1, long_text, output, sizeof output) == TISPEECH_E_BADPARAM);
        CHECK(output[0] == '\0');
    }
    CHECK(check_language(TISPEECH_LANG_ENGLISH, &sv_lang_data_eng) == 0);
#else
    CHECK((languages & TISPEECH_LANG_ENGLISH) == 0);
    CHECK(tispeech_text_to_phonemes(TISPEECH_LANG_ENGLISH, "hello", output, sizeof(output)) == TISPEECH_E_NOLANGUAGE);
#endif
#ifdef TISPEECH_HAVE_SPAN
    CHECK((languages & TISPEECH_LANG_SPANISH) != 0);
    CHECK(strstr(tispeech_build_info(), "Spanish") != NULL);
    CHECK(check_language(TISPEECH_LANG_SPANISH, &sv_lang_data_span) == 0);
    /* Confirmed by TISPAN32!0x1c4062f0 oracle probes, not another C path. */
    CHECK(tispeech_text_to_phonemes(TISPEECH_LANG_SPANISH, "hola mundo", output, sizeof(output)) == TISPEECH_OK);
    CHECK(strcmp(output, "OHLAA MUWNDOH") == 0);
    CHECK(tispeech_text_to_phonemes(TISPEECH_LANG_SPANISH, "caf\xc3\xa9", output, sizeof(output)) == TISPEECH_OK);
    CHECK(strcmp(output, "KAAFEH5") == 0);
#else
    CHECK((languages & TISPEECH_LANG_SPANISH) == 0);
    CHECK(tispeech_text_to_phonemes(TISPEECH_LANG_SPANISH, "hola", output, sizeof(output)) == TISPEECH_E_NOLANGUAGE);
#endif
    puts("PASS: native ABI capability, encoding, argument and failure tests");
    return 0;
}
