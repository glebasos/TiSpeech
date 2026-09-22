/*
 * test_generator.c — deterministic tests for the two reconstructed generator
 * stages (TIENG32!FUN_1c203010 and TIENG32!FUN_1c203870).
 *
 * These do NOT prove agreement with the original engine. That is what
 * tools/verify_generator.py does, against the real TIENG32.DLL under Unicorn,
 * comparing the output and every byte of the module's .data. What these tests
 * cover is the part a differential cannot: that the argument checking we added
 * on top of the original actually refuses, that the two entry points agree
 * with each other, and that the structural invariants which make the stage
 * meaningful hold on values chosen by hand.
 *
 * The arithmetic tests carry a synthetic rate table, so they run with no DLL
 * present. The tests that need the real phoneme inventory are gated on the
 * extracted data, exactly as test_ruleset.c gates its Spanish cases.
 */

#include "tispeech/generator.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int condition, const char *what)
{
    if (condition) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        failures++;
    }
}

/* The real table's shape: 9 rows of 16, row 8 all-256 ("snap immediately"),
 * column 0 of the later rows 256. Values here are the real ones for the rows
 * the tests below use; they are a 9x16 arithmetic fixture, not the table
 * itself, so nothing proprietary is needed to run this file. */
static int rate_values[SV_GEN_RATE_ROWS * SV_GEN_RATE_COLS];
static sv_gen_rates rates = { rate_values, sizeof rate_values / sizeof rate_values[0] };

static void build_rates(void)
{
    int row, col;
    for (row = 0; row < SV_GEN_RATE_ROWS; ++row) {
        for (col = 0; col < SV_GEN_RATE_COLS; ++col) {
            /* Row 8 snaps; earlier rows decay. The exact curve does not
             * matter to these tests, only that it is monotone and that row 8
             * is 256. */
            rate_values[row * SV_GEN_RATE_COLS + col] =
                (row == 8) ? 256 : (256 >> (col / 2)) >> (8 - row);
        }
    }
}

static int16_t contour[SV_GEN_MAX_FRAMES];
static int16_t ramp_buf[SV_GEN_MAX_FRAMES];
static int16_t forward_buf[SV_GEN_MAX_FRAMES];
static sv_gen_scratch scratch = { ramp_buf, forward_buf, SV_GEN_MAX_FRAMES };

static sv_gen_track base_track(void)
{
    /* Exactly what the generator's own init writes into every track at
     * 0x1c204bf1: hold 0x19, fall 0x4b, row 3, both columns 7. */
    sv_gen_track t;
    memset(&t, 0, sizeof t);
    t.fwd_target = 1000;
    t.bwd_target = 2000;
    t.fwd_start = 0;
    t.bwd_start = 0;
    t.fwd_rate_col = 7;
    t.bwd_rate_col = 7;
    t.rate_row = 3;
    t.hold_percent = 25;
    t.fall_percent = 75;
    return t;
}

static void test_ramp_shape(void)
{
    sv_gen_track t = base_track();
    int n = 40;
    int hold = (25 * 40) / 100 + 1;     /* 11 */
    int i;
    int monotone = 1;

    check(sv_gen_crossfade_ramp(&t, n, ramp_buf, SV_GEN_MAX_FRAMES) == SV_GEN_OK,
          "ramp: accepted");

    for (i = 0; i < hold; ++i) {
        if (ramp_buf[i] != 256) {
            break;
        }
    }
    check(i == hold, "ramp: holds at 256 for (hold_percent*n)/100 + 1 frames");

    /* 0x1c20391a forces the LAST frame to zero, whatever the fall says. The
     * store's base is the ramp base MINUS two, so it is ramp[n-1]. */
    check(ramp_buf[n - 1] == 0, "ramp: last frame forced to 0 (0x1c20391a)");

    for (i = hold; i < n - 1; ++i) {
        if (ramp_buf[i] > ramp_buf[i - 1]) {
            monotone = 0;
        }
        if (ramp_buf[i] < 0 || ramp_buf[i] > 256) {
            monotone = 0;
        }
    }
    check(monotone, "ramp: falls monotonically and stays within 0..256");
}

