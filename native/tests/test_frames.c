/* Deterministic tests for the parameter-frame stage, src/frames.c.
 *
 * These are NOT the differential. tools/verify_frames.py is what proves the
 * reconstruction agrees with TIBASE32, by running the original x86 code under
 * an emulator; it needs unicorn and a copy of the DLL, so it cannot run in an
 * ordinary build. What lives here is the part that can: the behaviour the
 * differential established, pinned so a later edit cannot quietly undo it.
 *
 * Deliberately no golden PCM and no table values. Expectations are either
 * pure arithmetic of our own, or relations computed from the extracted table
 * at run time, so nothing owned by SoftVoice is written into this repository.
 * The build-time extract supplies the data; see tools/extract_base.py.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "tispeech/frames.h"

extern const sv_frame_tables sv_base_tables;
extern const sv_phoneme_names sv_base_tables_phonemes;

#define RATE 11025

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

/* A frame array the renderer will accept: a lead-in the reset skips over, a
 * body, and a terminator. Index 0..4 are never rendered -- sv_frame_reset()
 * leaves frame_index at 4 and sv_frame_apply() steps before it applies. */
static void build_frames(sv_frame *frames, unsigned count, unsigned terminator)
{
    unsigned i;

    memset(frames, 0, count * sizeof(*frames));
    for (i = 0; i < count; ++i) {
        frames[i].duration = 8;
        frames[i].pitch = 0x40;
        frames[i].amp_voicing = 0x20;
        frames[i].source_index_a = 0;
        frames[i].source_index_b = 1;
    }
    frames[terminator].formant_freq[0] = SV_FRAME_END;
}

static void init_state(sv_frame_state *s)
{
    memset(s, 0, sizeof(*s));
    s->sample_rate = RATE;
    s->flags = SV_FRAME_FLAG_NO_EVENTS;
    s->restart = 1;
    s->speaking = 1;
}

/* ---------------------------------------------------------------------- */

static void test_frame_length(void)
{
    /* Our own arithmetic, from 0x1c0044f8: rate * 150 / 60, then / (2 * d),
     * then >> 1, each step truncating in 16 bits. 11025 * 150 / 60 = 27562. */
    CHECK(sv_frame_length(RATE, 1) == 6890);   /* 27562 / 2   = 13781, >>1 */
    CHECK(sv_frame_length(RATE, 8) == 861);    /* 27562 / 16  = 1722,  >>1 */
    CHECK(sv_frame_length(RATE, 32) == 215);   /* 27562 / 64  = 430,   >>1 */
    CHECK(sv_frame_length(RATE, 255) == 27);   /* 27562 / 510 = 54,    >>1 */

    /* Longer frames for longer durations, with no reversals anywhere in the
     * range -- the truncation is what makes that worth asserting. */
    {
        unsigned d;
        for (d = 2; d <= 255; ++d) {
            CHECK(sv_frame_length(RATE, (uint8_t)d)
                  <= sv_frame_length(RATE, (uint8_t)(d - 1)));
        }
    }

    /* Duration zero is the original's divide by zero; we return 0 and
     * sv_frame_apply() refuses before ever calling this. */
    CHECK(sv_frame_length(RATE, 0) == 0);
}

static void test_reset(void)
{
    sv_frame_state s;

    memset(&s, 0xAB, sizeof(s));
    s.scratch_2c0 = 0x12345678u;
    sv_frame_reset(&s);

    CHECK(s.restart == 0);
    CHECK(s.frame_index == 4);      /* frames + 0x80 */
    CHECK(s.elapsed == 0x40);
    CHECK(s.noise_left == 0x3ff);
    CHECK(s.noise_index == 0);
    CHECK(s.dsp.phase == 0);
    CHECK(s.dsp.phase_step == 0x20000u);
    CHECK(s.dsp.previous_voice == 0);
    CHECK(s.voice_frac == 0);       /* the whole dword at 0x2aa, not half */
    CHECK(s.scratch_2e8 == 0x12345678u);

    /* The reset clears each filter's two history words and nothing else: it is
     * one dword store per slot, so the a/b ramps keep whatever they held. */
    {
        int i;
        for (i = 0; i < SV_DSP_FILTER_COUNT; ++i) {
            CHECK(s.dsp.filters[i].previous == 0);
            CHECK(s.dsp.filters[i].previous2 == 0);
            CHECK(s.dsp.filters[i].a.value == 0xABABABABu);
        }
    }
}

