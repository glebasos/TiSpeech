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

#ifdef TISPEECH_HAVE_FRONTEND_SPAN
#  define HAVE_FRONTEND_SPAN 1
#else
#  define HAVE_FRONTEND_SPAN 0
#endif

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
    /* Expected strings are the original SVTextToPhon's (TIENG32 / TISPAN32
     * under tools/sv_emu.py); the matcher-only path agrees with the matcher. */
    const char *cafe = language == TISPEECH_LANG_ENGLISH ? " KAEFEY4"
                     : HAVE_FRONTEND_SPAN ? " KAAFEH5" : expected;
    CHECK(tispeech_text_to_phonemes(language, "caf\xc3\xa9", output, sizeof(output)) == TISPEECH_OK);
    CHECK(strcmp(output, cafe) == 0);
    CHECK(tispeech_text_to_phonemes(language, "CAF\xc3\x89", output, sizeof(output)) == TISPEECH_OK);
    CHECK(strcmp(output, cafe) == 0);
    CHECK(tispeech_text_to_phonemes(language, "hello world", expected, sizeof(expected)) == TISPEECH_OK);
    CHECK(tispeech_text_to_phonemes(language, "\thello\xc2\xa0world\n", output, sizeof(output)) == TISPEECH_OK);
    if (language == TISPEECH_LANG_ENGLISH)
        CHECK(strcmp(output, "  /HEH5LOW WER5LD") == 0); /* Original keeps tab spacing. */
    else if (HAVE_FRONTEND_SPAN)
        CHECK(strcmp(output, "  EY5OH WOH5RLD") == 0);
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
#  ifdef TISPEECH_HAVE_SYNTH_SPAN
    CHECK(tispeech_synthesis_languages() == (TISPEECH_LANG_ENGLISH | TISPEECH_LANG_SPANISH));
    CHECK(tispeech_synthesize(TISPEECH_LANG_SPANISH, " OH5LAA MUW5NDOH", &samples, &count, &rate) == TISPEECH_OK);
    CHECK(samples != NULL && count > 0x2000 && rate == 11025);
    tispeech_free_samples(samples);
#  else
    CHECK(tispeech_synthesis_languages() == TISPEECH_LANG_ENGLISH);
    CHECK(tispeech_synthesize(TISPEECH_LANG_SPANISH, " OHLAA", &samples, &count, &rate) == TISPEECH_E_NOLANGUAGE);
#  endif
    CHECK(tispeech_synthesize(TISPEECH_LANG_ENGLISH | TISPEECH_LANG_SPANISH, " OHLAA", &samples, &count, &rate)
          == TISPEECH_E_NOLANGUAGE);
    CHECK(tispeech_synthesize(TISPEECH_LANG_ENGLISH, " XYZ", &samples, &count, &rate) == TISPEECH_E_BADPARAM);
    CHECK(samples == NULL && count == 0 && rate == 0);
    CHECK(tispeech_synthesize(TISPEECH_LANG_ENGLISH, " \xc3\xa9", &samples, &count, &rate) == TISPEECH_E_BADPARAM);
