/* Deterministic tests for the pitch-smoothing pass, src/smoothing.c.
 *
 * These are NOT the differential. tools/verify_smoothing.py is what proves
 * the reconstruction agrees with TIBASE32 0x1c00cd40..0x1c00cdb3 and
 * 0x1c00de60..0x1c00df00, by running
 * the original x86 code under an emulator; it needs unicorn and a copy of
 * the DLL, so it cannot run in an ordinary build. What lives here is the
 * part that can: the arithmetic the differential established, pinned so a
 * later edit cannot quietly undo it. No proprietary data of any kind is
 * needed for this pass, so unlike test_frames.c/test_ruleset.c this file
 * needs no build-time extraction and runs unconditionally.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tispeech/smoothing.h"

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static void clear(sv_frame *frames, unsigned count)
{
    memset(frames, 0, count * sizeof(*frames));
}

static void terminate(sv_frame *frames, unsigned index)
{
    /* Matches TIBASE32 0x1c0058c5..0x1c0058d5: formant_freq[0..2] = 0xff,
     * amp_voicing = 0xff, pitch = 0xffff. Only formant_freq[0] and pitch
     * matter to this pass, but all five are set for fidelity. */
    frames[index].formant_freq[0] = SV_FRAME_END;
    frames[index].formant_freq[1] = 0xff;
    frames[index].formant_freq[2] = 0xff;
    frames[index].amp_voicing = 0xff;
    frames[index].pitch = 0xffff;
}

/* A single gap of zero-pitch frames between two known values splits evenly,
 * with truncation toward zero on ties. */
static void test_basic_gap(void)
{
    sv_frame frames[6];

    clear(frames, 6);
    frames[0].pitch = 100;
    /* frames[1..3].pitch == 0 */
    frames[4].pitch = 200;
    terminate(frames, 5);

    sv_smooth_pitch(frames);

    /* start=100<<8=25600, end=200<<8=51200, delta=25600, run=3, step =
     * 25600/4 = 6400. Values: 25600+6400=32000 -> 32000>>8=125;
     * +6400=38400 -> 150; +6400=44800 -> 175. */
    CHECK(frames[0].pitch == 100);
    CHECK(frames[1].pitch == 125);
    CHECK(frames[2].pitch == 150);
    CHECK(frames[3].pitch == 175);
    CHECK(frames[4].pitch == 200);
}

/* A falling contour: step is negative, and per the header's proof every
 * partial sum stays strictly between the two endpoints, never touching 0
 * even though the arithmetic shift is signed. */
static void test_falling_gap(void)
{
    sv_frame frames[5];

    clear(frames, 5);
    frames[0].pitch = 300;
    frames[3].pitch = 1;
    terminate(frames, 4);

    sv_smooth_pitch(frames);

    /* start=300<<8=76800, end=1<<8=256, delta=-76544, run=2, step =
     * -76544/3 = -25514 (truncated toward zero, not -25514.67 floored).
     * v1 = 76800-25514 = 51286 -> >>8 = 200 (integer part).
     * v2 = 51286-25514 = 25772 -> >>8 = 100. */
    CHECK(frames[0].pitch == 300);
    CHECK(frames[1].pitch == 200);
    CHECK(frames[2].pitch == 100);
    CHECK(frames[3].pitch == 1);
    /* Never negative, never zero, despite the falling contour. */
    CHECK(frames[1].pitch > 0);
    CHECK(frames[2].pitch > 0);
}

/* No gap: adjacent nonzero frames are left untouched and the anchor simply
 * advances (0x1c00cd78..0x1c00cda8 with run == 0). */
static void test_no_gap(void)
{
    sv_frame frames[4];

    clear(frames, 4);
    frames[0].pitch = 50;
    frames[1].pitch = 60;
    frames[2].pitch = 70;
    terminate(frames, 3);

    sv_smooth_pitch(frames);

    CHECK(frames[0].pitch == 50);
    CHECK(frames[1].pitch == 60);
    CHECK(frames[2].pitch == 70);
}

