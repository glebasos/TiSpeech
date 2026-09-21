#include "tispeech/dsp.h"

/* Explicit narrowing avoids signed overflow and implementation-defined
 * unsigned-to-signed conversion. The original loop wraps at every 16-bit
 * add/subtract and takes the indicated middle bits of signed products. */
static int16_t narrow(uint32_t value)
{
    uint32_t low = value & 0xffffu;
    return (int16_t)((int32_t)low - ((low & 0x8000u) ? 65536 : 0));
}

static int16_t high(uint32_t value)
{
    return narrow(value >> 16);
}

static int16_t multiply(int16_t a, int16_t b, unsigned shift)
{
    int32_t product = (int32_t)a * (int32_t)b;
    return narrow((uint32_t)product >> shift);
}

static int16_t arithmetic_shift(int16_t value, unsigned bits)
{
    int32_t divisor = (int32_t)(1u << bits);
    int32_t v = value;
    return (int16_t)(v >= 0 ? v / divisor : -((-v + divisor - 1) / divisor));
}

static int16_t filter_sample(sv_dsp_filter *filter, int16_t input, int interpolate)
{
    int16_t d1 = narrow((uint32_t)((int32_t)filter->previous - input));
    int16_t d2 = narrow((uint32_t)((int32_t)filter->previous2 - input));
    int16_t output = narrow((uint32_t)((int32_t)input
        - multiply(d2, high(filter->b.value), 15)
        + multiply(d1, high(filter->a.value), 14)));
    filter->previous2 = filter->previous;
    filter->previous = output;
    if (interpolate) {
        filter->a.value += filter->a.step;
        filter->b.value += filter->b.step;
    }
    return output;
}

uint8_t sv_dsp_sample(sv_dsp_state *s, const sv_dsp_tables *tables,
                      int16_t noise_a, int16_t noise_b)
{
    s->phase = (s->phase + s->phase_step) & 0x03ffffffu;
    unsigned index = s->phase >> 16;
    int16_t source = narrow((uint32_t)(
        (int32_t)multiply(tables->source_a[index], s->mix_a, 15)
        + multiply(tables->source_b[index], s->mix_b, 15)));
    int16_t voiced = multiply(source, high(s->voicing.value), 15);
    s->voicing.value += s->voicing.step;

    int16_t aspiration = voiced < 0 ? s->aspiration_negative : s->aspiration_positive;
    int16_t frication = voiced < 0 ? s->frication_negative : s->frication_positive;
    int16_t differentiated = narrow((uint32_t)(2 * ((int32_t)voiced - s->previous_voice)));
    s->previous_voice = voiced;

    int16_t signal = filter_sample(&s->filters[SV_DSP_SOURCE_FILTER], differentiated, 1);
    signal = narrow((uint32_t)((int32_t)signal + multiply(noise_a, aspiration, 15)));
    signal = filter_sample(&s->filters[SV_DSP_ASPIRATION_FILTER], signal, 0);
    signal = filter_sample(&s->filters[SV_DSP_FORMANT1], signal, 1);
    signal = filter_sample(&s->filters[SV_DSP_FORMANT3], signal, 1);
    signal = filter_sample(&s->filters[SV_DSP_FORMANT4], signal, 0);
    signal = filter_sample(&s->filters[SV_DSP_FORMANT2], signal, 1);

    int16_t noise = filter_sample(&s->filters[SV_DSP_FRICATION_FILTER],
                                 arithmetic_shift(noise_b, 2), 0);
    noise = narrow((uint32_t)((int32_t)multiply(noise, s->frication_shape, 15)
                             - arithmetic_shift(noise_b, 7)));
    int32_t product = (int32_t)noise * frication;
    /* Two successive SHLDs reuse the same unshifted low product word.
     * Replacing this with product >> 12 changes the original's rounding. */
    uint16_t middle = (uint16_t)((uint32_t)product >> 15);
    uint16_t shaped = (uint16_t)(((uint32_t)middle << 3)
                                | ((uint16_t)product >> 13));
    int16_t mixed = narrow((uint32_t)((int32_t)signal + narrow(shaped)));
    int16_t level = arithmetic_shift(mixed, 5);
    if (level < -200) level = -200;
    if (level > 200) level = 200;
    return (uint8_t)(tables->output_curve[level + 200] + 128u);
}
