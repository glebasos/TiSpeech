/*
 * svsay — synthesise a SoftVoice phoneme string (or plain text, through the
 * letter-to-sound stage) to a WAV file with libtispeech.
 *
 *   svsay [-t] "PHONEMES OR TEXT" out.wav
 *
 * Without -t the argument is a phoneme string, e.g. " /HEH5LOW WER5LD", and
 * the output is sample-exact with the original engine. With -t the text goes
 * through the reconstructed English front end, including number expansion,
 * exception pronunciations and stress. Text is UTF-8 restricted to Latin-1,
 * at most 514 characters. Inline synthesis commands are not yet supported.
 */

#include "tispeech/capi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(FILE *f, uint32_t v)
{
    unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8),
                          (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
    fwrite(b, 1, 4, f);
}

static void put16(FILE *f, uint16_t v)
{
    unsigned char b[2] = {(unsigned char)v, (unsigned char)(v >> 8)};
    fwrite(b, 1, 2, f);
}

int main(int argc, char **argv)
{
    int text = 0, i = 1;
    if (argc > 1 && strcmp(argv[1], "-t") == 0) {
        text = 1;
        i++;
    }
    if (argc - i != 2) {
        fprintf(stderr, "usage: svsay [-t] \"PHONEMES OR TEXT\" out.wav\n");
        return 2;
    }
    if (!(tispeech_capabilities() & TISPEECH_CAP_SYNTHESIS)) {
        fprintf(stderr, "svsay: %s\n", tispeech_build_info());
        return 1;
    }
    const char *phon = argv[i];
    char buf[8192];
    if (text) {
        int32_t rc = tispeech_text_to_phonemes(TISPEECH_LANG_ENGLISH, argv[i], buf, sizeof buf);
        if (rc != TISPEECH_OK) {
            fprintf(stderr, "svsay: text_to_phonemes failed (%#x)\n", (unsigned)rc);
            return 1;
        }
        phon = buf;
        fprintf(stderr, "phonemes: %s\n", phon);
    }
    uint8_t *pcm;
    int32_t n, rate;
    int32_t rc = tispeech_synthesize(TISPEECH_LANG_ENGLISH, phon, &pcm, &n, &rate);
    if (rc != TISPEECH_OK) {
        fprintf(stderr, "svsay: synthesize failed (%#x)\n", (unsigned)rc);
        return 1;
    }
    FILE *f = fopen(argv[i + 1], "wb");
    if (!f) {
        perror(argv[i + 1]);
        tispeech_free_samples(pcm);
        return 1;
    }
    fwrite("RIFF", 1, 4, f);
    put32(f, 36u + (uint32_t)n);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);             /* PCM */
    put16(f, 1);             /* mono */
    put32(f, (uint32_t)rate);
    put32(f, (uint32_t)rate);
    put16(f, 1);
    put16(f, 8);
    fwrite("data", 1, 4, f);
    put32(f, (uint32_t)n);
    fwrite(pcm, 1, (size_t)n, f);
    fclose(f);
    tispeech_free_samples(pcm);
    fprintf(stderr, "%d samples at %d Hz\n", n, rate);
    return 0;
}
