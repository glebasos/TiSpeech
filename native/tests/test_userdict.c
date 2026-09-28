/* Deterministic tests for the user dictionary, src/userdict.c.
 *
 * These are NOT the differential. tools/verify_narrate.py's
 * --user-dict-library mode is what proves the reconstruction agrees with
 * TIBASE32!_SVLoadUserDictionary@8 / _SVUnloadUserDictionary@8 and
 * TIENG32!FUN_1c2097a0, by loading synthetic dictionaries into the original
 * engine under Unicorn; it needs that emulator and a copy of the DLLs, so it
 * cannot run in an ordinary build. What lives here is the file format and
 * lookup behaviour the differential established, pinned with hand-built
 * dictionary bytes so a later edit cannot quietly undo it, and the parser's
 * own error paths. No proprietary data of any kind is needed for this file,
 * so it runs unconditionally, like test_smoothing.c and test_generator.c.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tispeech/userdict.h"

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

/* A minimal class table matching the bits userdict.c reads off TIENG32's
 * 0x1c209c20 (ruleset.h's SV_CC_ALPHA / the digit class): 0x80 alphabetic
 * for 'A'-'Z', 0x02 for '0'-'9'. Real dictionaries are looked up against the
 * language module's own table (textphon_eng.c); this is enough to drive
 * userdict.c's bucket classification and word-boundary test in isolation. */
static uint16_t g_cls[256];

static void init_cls(void)
{
    int c;
    memset(g_cls, 0, sizeof g_cls);
    for (c = 'A'; c <= 'Z'; c++)
        g_cls[c] = 0x0080;
    for (c = '0'; c <= '9'; c++)
        g_cls[c] = 0x0002;
}

/* Builds one "SVXF" dictionary file. `entries` is a flat byte string of
 * pre-built [H0][H1][H2][word][phon] records terminated by [0,0,0], already
 * placed at `bucket_off` within `blob`; the caller is responsible for the
 * layout (this mirrors verify_narrate.py's ud_build(), kept independent on
 * purpose so a bug in one cannot hide behind the other). */
static size_t build(uint8_t *out, size_t cap, uint32_t field0,
                    const uint32_t bucket_off[28],
                    const uint8_t *blob, size_t blob_len)
{
    size_t n = 0, i;
    CHECK(cap >= 4 + 20 + 4 + 4 + 28 * 4 + blob_len);
    memcpy(out + n, "SVXF", 4); n += 4;
    memset(out + n, 0, 20); n += 20;
    memcpy(out + n, &field0, 4); n += 4;
    {
        uint32_t bl = (uint32_t)blob_len;
        memcpy(out + n, &bl, 4);
        n += 4;
    }
    for (i = 0; i < 28; i++) {
        memcpy(out + n, &bucket_off[i], 4);
        n += 4;
    }
    memcpy(out + n, blob, blob_len);
    n += blob_len;
    return n;
}

/* One entry: [len(word)+len(phon)][len(word)][h2][word][phon]. */
static size_t put_entry(uint8_t *out, const char *word, const char *phon, uint8_t h2)
{
    size_t wl = strlen(word), pl = strlen(phon), n = 0;
    CHECK(wl + pl <= 0xff && wl <= 0xff);
    out[n++] = (uint8_t)(wl + pl);
    out[n++] = (uint8_t)wl;
    out[n++] = h2;
    memcpy(out + n, word, wl); n += wl;
    memcpy(out + n, phon, pl); n += pl;
    return n;
}

static void test_parse_rejects_bad_magic(void)
{
    uint8_t file[144] = {0};
    sv_userdict_t *d = (sv_userdict_t *)0x1; /* poisoned, must come back NULL */
    memcpy(file, "XXXX", 4);
    CHECK(sv_userdict_parse(file, sizeof file, &d) == SV_USERDICT_E_MAGIC);
    CHECK(d == NULL);
}

static void test_parse_rejects_truncated_header(void)
{
    uint8_t file[10] = "SVXF";
    sv_userdict_t *d = (sv_userdict_t *)0x1;
    CHECK(sv_userdict_parse(file, sizeof file, &d) == SV_USERDICT_E_SHORT);
    CHECK(d == NULL);
}