static void test_degenerate_span(void)
{
    /* fall <= hold makes the span non-positive; 0x1c2038ad clamps it to 1, so
     * the ramp drops in a single step rather than dividing by zero. */
    sv_gen_track t = base_track();
    t.hold_percent = 80;
    t.fall_percent = 10;
    check(sv_gen_crossfade_ramp(&t, 20, ramp_buf, SV_GEN_MAX_FRAMES) == SV_GEN_OK,
          "ramp: fall below hold is clamped, not a division by zero");
    check(ramp_buf[19] == 0, "ramp: last frame still 0 with a clamped span");
}

static void test_single_frame(void)
{
    sv_gen_track t = base_track();
    check(sv_gen_track_contour(&t, 1, &rates, contour, &scratch) == SV_GEN_OK,
          "track: a one-frame phoneme is legal");
    /* With n == 1 the only frame is the forced-zero one, so the forward pass
     * contributes nothing and the contour is the backward pass alone. */
    check(ramp_buf[0] == 0, "track: the single frame belongs to the backward pass");
}

static void test_snap_row(void)
{
    /* Row 8 is all 256, i.e. rate == 256, so (target - v) * 256 >> 8 is the
     * whole distance: the value reaches the target on the first frame. The
     * row is clamped to 0..8 at 0x1c203927, so 99 selects row 8 as well. */
    sv_gen_track t = base_track();
    t.rate_row = 99;
    t.fwd_start = 0;
    t.fwd_target = 700;
    t.bwd_start = 0;
    t.bwd_target = 700;
    t.hold_percent = 0;
    t.fall_percent = 100;
    check(sv_gen_track_contour(&t, 8, &rates, contour, &scratch) == SV_GEN_OK,
          "track: rate_row above 8 is clamped, not a table overrun");
    check(forward_buf[0] == (int16_t)((ramp_buf[0] * 700) >> 8),
          "track: row 8 snaps the forward value to its target on frame 0");
}

static void test_crossfade_endpoints(void)
{
    /* The point of the stage: the contour starts near the forward anchor and
     * ends at the backward anchor, because ramp[] runs 256 -> 0 and the last
     * frame is forced to 0. With both rates at row 8 the two passes are at
     * their targets immediately, which makes the endpoints exact. */
    sv_gen_track t = base_track();
    t.rate_row = 8;
    t.fwd_start = 500;
    t.fwd_target = 500;
    t.bwd_start = 900;
    t.bwd_target = 900;
    t.hold_percent = 0;
    t.fall_percent = 100;

    check(sv_gen_track_contour(&t, 16, &rates, contour, &scratch) == SV_GEN_OK,
          "track: crossfade case accepted");
    check(contour[15] == 900,
          "track: the last frame is the backward anchor alone");
    /* hold is (0*16)/100 + 1 == 1, so frame 0 is still fully held at 256 and
     * the backward pass contributes (256-256)*w >> 8 == 0. The first frame is
     * therefore the forward anchor EXACTLY, not a mix -- the crossfade only
     * begins at frame 1. */
    check(contour[0] == 500,
          "track: the first frame is the forward anchor alone");
    check(contour[8] > 500 && contour[8] < 900,
          "track: a middle frame is a mix of the two anchors");
}

static void test_agreement(void)
{
    /* sv_gen_crossfade_ramp is the same code the fused call runs; if these
     * ever disagree the split is a lie. */
    sv_gen_track t = base_track();
    int16_t standalone[64];
    int n = 37;

    check(sv_gen_track_contour(&t, n, &rates, contour, &scratch) == SV_GEN_OK,
          "agreement: fused call accepted");
    check(sv_gen_crossfade_ramp(&t, n, standalone, 64) == SV_GEN_OK,
          "agreement: standalone ramp accepted");
    check(memcmp(standalone, ramp_buf, (size_t)n * sizeof standalone[0]) == 0,
          "agreement: both entry points produce the same ramp");
}

