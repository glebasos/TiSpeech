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

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall passed\n");
    return 0;
}
