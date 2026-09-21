#ifndef TISPEECH_DSP_H
#define TISPEECH_DSP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native reconstruction of TIBASE32's integer waveform loop at 0x1c004120.
 * This consumes already prepared coefficients and excitation tables. It is
 * NOT yet a text/phoneme synthesizer: frame generation is a separate stage. */

typedef struct {
    uint32_t value; /* signed Q16.16, stored unsigned for defined wraparound */
    uint32_t step;
} sv_dsp_ramp;

typedef struct {
    int16_t previous;
    int16_t previous2;
    sv_dsp_ramp a;
    sv_dsp_ramp b;
} sv_dsp_filter;

enum {
    SV_DSP_SOURCE_FILTER,
    SV_DSP_ASPIRATION_FILTER,
    SV_DSP_FORMANT1,
    SV_DSP_FORMANT3,
    SV_DSP_FORMANT4,
    SV_DSP_FORMANT2,
    SV_DSP_FRICATION_FILTER,
    SV_DSP_FILTER_COUNT
};

typedef struct {
    uint32_t phase; /* 26-bit phase, ten table-index bits plus 16 fractional */
    uint32_t phase_step;
    int16_t mix_a;
    int16_t mix_b;
    sv_dsp_ramp voicing;
    int16_t previous_voice;
    int16_t aspiration_negative;
    int16_t aspiration_positive;
    int16_t frication_negative;
    int16_t frication_positive;
    int16_t frication_shape;
    sv_dsp_filter filters[SV_DSP_FILTER_COUNT];
} sv_dsp_state;

typedef struct {
    const int16_t *source_a; /* 1024 signed samples */
    const int16_t *source_b; /* 1024 signed samples */
    const uint8_t *output_curve; /* 401 bytes, original VA 0x1c0010fe */
} sv_dsp_tables;

/* Each call produces one unsigned 8-bit PCM sample and advances state.
 * noise_a/noise_b are consecutive samples from the original noise table.
 * The caller owns table bounds and frame timing; there are no DLL calls,
 * native audio APIs, instruction interpreters or allocation in this kernel. */
uint8_t sv_dsp_sample(sv_dsp_state *state, const sv_dsp_tables *tables,
                      int16_t noise_a, int16_t noise_b);

#ifdef __cplusplus
}
#endif
#endif