static void test_parse_rejects_truncated_table(void)
{
    uint32_t bucket[28] = {0};
    uint8_t file[144 + 5];
    size_t n = build(file, sizeof file, 1, bucket, (const uint8_t *)"\0\0\0\0\0", 5);
    /* `file` declares a 5-byte table_len but the caller hands over only 2 of
     * those bytes -- as if the read that produced this buffer stopped
     * short. */
    sv_userdict_t *d = (sv_userdict_t *)0x1;
    CHECK(n == 144 + 5);
    CHECK(sv_userdict_parse(file, 144 + 2, &d) == SV_USERDICT_E_SHORT);
    CHECK(d == NULL);
}

static void test_parse_accepts_empty_dictionary(void)
{
    uint32_t bucket[28] = {0};
    uint8_t blob[3] = {0, 0, 0};
    uint8_t file[144 + 3];
    sv_userdict_t *d = NULL;
    size_t n = build(file, sizeof file, 1, bucket, blob, sizeof blob);
    CHECK(sv_userdict_parse(file, n, &d) == SV_USERDICT_OK);
    CHECK(d != NULL);
    sv_userdict_free(d);
}

static void test_parse_bad_args(void)
{
    uint8_t file[4] = "SVXF";
    CHECK(sv_userdict_parse(NULL, 4, NULL) == SV_USERDICT_E_BADARG);
    CHECK(sv_userdict_parse(file, 4, NULL) == SV_USERDICT_E_BADARG);
    {
        sv_userdict_t *d = (sv_userdict_t *)0x1;
        CHECK(sv_userdict_parse(NULL, 4, &d) == SV_USERDICT_E_BADARG);
        CHECK(d == NULL);
    }
}

static void test_free_null_is_a_noop(void)
{
    sv_userdict_free(NULL); /* must not crash */
}

/* Lookup: a whole-word match, the phoneme text appended and the cursor
 * advanced, status set the way exceptions() is (skips default_stress). */
static void test_lookup_matches_whole_word(void)
{
    uint32_t bucket[28] = {0};
    uint8_t blob[64];
    size_t bl = 0;
    bl += put_entry(blob + bl, "HELLO", " /HAH5LOW", 0x02);
    blob[bl++] = 0; blob[bl++] = 0; blob[bl++] = 0; /* terminator */
    bucket['H' - 'A'] = 0;

    {
        uint8_t file[144 + sizeof blob];
        size_t n = build(file, sizeof file, 1, bucket, blob, bl);
        sv_userdict_t *d = NULL;
        char normalised[] = " HELLO";   /* SLACK-free stand-in for ctx->in */
        char out[64] = {0};
        const char *in = normalised + 1;
        char *outp = out;
        int32_t out_left = (int32_t)sizeof out - 1;
        uint32_t status = 0;
        int rc;

        CHECK(sv_userdict_parse(file, n, &d) == SV_USERDICT_OK);
        init_cls();
        rc = sv_userdict_lookup(d, g_cls, normalised + 1, &in, &outp, &out_left, &status);
        CHECK(rc == 0);
        CHECK(strcmp(out, " /HAH5LOW") == 0);
        CHECK(in == normalised + 1 + 5);   /* past "HELLO" */
        CHECK(status == 2);
        sv_userdict_free(d);
    }
}

/* A word that isn't in the dictionary: nothing changes, rc == 0. */
static void test_lookup_miss_leaves_cursor(void)
{
    uint32_t bucket[28] = {0};
    uint8_t blob[3] = {0, 0, 0};   /* an immediate terminator */
    bucket['H' - 'A'] = 0;

    uint8_t file[144 + 3];
    size_t n = build(file, sizeof file, 1, bucket, blob, sizeof blob);
    sv_userdict_t *d = NULL;
    char normalised[] = " HAT";
    char out[16] = {0};
    const char *in = normalised + 1;
    char *outp = out;
    int32_t out_left = (int32_t)sizeof out - 1;
    uint32_t status = 0xdead;

    CHECK(sv_userdict_parse(file, n, &d) == SV_USERDICT_OK);
    init_cls();
    CHECK(sv_userdict_lookup(d, g_cls, normalised + 1, &in, &outp, &out_left, &status) == 0);
    CHECK(in == normalised + 1);   /* unchanged */
    CHECK(out[0] == '\0');
    CHECK(status == 0xdead);       /* untouched */
    sv_userdict_free(d);
}

