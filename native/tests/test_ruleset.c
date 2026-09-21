/*
 * test_ruleset.c — deterministic tests for the reconstructed rule matcher.
 *
 * The expectations here are GROUND TRUTH in the strict sense: each one is a
 * whole-word rule that appears verbatim in the extracted table, so the right
 * answer is not a matter of opinion. If one of these fails, the matcher is
 * wrong, not the test.
 *
 * These do NOT prove agreement with the original engine on arbitrary input.
 * That needs a differential run against TIENG32 and is tracked in REVERSING.md.
 */

#include "tispeech/ruleset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect(const char *word, const char *want)
{
    char in[256];
    char got[1024];

    snprintf(in, sizeof(in), " %s ", word);
    if (sv_rules_apply(&sv_lang_data_eng, in + 1, got, sizeof(got), 0) != 0) {
        printf("FAIL %-12s overflow\n", word);
        failures++;
        return;
    }
    if (strcmp(got, want) != 0) {
        printf("FAIL %-12s want \"%s\" got \"%s\"\n", word, want, got);
        failures++;
    } else {
        printf("ok   %-12s -> %s\n", word, got);
    }
}

/*
 * Regression: sv_rules_apply must not read before `in`, even when the
 * caller's buffer starts exactly at the word with no leading space. That is
 * exactly how the capi.c ABI calls in (a word carved out of a larger, but
 * not necessarily left-padded, decoded-text buffer) and ASan caught a
 * one-byte underflow here for "hello" -- see sv_peek() in src/ruleset.c.
 *
 * The word is heap-allocated at exactly strlen+1 bytes so its ASan redzone
 * sits immediately before byte 0: any regression crashes this test under
 * -fsanitize=address instead of merely reading unrelated stack/heap bytes.
 */
static void expect_unpadded(const char *word)
{
    size_t n = strlen(word);
    char *in = malloc(n + 1);
    char got[1024];

    if (in == NULL) {
        printf("FAIL %-12s (unpadded) out of memory\n", word);
        failures++;
        return;
    }
    memcpy(in, word, n + 1);
    if (sv_rules_apply(&sv_lang_data_eng, in, got, sizeof(got), 0) != 0) {
        printf("FAIL %-12s (unpadded) overflow\n", word);
        failures++;
    } else {
        printf("ok   %-12s (unpadded, no leading byte) -> %s\n", word, got);
    }
    free(in);
}

#ifdef TISPEECH_HAVE_SPAN
/* Same shape as expect(), against sv_lang_data_span instead. Kept separate
 * rather than parameterising expect() so a missing -DTISPEECH_HAVE_SPAN
 * (no TISPAN32.DLL available) compiles this whole block out instead of
 * failing to link sv_lang_data_span. */
static void expect_span(const char *word, const char *want)
{
    char in[256];
    char got[1024];

    snprintf(in, sizeof(in), " %s ", word);
    if (sv_rules_apply(&sv_lang_data_span, in + 1, got, sizeof(got), 0) != 0) {
        printf("FAIL %-12s (span) overflow\n", word);
        failures++;
        return;
    }
    if (strcmp(got, want) != 0) {
        printf("FAIL %-12s (span) want \"%s\" got \"%s\"\n", word, want, got);
        failures++;
    } else {
        printf("ok   %-12s (span) -> %s\n", word, got);
    }
}
#endif

int main(void)
{
    /* Whole-word rules, quoted from the extracted English table. */
    expect("AND", "AEND");
    expect("ARE", "AAR");
    expect("AS", "AEZ");
    expect("AT", "AET");
    expect("ABOVE", "AHBAH4V");
    expect("AROUND", "AHRAW4ND");
    expect("BOTH", "BOW8TH");
    expect("BREAK", "BREY4K");
    expect("LEVEL", "LEH4VUL");
    expect("SEVEN", "SEH4VIN");
    expect("WITHOUT", "WIHTHAW6T");
    expect("YOUNG", "YAH5NX");

    /* First-match-wins, demonstrated. The table contains [BEFORE]=BIXFOH3R,
     * but [BE]^#=BIX appears 126 bytes earlier and fires first, so the whole-
     * word rule is unreachable dead data in the original ruleset. Asserting
     * the reachable answer here documents the ordering semantics; it is also
     * the top item on the differential-testing list in REVERSING.md, because
     * only a run against TIENG32 itself can settle it for certain. */
    expect("BEFORE", "BIXFOHR");

    /* No byte before `in` is allocated at all; must not underflow. */
    expect_unpadded("HELLO");
    expect_unpadded("A");
    expect_unpadded("STRENGTH");

#ifdef TISPEECH_HAVE_SPAN
    /* Whole-word rules, quoted from the extracted Spanish table, and
     * confirmed byte-exact against the real TISPAN32.DLL matcher via
     * `tools/verify_ruleset.py --language span` (60,000+ real Spanish
     * words, tens of thousands of randomized stress strings, 0 mismatches
     * at the time this was written -- see include/tispeech/ruleset.h). */
    expect_span("HOLA", "OHLAA");
    expect_span("CASA", "KAASAA");
    expect_span("GATO", "GAATOH");
    expect_span("NINO", "NIYNOH");
#endif

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall passed\n");
    return 0;
}