static void test_apply_refusals(void)
{
    sv_frame_state s;
    sv_frame frames[16];

    build_frames(frames, 16, 15);

    /* The event-reporting block at 0x1c004543 is not reconstructed, so a
     * state that would reach it is refused rather than silently stripped. */
    init_state(&s);
    s.flags = 0;
    s.frame_index = 5;
    CHECK(sv_frame_apply(&s, &sv_base_tables, frames) == SV_FRAMES_E_EVENTS);

    /* A terminator ends the stream. */
    init_state(&s);
    s.frame_index = 14;
    CHECK(sv_frame_apply(&s, &sv_base_tables, frames) == SV_FRAMES_END);

    /* So does a marker of 1 or 2 two frames back (0x1c0044de). */
    {
        uint8_t marker;
        for (marker = 1; marker <= 2; ++marker) {
            init_state(&s);
            s.frame_index = 7;
            frames[6].marker = marker;
            CHECK(sv_frame_apply(&s, &sv_base_tables, frames) == SV_FRAMES_END);
            frames[6].marker = 0;
        }
        /* Any other marker value does not. */
        init_state(&s);
        s.frame_index = 7;
        frames[6].marker = 3;
        CHECK(sv_frame_apply(&s, &sv_base_tables, frames) == SV_FRAMES_OK);
        frames[6].marker = 0;
    }

    /* A duration of zero is the original's #DE. */
    init_state(&s);
    s.frame_index = 7;
    frames[8].duration = 0;
    CHECK(sv_frame_apply(&s, &sv_base_tables, frames) == SV_FRAMES_E_LENGTH);
    frames[8].duration = 8;

    /* Row indices past the extracted blob, where the original reads its own
     * .text, and a glottal selector past the twelve real pointers, where it
     * reads the string that follows the table. */
    init_state(&s);
    s.frame_index = 7;
    frames[8].formant_freq[0] = SV_CASCADE_ROWS_ADDRESSABLE;
    CHECK(sv_frame_apply(&s, &sv_base_tables, frames) == SV_FRAMES_E_RANGE);
    frames[8].formant_freq[0] = 0;

    init_state(&s);
    s.frame_index = 7;
    frames[8].formant_freq[3] = SV_PARALLEL_ROWS_ADDRESSABLE;
    CHECK(sv_frame_apply(&s, &sv_base_tables, frames) == SV_FRAMES_E_RANGE);
    frames[8].formant_freq[3] = 0;

    init_state(&s);
    s.frame_index = 7;
    frames[8].source_index_a = SV_GLOTTAL_TABLES;
    CHECK(sv_frame_apply(&s, &sv_base_tables, frames) == SV_FRAMES_E_RANGE);
    frames[8].source_index_a = 0;

    /* The last real row on each table is still accepted. */
    init_state(&s);
    s.frame_index = 7;
    frames[8].formant_freq[0] = SV_CASCADE_ROWS - 1;
    frames[8].formant_freq[3] = SV_PARALLEL_ROWS - 1;
    frames[8].source_index_a = SV_GLOTTAL_TABLES - 1;
    CHECK(sv_frame_apply(&s, &sv_base_tables, frames) == SV_FRAMES_OK);
    frames[8].formant_freq[0] = 0;
    frames[8].formant_freq[3] = 0;
    frames[8].source_index_a = 0;
}

/* The finding the whole-state differential exists to protect: the amplitude
 * table is indexed by BYTE offset, not by element (0x1c004857 has no
 * `shl edi,1`). This asserts the reconstruction reads the byte-offset word,
 * and -- so the assertion cannot pass vacuously -- that the element-indexed
 * answer at the same index is a different number. */