#else
    CHECK(tispeech_synthesis_languages() == 0);
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
    {
        /* A hand-built "SVXF" dictionary: HELLO -> " XYZZY" in bucket 'H',
         * every other bucket pointing at the terminator (userdict.h). */
        static const char word[] = "HELLO", phon[] = " XYZZY";
        uint8_t file[144 + 32] = {'S', 'V', 'X', 'F'};
        uint8_t *table = file + 144;
        uint32_t wl = sizeof word - 1, pl = sizeof phon - 1, term = 3 + wl + pl;
        uint32_t table_len = term + 3;
        tispeech_userdict *dict = (tispeech_userdict *)&file; /* poisoned */
        char plain[256];
        file[24] = 1;
        memcpy(file + 28, &table_len, 4);
        for (int b = 0; b < 28; b++)
            memcpy(file + 32 + 4 * b, b == 'H' - 'A' ? &(uint32_t){0} : &term, 4);
        table[0] = (uint8_t)(wl + pl);
        table[1] = (uint8_t)wl;
        table[2] = 0x02; /* ordinary entry, compared against normalised text */
        memcpy(table + 3, word, wl);
        memcpy(table + 3 + wl, phon, pl);

        CHECK(tispeech_userdict_load(file, 144 + (int32_t)table_len, &dict) == TISPEECH_OK);
        CHECK(dict != NULL);
        CHECK(tispeech_text_to_phonemes_ex(1, "hello world", dict, output, sizeof output) == TISPEECH_OK);
        CHECK(strstr(output, "XYZZY") != NULL && strstr(output, "WER5LD") != NULL);
        CHECK(strstr(output, "HEH5LOW") == NULL);
        /* Prefix only: HELLOS is a different word and misses the entry. */
        CHECK(tispeech_text_to_phonemes_ex(1, "hellos", dict, output, sizeof output) == TISPEECH_OK);
        CHECK(strstr(output, "XYZZY") == NULL);
        /* NULL dictionary is exactly the plain entry point. */
        CHECK(tispeech_text_to_phonemes_ex(1, "hello world", NULL, output, sizeof output) == TISPEECH_OK);
        CHECK(tispeech_text_to_phonemes(1, "hello world", plain, sizeof plain) == TISPEECH_OK);
        CHECK(strcmp(output, plain) == 0);
        CHECK(tispeech_text_to_phonemes_ex(TISPEECH_LANG_SPANISH, "hola", dict, output, sizeof output)
              == (HAVE_FRONTEND_SPAN ? TISPEECH_OK : languages & TISPEECH_LANG_SPANISH
                  ? TISPEECH_E_NOTIMPL : TISPEECH_E_NOLANGUAGE));
        tispeech_userdict_free(dict);

        CHECK(tispeech_userdict_load(file, 100, &dict) == TISPEECH_E_DICTSHORT && dict == NULL);
        file[0] = 'X';
        CHECK(tispeech_userdict_load(file, 144 + (int32_t)table_len, &dict) == TISPEECH_E_DICTFORMAT);
        CHECK(dict == NULL);
        CHECK(tispeech_userdict_load(NULL, 0, &dict) == TISPEECH_E_BADPARAM);
        CHECK(tispeech_userdict_load(file, 0, NULL) == TISPEECH_E_BADPARAM);
        tispeech_userdict_free(NULL);
    }
#else
    CHECK((languages & TISPEECH_LANG_ENGLISH) == 0);
    CHECK(tispeech_text_to_phonemes(TISPEECH_LANG_ENGLISH, "hello", output, sizeof(output)) == TISPEECH_E_NOLANGUAGE);
#endif
#ifdef TISPEECH_HAVE_SPAN
    CHECK((languages & TISPEECH_LANG_SPANISH) != 0);
    CHECK(strstr(tispeech_build_info(), "Spanish") != NULL);
    CHECK(check_language(TISPEECH_LANG_SPANISH, &sv_lang_data_span) == 0);
#  if HAVE_FRONTEND_SPAN
    /* The original SVTextToPhon, including default stress. */
    CHECK(tispeech_text_to_phonemes(TISPEECH_LANG_SPANISH, "hola mundo", output, sizeof(output)) == TISPEECH_OK);
    CHECK(strcmp(output, " OH5LAA MUW5NDOH") == 0);
    /* Spanish cardinal numbers are not reconstructed. */
    CHECK(tispeech_text_to_phonemes(TISPEECH_LANG_SPANISH, "hola 42", output, sizeof(output)) == TISPEECH_E_NOTIMPL);
#  else
    /* Confirmed by TISPAN32!0x1c4062f0 oracle probes, not another C path. */
    CHECK(tispeech_text_to_phonemes(TISPEECH_LANG_SPANISH, "hola mundo", output, sizeof(output)) == TISPEECH_OK);
    CHECK(strcmp(output, "OHLAA MUWNDOH") == 0);
    CHECK(tispeech_text_to_phonemes(TISPEECH_LANG_SPANISH, "caf\xc3\xa9", output, sizeof(output)) == TISPEECH_OK);
    CHECK(strcmp(output, "KAAFEH5") == 0);
#  endif
#else
    CHECK((languages & TISPEECH_LANG_SPANISH) == 0);
    CHECK(tispeech_text_to_phonemes(TISPEECH_LANG_SPANISH, "hola", output, sizeof(output)) == TISPEECH_E_NOLANGUAGE);
#endif
    puts("PASS: native ABI capability, encoding, argument and failure tests");
    return 0;
}