/* A NULL dictionary is a pure no-op, matching sv_text_to_phon_ex(dict=NULL)
 * being byte-identical to sv_text_to_phon(). */
static void test_lookup_null_dict_is_noop(void)
{
    char normalised[] = " HELLO";
    char out[16] = {0};
    const char *in = normalised + 1;
    char *outp = out;
    int32_t out_left = (int32_t)sizeof out - 1;
    uint32_t status = 12345;

    init_cls();
    CHECK(sv_userdict_lookup(NULL, g_cls, normalised + 1, &in, &outp, &out_left, &status) == 0);
    CHECK(in == normalised + 1);
    CHECK(status == 12345);
}

/* field0 == 0: a well-formed table that is nonetheless disabled -- every
 * lookup must miss, exactly like no dictionary at all. */
static void test_lookup_disabled_dictionary(void)
{
    uint32_t bucket[28] = {0};
    uint8_t blob[64];
    size_t bl = put_entry(blob, "HELLO", " X", 0x02);
    blob[bl++] = 0; blob[bl++] = 0; blob[bl++] = 0;
    bucket['H' - 'A'] = 0;

    uint8_t file[144 + sizeof blob];
    size_t n = build(file, sizeof file, 0 /* disabled */, bucket, blob, bl);
    sv_userdict_t *d = NULL;
    char normalised[] = " HELLO";
    char out[16] = {0};
    const char *in = normalised + 1;
    char *outp = out;
    int32_t out_left = (int32_t)sizeof out - 1;
    uint32_t status = 7;

    CHECK(sv_userdict_parse(file, n, &d) == SV_USERDICT_OK);
    init_cls();
    CHECK(sv_userdict_lookup(d, g_cls, normalised + 1, &in, &outp, &out_left, &status) == 0);
    CHECK(in == normalised + 1);
    CHECK(status == 7);
    sv_userdict_free(d);
}

/* Buffer-full: the phoneme text doesn't fit, so nothing is written and the
 * caller is told to stop, matching exceptions()/numbers()/sv_rules_step(). */
static void test_lookup_buffer_full(void)
{
    uint32_t bucket[28] = {0};
    uint8_t blob[64];
    size_t bl = put_entry(blob, "HELLO", " 1234567890", 0x02); /* 11 bytes */
    blob[bl++] = 0; blob[bl++] = 0; blob[bl++] = 0;
    bucket['H' - 'A'] = 0;

    uint8_t file[144 + sizeof blob];
    size_t n = build(file, sizeof file, 1, bucket, blob, bl);
    sv_userdict_t *d = NULL;
    char normalised[] = " HELLO";
    char out[16] = {0};
    const char *in = normalised + 1;
    char *outp = out;
    int32_t out_left = 3;   /* far less than the 11-byte phoneme text */
    uint32_t status = 99;

    CHECK(sv_userdict_parse(file, n, &d) == SV_USERDICT_OK);
    init_cls();
    CHECK(sv_userdict_lookup(d, g_cls, normalised + 1, &in, &outp, &out_left, &status) == 1);
    CHECK(out[0] == '\0');          /* nothing committed */
    CHECK(in == normalised + 1);    /* cursor untouched */
    CHECK(out_left == 3);
    CHECK(status == 99);
    sv_userdict_free(d);
}

int main(void)
{
    test_parse_rejects_bad_magic();
    test_parse_rejects_truncated_header();
    test_parse_rejects_truncated_table();
    test_parse_accepts_empty_dictionary();
    test_parse_bad_args();
    test_free_null_is_a_noop();

    test_lookup_matches_whole_word();
    test_lookup_miss_leaves_cursor();
    test_lookup_null_dict_is_noop();
    test_lookup_disabled_dictionary();
    test_lookup_buffer_full();

    if (failures != 0) {
        printf("test_userdict: %d check(s) failed\n", failures);
        return 1;
    }
    printf("test_userdict: all checks passed\n");
    return 0;
}