/* A trailing run of zero-pitch frames that reaches the terminator without
 * meeting another nonzero anchor is left at 0 -- the terminator check comes
 * before the run is used (0x1c00cd73..0x1c00cd76), and there is no second
 * endpoint to interpolate toward. */
static void test_trailing_zero_run_untouched(void)
{
    sv_frame frames[5];

    clear(frames, 5);
    frames[0].pitch = 80;
    /* frames[1..3].pitch left at 0 */
    terminate(frames, 4);

    sv_smooth_pitch(frames);

    CHECK(frames[0].pitch == 80);
    CHECK(frames[1].pitch == 0);
    CHECK(frames[2].pitch == 0);
    CHECK(frames[3].pitch == 0);
}

/* A leading anchor of pitch 0 (frames[0] itself) is used as-is: the original
 * never special-cases the very first frame, it is just the first anchor. */
static void test_zero_leading_anchor(void)
{
    sv_frame frames[4];

    clear(frames, 4);
    /* frames[0].pitch == 0 */
    frames[1].pitch = 0; /* part of the run */
    frames[2].pitch = 40;
    terminate(frames, 3);

    sv_smooth_pitch(frames);

    /* start=0, end=40<<8=10240, run=1, step=10240/2=5120.
     * v1 = 0+5120 = 5120 -> >>8 = 20. */
    CHECK(frames[0].pitch == 0);
    CHECK(frames[1].pitch == 20);
    CHECK(frames[2].pitch == 40);
}

/* Multiple independent runs in one array: the anchor advances past each
 * resolved gap and the next run is interpolated against the next pair of
 * real endpoints, matching the 0x1c00cda8/0x1c00cdaa outer loop. */
static void test_multiple_runs(void)
{
    sv_frame frames[9];

    clear(frames, 9);
    frames[0].pitch = 10;
    /* gap: frames[1] */
    frames[2].pitch = 30;
    frames[3].pitch = 30;
    /* gap: frames[4..5] */
    frames[6].pitch = 90;
    terminate(frames, 7);

    sv_smooth_pitch(frames);

    /* First gap: start=10<<8=2560, end=30<<8=7680, run=1,
     * step=5120/2=2560 -> v1=5120 -> >>8=20. */
    CHECK(frames[1].pitch == 20);
    /* frames[2],[3] both nonzero already -> no gap between them (run==0). */
    CHECK(frames[2].pitch == 30);
    CHECK(frames[3].pitch == 30);
    /* Second gap: start=30<<8=7680, end=90<<8=23040, run=2,
     * step=15360/3=5120 -> v1=12800 -> >>8=50; v2=17920 -> >>8=70. */
    CHECK(frames[4].pitch == 50);
    CHECK(frames[5].pitch == 70);
    CHECK(frames[6].pitch == 90);
}

/* An array that is nothing but the preamble anchor and a terminator: the
 * scan immediately finds the terminator, run stays 0, function returns. */
static void test_empty_after_anchor(void)
{
    sv_frame frames[2];

    clear(frames, 2);
    frames[0].pitch = 42;
    terminate(frames, 1);

    sv_smooth_pitch(frames);

    CHECK(frames[0].pitch == 42);
}


/* ------------------------------------------------------------------------ */
/* The slew pass, TIBASE32 0x1c00de60.                                       */
/*                                                                           */
/* Every expected track below was read out of the ORIGINAL DLL under the     */
/* emulator (tools/verify_smoothing.py's harness) rather than derived here,  */
/* so these are oracle-confirmed fixtures, not predictions.                  */
/* ------------------------------------------------------------------------ */

static void load(sv_frame *frames, const uint16_t *pitches, unsigned count)
{
    unsigned i;

    clear(frames, count + 1);
    for (i = 0; i < count; ++i) {
        frames[i].pitch = pitches[i];
    }
    terminate(frames, count);
}