static void test_refusals(void)
{
    sv_gen_track t = base_track();
    sv_gen_scratch small = { ramp_buf, forward_buf, 4 };

    check(sv_gen_track_contour(0, 8, &rates, contour, &scratch) == SV_GEN_E_ARG,
          "refuse: null track");
    check(sv_gen_track_contour(&t, 8, &rates, 0, &scratch) == SV_GEN_E_ARG,
          "refuse: null contour");
    check(sv_gen_track_contour(&t, 0, &rates, contour, &scratch) == SV_GEN_E_FRAMES,
          "refuse: zero frames");
    check(sv_gen_track_contour(&t, SV_GEN_MAX_FRAMES + 1, &rates, contour,
                               &scratch) == SV_GEN_E_FRAMES,
          "refuse: more frames than the original's buffer holds");
    check(sv_gen_track_contour(&t, 32, &rates, contour, &small) == SV_GEN_E_SCRATCH,
          "refuse: scratch smaller than the frame count");

    /* The original does NOT bounds-check the column (0x1c20394e); it would
     * read past the table. We refuse instead. */
    t = base_track();
    t.fwd_rate_col = 900;
    check(sv_gen_track_contour(&t, 8, &rates, contour, &scratch) == SV_GEN_E_RANGE,
          "refuse: rate column past the end of the table");
    /* The index is the column PLUS row*16, so a negative column only leaves
     * the table when the row does not carry it back in. Row 0 does not. */
    t = base_track();
    t.rate_row = 0;
    t.bwd_rate_col = -1;
    check(sv_gen_track_contour(&t, 8, &rates, contour, &scratch) == SV_GEN_E_RANGE,
          "refuse: negative rate index");
    t = base_track();
    t.rate_row = 3;
    t.bwd_rate_col = -1;
    check(sv_gen_track_contour(&t, 8, &rates, contour, &scratch) == SV_GEN_OK,
          "allow: a negative column that row*16 carries back into the table");

    /* A negative hold makes the original write more ramp entries than it
     * indexes and run off its buffer. Refused rather than reproduced. */
    t = base_track();
    t.hold_percent = -500;
    check(sv_gen_track_contour(&t, 8, &rates, contour, &scratch) == SV_GEN_E_ARG,
          "refuse: hold_percent that would overrun the original's ramp");
}

static void test_class_terminator(void)
{
    /* 0x1c203014 answers the 0x00FF terminator before touching the table, so
     * a caller can classify the end of a phoneme array with no table at all. */
    check(sv_gen_phoneme_class(0, 0x00FF) == SV_GEN_CLASS_SILENCE,
          "class: 0x00FF is silence without a table");
    check(sv_gen_phoneme_class(0, 3) == SV_GEN_E_ARG,
          "class: a real code needs a table");
}

static void test_class_flags(void)
{
    /* Synthetic entries: 26-byte stride, flags at +2, little endian. These
     * pin the ORDER of the decision tree, which the real inventory cannot,
     * because no English phoneme carries two of these bits at once. */
    static unsigned char entries[SV_GEN_PHONEME_STRIDE * 6];
    sv_gen_phonemes table;
    size_t i;
    static const struct { unsigned int flags; int want; const char *what; } cases[] = {
        { SV_GEN_PH_VOWEL | SV_GEN_PH_NASAL, SV_GEN_CLASS_VOWEL,
          "class: vowel outranks nasal" },
        { SV_GEN_PH_GLIDE | SV_GEN_PH_STOP, SV_GEN_CLASS_APPROXIMANT,
          "class: glide outranks stop" },
        { SV_GEN_PH_FRICATIVE | SV_GEN_PH_VOICED, SV_GEN_CLASS_FRICATIVE_VOICED,
          "class: voiced fricative" },
        { SV_GEN_PH_FRICATIVE, SV_GEN_CLASS_FRICATIVE_VOICELESS,
          "class: voiceless fricative" },
        { SV_GEN_PH_STOP | SV_GEN_PH_VOICED, SV_GEN_CLASS_STOP_VOICED,
          "class: voiced stop" },
        { SV_GEN_PH_GLOTTAL, SV_GEN_CLASS_GLOTTAL_STOP,
          "class: glottal stop when nothing else fires" },
    };

    memset(entries, 0, sizeof entries);
    table.entries = entries;
    table.count = 6;
    for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        unsigned char *p = entries + i * SV_GEN_PHONEME_STRIDE
                         + SV_GEN_PHONEME_FLAGS;
        p[0] = (unsigned char)(cases[i].flags & 0xFF);
        p[1] = (unsigned char)((cases[i].flags >> 8) & 0xFF);
        p[2] = (unsigned char)((cases[i].flags >> 16) & 0xFF);
        p[3] = (unsigned char)((cases[i].flags >> 24) & 0xFF);
    }
    for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        check(sv_gen_phoneme_class(&table, (int)i) == cases[i].want,
              cases[i].what);
    }
    check(sv_gen_phoneme_class(&table, 6) == SV_GEN_E_RANGE,
          "class: a code past the table is refused, not read");
}