static void test_amplitude_is_byte_indexed(void)
{
    sv_frame_state s;
    sv_frame frames[16];
    uint16_t byte_indexed;
    uint16_t element_indexed;
    const uint8_t *table = sv_base_tables.amplitude;

    build_frames(frames, 16, 15);
    frames[8].amp_aspiration = 1;

    byte_indexed = (uint16_t)(table[1] | ((uint16_t)table[2] << 8));
    element_indexed = (uint16_t)(table[2] | ((uint16_t)table[3] << 8));
    CHECK(byte_indexed != element_indexed);

    init_state(&s);
    s.frame_index = 7;
    CHECK(sv_frame_apply(&s, &sv_base_tables, frames) == SV_FRAMES_OK);
    CHECK(s.aspiration_level == byte_indexed);
    CHECK(s.aspiration_level != element_indexed);

    /* Both half-cycle levels get the same word, and both fractions are
     * cleared, because the original stores one dword three times. */
    CHECK((uint16_t)s.dsp.aspiration_negative == byte_indexed);
    CHECK((uint16_t)s.dsp.aspiration_positive == byte_indexed);
    CHECK(s.aspiration_frac == 0);
    CHECK(s.level_frac[0] == 0 && s.level_frac[1] == 0);
}

static void test_apply_effects(void)
{
    sv_frame_state s;
    sv_frame frames[16];

    build_frames(frames, 16, 15);

    /* Frame timing and the walk through the array. */
    init_state(&s);
    s.frame_index = 7;
    CHECK(sv_frame_apply(&s, &sv_base_tables, frames) == SV_FRAMES_OK);
    CHECK(s.frame_index == 8);
    CHECK(s.frame_length == sv_frame_length(RATE, frames[8].duration));
    CHECK(s.frame_left == s.frame_length);
    CHECK(s.interp_scale == (int16_t)(0x10000 / s.frame_length));

    /* phase_step = pitch << 9 (0x1c004676). */
    CHECK(s.dsp.phase_step == ((uint32_t)frames[8].pitch << 9));

    /* The crossfade weights always sum to 0x7f80 (0x1c004839). */
    {
        unsigned mix;
        for (mix = 0; mix < 256; mix += 37) {
            init_state(&s);
            s.frame_index = 7;
            frames[8].source_mix = (uint8_t)mix;
            CHECK(sv_frame_apply(&s, &sv_base_tables, frames) == SV_FRAMES_OK);
            CHECK((uint16_t)(s.dsp.mix_a + s.dsp.mix_b) == 0x7f80);
        }
        frames[8].source_mix = 0;
    }

    /* Above a voicing level of 0x3c the positive half-cycle is attenuated by
     * 3 and 4 bits respectively (0x1c0048d8); at or below it, it is not. */
    init_state(&s);
    s.frame_index = 7;
    frames[8].amp_voicing = 0x3c;
    frames[8].amp_aspiration = 40;
    frames[8].amp_frication = 40;
    CHECK(sv_frame_apply(&s, &sv_base_tables, frames) == SV_FRAMES_OK);
    CHECK((uint16_t)s.dsp.aspiration_positive
          == (uint16_t)((uint16_t)s.dsp.aspiration_negative >> 3));
    CHECK((uint16_t)s.dsp.frication_positive
          == (uint16_t)((uint16_t)s.dsp.frication_negative >> 4));

    init_state(&s);
    s.frame_index = 7;
    frames[8].amp_voicing = 0x3b;
    CHECK(sv_frame_apply(&s, &sv_base_tables, frames) == SV_FRAMES_OK);
    CHECK(s.dsp.aspiration_positive == s.dsp.aspiration_negative);
    CHECK(s.dsp.frication_positive == s.dsp.frication_negative);

    /* Two consecutive near-silent frames clear five filter histories and
     * leave the frication filter and the fifth resonator alone (0x1c0048fc). */
    init_state(&s);
    s.frame_index = 7;
    frames[7].amp_voicing = 0x0e;
    frames[7].amp_aspiration = 0x0e;
    frames[8].amp_voicing = 0x0e;
    frames[8].amp_aspiration = 0x0e;
    frames[8].amp_frication = 0;
    s.dsp.filters[SV_DSP_FORMANT1].previous = 0x1234;
    s.dsp.filters[SV_DSP_SOURCE_FILTER].previous2 = 0x1234;
    s.dsp.filters[SV_DSP_FRICATION_FILTER].previous = 0x1234;
    s.dsp.filters[SV_DSP_ASPIRATION_FILTER].previous = 0x1234;
    CHECK(sv_frame_apply(&s, &sv_base_tables, frames) == SV_FRAMES_OK);
    CHECK(s.dsp.filters[SV_DSP_FORMANT1].previous == 0);
    CHECK(s.dsp.filters[SV_DSP_SOURCE_FILTER].previous2 == 0);
    CHECK(s.dsp.filters[SV_DSP_FRICATION_FILTER].previous == 0x1234);
    CHECK(s.dsp.filters[SV_DSP_ASPIRATION_FILTER].previous == 0x1234);

    /* One frame above the threshold is enough to keep them. */
    init_state(&s);
    s.frame_index = 7;
    frames[7].amp_voicing = 0x0f;
    s.dsp.filters[SV_DSP_FORMANT1].previous = 0x1234;
    CHECK(sv_frame_apply(&s, &sv_base_tables, frames) == SV_FRAMES_OK);
    CHECK(s.dsp.filters[SV_DSP_FORMANT1].previous == 0x1234);
}