static void expect(const sv_frame *frames, const uint16_t *want,
                   unsigned count, const char *what)
{
    unsigned i;

    for (i = 0; i < count; ++i) {
        if (frames[i].pitch != want[i]) {
            printf("FAIL %s: frame %u is %u, expected %u\n",
                   what, i, frames[i].pitch, want[i]);
            ++failures;
            return;
        }
    }
}

static void test_slew_lags_a_step(void)
{
    static const uint16_t in[] = { 100, 200, 200, 200 };
    static const uint16_t want[] = { 100, 166, 181, 187 };
    sv_frame frames[5];

    load(frames, in, 4);
    sv_smooth_pitch_slew(frames, 128, 256);
    expect(frames, want, 4, "slew: a rising step lags at rate 128");
    /* The terminator is past the forward pass's stop test. */
    CHECK(frames[4].pitch == 0xffff);
}

static void test_slew_snaps_at_256(void)
{
    static const uint16_t in[] = { 100, 200, 200, 200 };
    sv_frame frames[5];

    load(frames, in, 4);
    sv_smooth_pitch_slew(frames, 256, 256);
    expect(frames, in, 4, "slew: rate 256 leaves the track alone");
}

static void test_slew_freezes_at_zero(void)
{
    static const uint16_t in[] = { 100, 200, 200, 200 };
    static const uint16_t want[] = { 100, 100, 100, 100 };
    sv_frame frames[5];

    load(frames, in, 4);
    sv_smooth_pitch_slew(frames, 0, 0);
    expect(frames, want, 4, "slew: rate 0 holds frame 0's pitch");
}

static void test_slew_uses_the_falling_rate(void)
{
    static const uint16_t in[] = { 200, 100, 100, 100 };
    static const uint16_t want[] = { 200, 153, 145, 142 };
    sv_frame frames[5];

    /* Same shape as test_slew_lags_a_step, mirrored: this track only falls,
     * so it is the second rate that decides it. Swapping the two arguments
     * changes the result, which is what makes the pair meaningful. */
    load(frames, in, 4);
    sv_smooth_pitch_slew(frames, 128, 64);
    expect(frames, want, 4, "slew: a falling step uses the falling rate");
}

static void test_slew_alternating(void)
{
    static const uint16_t in[] = { 300, 100, 300, 100, 300 };
    static const uint16_t want[] = { 300, 172, 179, 155, 166 };
    sv_frame frames[6];

    load(frames, in, 5);
    sv_smooth_pitch_slew(frames, 64, 192);
    expect(frames, want, 5, "slew: alternating edges pick both rates");
}

static void test_slew_frame_zero_never_moves(void)
{
    static const uint16_t in[] = { 120 };
    sv_frame frames[2];

    /* One real frame: the forward pass writes it with a zero delta and the
     * backward pass does not reach it (0x1c00dec4 decrements the count). */
    load(frames, in, 1);
    sv_smooth_pitch_slew(frames, 128, 64);
    CHECK(frames[0].pitch == 120);
}

static void test_slew_terminator_only(void)
{
    sv_frame frames[1];

    clear(frames, 1);
    terminate(frames, 0);
    sv_smooth_pitch_slew(frames, 128, 64);
    CHECK(frames[0].pitch == 0xffff);
    CHECK(frames[0].formant_freq[0] == SV_FRAME_END);
}

int main(void)
{
    test_basic_gap();
    test_falling_gap();
    test_no_gap();
    test_trailing_zero_run_untouched();
    test_zero_leading_anchor();
    test_multiple_runs();
    test_empty_after_anchor();

    test_slew_lags_a_step();
    test_slew_snaps_at_256();
    test_slew_freezes_at_zero();
    test_slew_uses_the_falling_rate();
    test_slew_alternating();
    test_slew_frame_zero_never_moves();
    test_slew_terminator_only();

    if (failures != 0) {
        printf("test_smoothing: %d check(s) failed\n", failures);
        return 1;
    }
    printf("test_smoothing: all checks passed\n");
    return 0;
}