#ifdef TISPEECH_HAVE_ENG_GEN_DATA
extern const sv_gen_phonemes sv_gen_data_eng_phonemes[2];
extern const sv_gen_rates sv_gen_data_eng_rates;

/* The name is a 16-bit word, so it reads back high byte first: "IY" is stored
 * 'Y','I', and a one-character name like "R" is stored 0x00,'R'. Passing a
 * plain C string works for both, because "R" already is 'R','\0'. */
static int class_of(const char *name)
{
    const sv_gen_phonemes *t = &sv_gen_data_eng_phonemes[0];
    size_t i;
    char want[2];

    want[0] = name[0];
    want[1] = name[0] ? name[1] : '\0';
    for (i = 0; i < t->count; ++i) {
        const unsigned char *e = t->entries + i * SV_GEN_PHONEME_STRIDE;
        if ((char)e[1] == want[0] && (char)e[0] == want[1]) {
            return sv_gen_phoneme_class(t, (int)i);
        }
    }
    return -999;
}

static void test_real_inventory(void)
{
    /* These are the observed partition of the real English table, which is
     * what justifies the class names in generator.h. */
    check(class_of("IY") == SV_GEN_CLASS_VOWEL, "eng: IY is a vowel");
    check(class_of("AA") == SV_GEN_CLASS_VOWEL, "eng: AA is a vowel");
    check(class_of("OW") == SV_GEN_CLASS_VOWEL, "eng: OW is a vowel");
    check(class_of("R") == SV_GEN_CLASS_APPROXIMANT, "eng: R is an approximant");
    check(class_of("L") == SV_GEN_CLASS_APPROXIMANT, "eng: L is an approximant");
    check(class_of("M") == SV_GEN_CLASS_NASAL, "eng: M is a nasal");
    check(class_of("N") == SV_GEN_CLASS_NASAL, "eng: N is a nasal");
    check(class_of("B") == SV_GEN_CLASS_STOP_VOICED, "eng: B is a voiced stop");
    check(class_of("P") == SV_GEN_CLASS_STOP_VOICELESS,
          "eng: P is a voiceless stop");
    check(class_of(".") == SV_GEN_CLASS_SILENCE, "eng: '.' is silence");
    check(class_of("SH") == SV_GEN_CLASS_FRICATIVE_VOICELESS,
          "eng: SH is a voiceless fricative");
    check(class_of("ZH") == SV_GEN_CLASS_FRICATIVE_VOICED,
          "eng: ZH is a voiced fricative");
    check(class_of("CH") == SV_GEN_CLASS_STOP_VOICELESS,
          "eng: CH is a voiceless stop/affricate");
    check(class_of("DH") == SV_GEN_CLASS_FRICATIVE_VOICED,
          "eng: DH is a voiced fricative");

    check(sv_gen_data_eng_rates.count
              == SV_GEN_RATE_ROWS * SV_GEN_RATE_COLS,
          "eng: the rate table is 9 rows of 16");
    check(sv_gen_data_eng_rates.table[8 * SV_GEN_RATE_COLS] == 256
              && sv_gen_data_eng_rates.table[9 * SV_GEN_RATE_COLS - 1] == 256,
          "eng: row 8 snaps");

    /* The extracted table must drive the real code path, not just sit there. */
    {
        sv_gen_track t = base_track();
        check(sv_gen_track_contour(&t, 24, &sv_gen_data_eng_rates, contour,
                                   &scratch) == SV_GEN_OK,
              "eng: a track runs against the extracted rate table");
    }
}
#endif

int main(void)
{
    build_rates();

    test_ramp_shape();
    test_degenerate_span();
    test_single_frame();
    test_snap_row();
    test_crossfade_endpoints();
    test_agreement();
    test_refusals();
    test_class_terminator();
    test_class_flags();
#ifdef TISPEECH_HAVE_ENG_GEN_DATA
    test_real_inventory();
#endif

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall passed\n");
    printf("NOTE: agreement with the original is proved by "
           "tools/verify_generator.py, not by this file.\n");
    return 0;
}