static void test_render(void)
{
    sv_frame_state s;
    sv_frame frames[64];
    uint8_t out[512];
    uint8_t again[512];
    unsigned i;

    build_frames(frames, 64, 63);

    /* A zero sample count wraps the original's counter into a 65536-sample
     * write; we refuse it. */
    init_state(&s);
    CHECK(sv_frame_render(&s, &sv_base_tables, frames, out, 0)
          == SV_FRAMES_E_COUNT);

    /* Without the no-events flag the renderer refuses before writing. */
    init_state(&s);
    s.flags = 0;
    CHECK(sv_frame_render(&s, &sv_base_tables, frames, out, 16)
          == SV_FRAMES_E_EVENTS);

    /* A stream that terminates on the first frame the reset reaches pads the
     * whole buffer with silence and stops speaking (0x1c00496e). */
    build_frames(frames, 64, 5);
    init_state(&s);
    memset(out, 0, sizeof(out));
    CHECK(sv_frame_render(&s, &sv_base_tables, frames, out, sizeof(out))
          == SV_FRAMES_END);
    CHECK(s.speaking == 0);
    for (i = 0; i < sizeof(out); ++i) {
        if (out[i] != 0x80) {
            printf("FAIL %s:%d: silence pad byte %u is %#x\n",
                   __FILE__, __LINE__, i, out[i]);
            ++failures;
            break;
        }
    }

    /* A long stream renders a full buffer and keeps speaking. */
    build_frames(frames, 64, 63);
    init_state(&s);
    CHECK(sv_frame_render(&s, &sv_base_tables, frames, out, sizeof(out))
          == SV_FRAMES_OK);
    CHECK(s.speaking == 1);
    CHECK(s.samples_left == 0);
    CHECK(s.restart == 0);

    /* Same state in, same samples out. */
    init_state(&s);
    CHECK(sv_frame_render(&s, &sv_base_tables, frames, again, sizeof(again))
          == SV_FRAMES_OK);
    CHECK(memcmp(out, again, sizeof(out)) == 0);

    /* The noise cursor and its countdown stay in lockstep: the cursor takes
     * two int16 per sample and the counter runs 0x3ff down to 1 before both
     * rewind (0x1c0044a0). The relation seen from outside is
     *     noise_index == 2 * (0x400 - noise_left)
     * and NOT 2 * (0x3ff - noise_left), because a call returns out of
     * 0x1c00449a as soon as the sample count reaches zero -- one instruction
     * before the counter it owes is decremented. That debt is paid on entry
     * to the next call. The off-by-one is the whole reason the resume path
     * has to exist, so it is worth pinning. */
    init_state(&s);
    for (i = 0; i < 24; ++i) {
        uint16_t n = (uint16_t)(7 + 13 * i);
        int rc = sv_frame_render(&s, &sv_base_tables, frames, out, n);
        CHECK(rc == SV_FRAMES_OK);
        CHECK(s.noise_left >= 1 && s.noise_left <= 0x3ff);
        CHECK(s.noise_index == 2u * (0x400u - s.noise_left));
        CHECK(s.noise_index + 1 < SV_NOISE_SAMPLES);
    }

    /* Resuming is not restarting. Rendering 2N samples in one call and in two
     * calls of N must produce the same audio -- that only holds because a
     * continuation re-enters at 0x1c0044a0 rather than at the sample loop. */
    {
        uint8_t whole[256];
        uint8_t split[256];

        init_state(&s);
        CHECK(sv_frame_render(&s, &sv_base_tables, frames, whole, 256)
              == SV_FRAMES_OK);
        init_state(&s);
        CHECK(sv_frame_render(&s, &sv_base_tables, frames, split, 128)
              == SV_FRAMES_OK);
        CHECK(s.restart == 0);
        CHECK(sv_frame_render(&s, &sv_base_tables, frames, split + 128, 128)
              == SV_FRAMES_OK);
        CHECK(memcmp(whole, split, sizeof(whole)) == 0);
    }
}

