/*
 * svphon — run the reconstructed letter-to-sound rules over a word.
 *
 * This is the development oracle harness: it exercises exactly the stage that
 * has been reverse-engineered so far and nothing else. It does NOT synthesise
 * audio, because the synthesis path is not reconstructed yet.
 *
 *   svphon HELLO
 *   svphon --upper "the quick brown fox"
 */

#include "tispeech/ruleset.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    char in[1024];
    char out[4096];
    int argi = 1;
    int do_upper = 0;

    if (argi < argc && strcmp(argv[argi], "--upper") == 0) {
        do_upper = 1;
        argi++;
    }
    if (argi >= argc) {
        fprintf(stderr, "usage: svphon [--upper] <text>\n");
        return 2;
    }

    /* The ruleset is written entirely in upper case and the original front end
     * upper-cases before rule matching. Leading and trailing spaces matter:
     * many rules anchor on a word boundary. */
    in[0] = ' ';
    {
        size_t n = 1;
        for (int i = argi; i < argc && n < sizeof(in) - 2; i++) {
            if (i > argi)
                in[n++] = ' ';
            for (const char *s = argv[i]; *s && n < sizeof(in) - 2; s++)
                in[n++] = (char)toupper((unsigned char)*s);
        }
        in[n++] = ' ';
        in[n] = '\0';
    }
    (void)do_upper;

    {
        const char *p = in + 1;
        out[0] = '\0';
        /* Walk word by word: the matcher stops at each space by design. */
        while (*p) {
            char word[4096];
            size_t n = 0;
            if (sv_rules_apply(&sv_lang_data_eng, p, word, sizeof(word), 0) != 0) {
                fprintf(stderr, "svphon: output buffer overflow\n");
                return 1;
            }
            n = strlen(word);
            if (n == 0) {
                p++;
                continue;
            }
            if (strlen(out) + n + 1 >= sizeof(out))
                break;
            strcat(out, word);
            /* Advance past the characters the rules consumed. Without a
             * consumed-length out-parameter we re-scan; this harness favours
             * clarity over speed. */
            while (*p && *p != ' ')
                p++;
            while (*p == ' ')
                p++;
            if (*p)
                strcat(out, " ");
        }
        printf("%s\n", out);
    }
    return 0;
}
