#ifndef TISPEECH_SMOOTHING_H
#define TISPEECH_SMOOTHING_H

#include "tispeech/frames.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pitch-contour interpolation: TIBASE32 0x1c00cd40..0x1c00cdb3.
 *
 * Fills runs of zero-pitch frames between nonzero anchors, starting with
 * frames[0] as the first anchor even if its pitch is zero. Endpoints are
 * converted to Q24.8; the signed step is (end - start) / (run + 1), truncated
 * toward zero as by x86 IDIV. Each interior frame receives the high 16 bits
 * of the incremented accumulator after shifting right by 8.
 *
 * The terminator check at 0x1c00cd73 precedes interpolation: a trailing run
 * ending at SV_FRAME_END is left untouched, not interpolated toward the
 * terminator's pitch. No other frame fields are modified; no tables are used.
 *
 * Both endpoints are zero-extended uint16_t pitches shifted left 8. Truncating
 * the step toward zero keeps each intermediate value between the endpoints
 * (inclusive when the step is zero). Thus the accumulator is non-negative,
 * fits int32_t, and C's right shift agrees with the original SAR.
 *
 * PRECONDITIONS: frames is non-NULL and contains a non-terminator first frame
 * followed by an accessible terminator; the terminator has nonzero pitch.
 * The original generator sets its pitch to 0xffff at 0x1c0058d5, which stops
 * the zero-pitch scan at the terminator before the SV_FRAME_END check. Like
 * the original this function has no length argument: an unterminated array,
 * a zero-pitch terminator, or a terminator-only array is not supported.
 * The array must contain fewer than INT32_MAX frames (the original's counter
 * is signed 32-bit). Generator-produced arrays have a four-frame preamble.
 *
 * ORIGINAL ABI: cdecl(state *), reading the frame-array base from state+0xf6
 * at 0x1c00cd4a; it neither reads other state fields nor writes state. This
 * reconstruction takes the array directly, like the existing frame renderer.
 * State+0xf6 is distinct from the renderer's cursor at +0xfa.
 *
 * Call sequence recovered at 0x1c0039ec..0x1c003a21:
 *   generate(state, 0)       0x1c005840
 *   smooth_pitch(state)     0x1c00cd40  [this pass]
 *   slew_pitch(state)       0x1c00de60  [called twice; sv_smooth_pitch_slew]
 *   remaining_pass(state)   0x1c00be40  [not reconstructed]
 *   nothing                 0x1c00df10  [a single `retl`; see REVERSING.md]
 *   state->restart = 0xff   state+0x30e
 * All post-generation calls take one state-pointer argument. This pass alone
 * does not complete the pipeline or enable speech synthesis.
 *
 * tools/verify_smoothing.py compares whole frame arrays and state against the
 * original DLL under Unicorn. tests/test_smoothing.c needs no proprietary data.
 */
void sv_smooth_pitch(sv_frame *frames);

/* Pitch slew limiting: TIBASE32 0x1c00de60..0x1c00df00.
 *
 * The pass that runs immediately after sv_smooth_pitch(), TWICE in a row
 * (0x1c003a01 and 0x1c003a0a, same argument both times). Where the previous
 * pass fills gaps with straight lines, this one rounds the corners: a one-pole
 * lag over the pitch track, run forward and then backward over the same
 * accumulator.
 *
 *   acc starts at frames[0].pitch << 8                      0x1c00de85
 *   acc += (frame->pitch - (acc >> 8)) * rate               0x1c00deaf..0x1c00deb4
 *   frame->pitch = acc >> 8                                 0x1c00debb
 *
 * so a `rate` of 256 snaps to the frame's own value and leaves the track
 * unchanged, 0 freezes the accumulator, and values between lag behind it.
 * That is the same Q8 convention as the generator's interpolation-rate table.
 *
 * The rate is chosen per frame from two, by comparing the frame's pitch with
 * the accumulator:
 *
 *   forward  (0x1c00de9c, `jg`)    pitch << 8 >  acc  -> rise, else fall
 *   backward (0x1c00ded7, `jge`)   pitch << 8 >= acc  -> fall, else rise
 *
 * The two tests are opposite because the second pass walks backwards: in both
 * passes `rate_rise` is the one applied where the pitch RISES in forward time.
 *
 * Which rate an exact tie takes is UNOBSERVABLE, and the reconstruction only
 * matches the instructions rather than being pinned by the differential.
 * `pitch << 8 == acc` implies `acc >> 8 == pitch`, so the delta is zero and
 * the step is zero whichever rate was selected. Flipping the backward pass's
 * `>=` to `>` still passes 300 randomized cases; the other three mutations
 * tried (swapping the forward pass's rates, shifting the product as the
 * generator does, and dropping the 32-bit wrap) are all caught immediately.
 * Recorded because the asymmetry looks like a bug on first reading and is
 * not one. The original reads the pair from
 * its state block at +0x64 and +0x66, which 0x1c00df20 copies there from the
 * voice parameter block at +0xd0 (+0x18 and +0x1a within it) -- they are voice
 * settings, not anything this pass derives.
 *
 * The forward pass covers frames [0, n) where n is the index of the
 * terminator; the backward pass covers (0, n), i.e. every frame the forward
 * pass touched except frame 0 (the counter is decremented once at 0x1c00dec4
 * before the backward loop). Frame 0 is written by the forward pass with a
 * delta of zero, so its value never changes. The accumulator is NOT reset
 * between the passes (0x1c00dec4 leaves ecx alone), which is why the backward
 * pass starts from wherever the forward one ended rather than from the last
 * frame's pitch.
 *
 * Every step wraps: the delta subtract, the multiply and the accumulate are
 * 32-bit, and the store is 16. With the engine's own rates (0..256) none of
 * that is reachable, but the reconstruction reproduces it rather than assuming
 * the caller is the engine -- see src/smoothing.c.
 *
 * PRECONDITIONS: as sv_smooth_pitch() above -- a terminated array, fewer than
 * INT32_MAX frames, and no length argument because the original has none.
 * Unlike that pass this one does not depend on the terminator's pitch value.
 *
 * ORIGINAL ABI: cdecl(state *), reading the frame array from state+0xf6 and
 * the two rates from state+0x64/+0x66. It writes nothing back to the state.
 */
void sv_smooth_pitch_slew(sv_frame *frames, uint16_t rate_rise,
                          uint16_t rate_fall);

#ifdef __cplusplus
}
#endif
#endif