static void test_phoneme_names(void)
{
    unsigned taken;
    uint8_t code;

    /* A one-character name: the list head is the empty entry, so it matches
     * immediately and only one character is consumed. */
    taken = 0;
    code = sv_phoneme_lookup(&sv_base_tables_phonemes, ".", &taken);
    CHECK(code != SV_PHONEME_INVALID);
    CHECK(taken == 1);

    /* A two-character name consumes both. */
    taken = 0;
    code = sv_phoneme_lookup(&sv_base_tables_phonemes, "AE", &taken);
    CHECK(code != SV_PHONEME_INVALID);
    CHECK(taken == 2);

    /* An unknown second character still consumes both (0x1c004cfd). */
    taken = 0;
    code = sv_phoneme_lookup(&sv_base_tables_phonemes, "AQ", &taken);
    CHECK(code == SV_PHONEME_INVALID);
    CHECK(taken == 2);

    /* The run-of-H parity rule (0x1c004caa). "HH" is one name because the
     * number of H's after the first character is odd; "HHH" makes it even,
     * the second character is withheld from the list, and the lookup falls
     * through to the terminating entry. */
    taken = 0;
    code = sv_phoneme_lookup(&sv_base_tables_phonemes, "HH", &taken);
    CHECK(code != SV_PHONEME_INVALID);
    CHECK(taken == 2);

    taken = 0;
    code = sv_phoneme_lookup(&sv_base_tables_phonemes, "HHH", &taken);
    CHECK(code == SV_PHONEME_INVALID);

    /* '[' is a NULL list in this build; the original dereferences it, we
     * refuse. */
    taken = 0;
    code = sv_phoneme_lookup(&sv_base_tables_phonemes, "[", &taken);
    CHECK(code == SV_PHONEME_INVALID);
    CHECK(taken == 1);

    /* A character outside ' '..'[' falls back to list 1 (0x1c004c94), which
     * is the invalid one -- including a high-bit byte, because the compare is
     * signed. */
    taken = 0;
    CHECK(sv_phoneme_lookup(&sv_base_tables_phonemes, "\xe9", &taken)
          == SV_PHONEME_INVALID);
    taken = 0;
    CHECK(sv_phoneme_lookup(&sv_base_tables_phonemes, "!", &taken)
          == SV_PHONEME_INVALID);
}

int main(void)
{
    test_frame_length();
    test_reset();
    test_apply_refusals();
    test_amplitude_is_byte_indexed();
    test_apply_effects();
    test_render();
    test_phoneme_names();

    if (failures != 0) {
        printf("test_frames: %d check(s) failed\n", failures);
        return 1;
    }
    printf("test_frames: all checks passed\n");
    return 0;
}
