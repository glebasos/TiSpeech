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
    CHECK(strcmp(output, expected) == 0);
    CHECK(tispeech_text_to_phonemes(language, "CAF\xc3\x89", output, sizeof(output)) == TISPEECH_OK);
    CHECK(strcmp(output, expected) == 0);
    CHECK(tispeech_text_to_phonemes(language, "hello world", expected, sizeof(expected)) == TISPEECH_OK);
    CHECK(tispeech_text_to_phonemes(language, "\thello\xc2\xa0world\n", output, sizeof(output)) == TISPEECH_OK);
    CHECK(strcmp(output, expected) == 0);
    return 0;
}
#endif

int main(void)
{
    char output[64];
    uint8_t *samples = (uint8_t *)output;
    int32_t count = 123, rate = 123;
    uint32_t languages = tispeech_languages();

    CHECK((tispeech_capabilities() & TISPEECH_CAP_SYNTHESIS) == 0);
    CHECK(!!(tispeech_capabilities() & TISPEECH_CAP_TEXT_TO_PHONEMES) == !!languages);
    CHECK(tispeech_build_info() != NULL);
    CHECK(tispeech_text_to_phonemes(1, "hello", NULL, 1) == TISPEECH_E_BADPARAM);
    CHECK(tispeech_text_to_phonemes(1, "hello", output, 0) == TISPEECH_E_BADPARAM);
    CHECK(tispeech_text_to_phonemes(1, NULL, output, sizeof(output)) == TISPEECH_E_NULLTEXT);
    CHECK(output[0] == '\0');
    CHECK(tispeech_text_to_phonemes(3, "hello", output, sizeof(output)) == TISPEECH_E_NOLANGUAGE);
    CHECK(output[0] == '\0');
    CHECK(tispeech_synthesize(1, "HELLO", &samples, &count, &rate) == TISPEECH_E_NOTIMPL);
    CHECK(samples == NULL && count == 0 && rate == 0);
    CHECK(tispeech_synthesize(1, NULL, NULL, NULL, NULL) == TISPEECH_E_NOTIMPL);
    tispeech_free_samples(NULL);

#ifdef TISPEECH_HAVE_ENG
    CHECK((languages & TISPEECH_LANG_ENGLISH) != 0);
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
