// SPDX-License-Identifier: MIT

#include "shared/audioif_fp_contract.h"

#include "shared/audioif_modal.h"

#include "shared/audioif_trig.h"

#include <string.h>

// ln(1000) -- a mode's `decay` is its 60 dB time, so the per-sample pole
// radius is exp(-LN1000 / (decay * sample_rate)).
#define AUDIOIF_MODAL_LN1000 6.907755278982137

// Nothing here calls libm. `audioif_sincos()` supplies the angle, and the
// exponential below is a series with range reduction, for the reason
// audioif_trig.h gives: three platforms' `exp()` agree to within an ulp and
// differ in the last place, and this port's rule is that one hash of one
// probe matches on all three.

static float clampf(float value, float low, float high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

// Round half away from zero, then clamp -- the same conversion
// audioif_filter_f32.c and audioif_feedback_delay.c use, so a chain built out
// of several of them rounds one way.
static int16_t to_s16(float value) {
    if (value > 32767.0f) {
        return 32767;
    }
    if (value < -32768.0f) {
        return -32768;
    }
    return (int16_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

// True when one state word is small enough to be nothing.
static bool tiny(float value) {
    return value > -AUDIOIF_MODAL_FLUSH && value < AUDIOIF_MODAL_FLUSH;
}

// What makes a decaying float recursion actually arrive at zero, and here also
// what makes "this mode has finished" an exact test.
//
// BOTH WORDS OR NEITHER, and that is not tidiness. audioif_filter_f32.c
// flushes each word on its own, which is safe there; here it produced a stable
// limit cycle *above* the threshold, measured at 220 Hz / 0.125 s decay /
// 8 kHz: the state parked at ~2.1e-19 and was still there after two thousand
// blocks, thirty-eight seconds of audio, with the output long since silent.
//
// The mechanism, in the one place it can be seen. With no input the recursion
// is s1' = -a1*y0 + s2 and s2' = -a2*y0, y0 = s1, and -a1 is close to 2 for a
// high-Q pole (1.9567 for that mode). Flushing independently zeroes s2 first,
// because a2 < -a1 makes it the smaller word; the very next sample then
// computes s1' = 1.9567 * y0 + 0, which is nearly twice what it was. That
// pushes the pair back above the threshold, s2 refills from it, and the cycle
// repeats for ever. The flush was not ending the tail, it was feeding it.
//
// Zeroing the pair together cannot do that: the only state it can produce is
// the one where the whole mode is off.
static void flush_pair(float *s1, float *s2) {
    if (tiny(*s1) && tiny(*s2)) {
        *s1 = 0.0f;
        *s2 = 0.0f;
    }
}

// exp(-x) for x >= 0, in a fixed order on every platform.
//
// Halve the argument until it is under an eighth, evaluate seven Taylor terms
// there, then square back. Over [0, 1/8] the first dropped term is
// x^8/8! < 1.5e-12, which is four orders below float32's epsilon, and the
// squaring is exact in the sense that matters: it is the same sequence of
// multiplications everywhere. Range reduction rather than a longer series
// because the argument can reach ~0.9 (a one-millisecond decay on an 8 kHz
// graph) and a single polynomial wide enough for that is both longer and
// worse at the small end, which is where every real mode sits.
static double audioif_modal_exp_neg(double x) {
    int halvings = 0;
    if (x < 0.0) {
        x = 0.0;
    }
    while (x > 0.125 && halvings < 24) {
        x *= 0.5;
        halvings++;
    }
    // exp(-x) = 1 - x + x^2/2! - ... , Horner from the last term inward so
    // the additions happen in one fixed order.
    double t = -x;
    double y = 1.0 + t * (1.0 + t * (1.0 / 2.0 + t * (1.0 / 6.0
        + t * (1.0 / 24.0 + t * (1.0 / 120.0 + t * (1.0 / 720.0
        + t * (1.0 / 5040.0)))))));
    while (halvings-- > 0) {
        y *= y;
    }
    return y;
}

void audioif_modal_config_init(audioif_modal_config_t *config,
    uint32_t sample_rate, uint32_t channel_count, uint32_t mode_count,
    audioif_modal_mode_t *modes, audioif_modal_coeff_t *coeffs) {
    memset(config, 0, sizeof(*config));
    config->sample_rate = sample_rate < 1u ? 1u : sample_rate;
    config->channel_count = channel_count == 1u ? 1u : 2u;
    if (mode_count < 1u) {
        mode_count = 1u;
    }
    if (mode_count > AUDIOIF_MODAL_MAX_MODES) {
        mode_count = AUDIOIF_MODAL_MAX_MODES;
    }
    config->mode_count = mode_count;
    config->mix = 1.0f;
    config->gain = 1.0f;
    config->modes = modes;
    config->coeffs = coeffs;
    for (uint32_t i = 0; i < mode_count; ++i) {
        // Silent by default: a bank nobody has configured makes no sound
        // rather than a chord of whatever happened to be in memory.
        modes[i].frequency = 100.0f;
        modes[i].decay = AUDIOIF_MODAL_MIN_DECAY;
        modes[i].gain = 0.0f;
    }
    config->derived = false;
}

void audioif_modal_configure(audioif_modal_config_t *config,
    audioif_modal_option_t option, float value) {
    switch (option) {
        case AUDIOIF_MODAL_OPT_MIX:
            config->mix = clampf(value, 0.0f, 1.0f);
            break;
        case AUDIOIF_MODAL_OPT_GAIN:
            config->gain = clampf(value, 0.0f, 16.0f);
            break;
        default:
            break;
    }
}

void audioif_modal_set_mode(audioif_modal_config_t *config, uint32_t index,
    float frequency, float decay, float gain) {
    if (index >= config->mode_count || config->modes == NULL) {
        return;
    }
    const float nyquist = (float)config->sample_rate * 0.5f;
    audioif_modal_mode_t *mode = &config->modes[index];
    // A mode at or above Nyquist is not a mode, it is an alias. Clamped just
    // under rather than refused, so a table written for 48 kHz still loads on
    // a 22.05 kHz graph with its top partials folded onto the ceiling instead
    // of wrapping down into the middle of the drum.
    mode->frequency = clampf(frequency, 0.0f, nyquist * 0.999f);
    mode->decay = clampf(decay, AUDIOIF_MODAL_MIN_DECAY,
        AUDIOIF_MODAL_MAX_DECAY);
    mode->gain = clampf(gain, -16.0f, 16.0f);
    config->derived = false;
}

void audioif_modal_config_finish(audioif_modal_config_t *config) {
    if (config->derived || config->modes == NULL || config->coeffs == NULL) {
        return;
    }
    const double rate = (double)config->sample_rate;
    for (uint32_t i = 0; i < config->mode_count; ++i) {
        const audioif_modal_mode_t *mode = &config->modes[i];
        audioif_modal_coeff_t *coeff = &config->coeffs[i];
        if (mode->frequency <= 0.0f) {
            // No pole to speak of. This is the only case that clears the
            // recursion as well as the input.
            coeff->b0 = 0.0f;
            coeff->a1 = 0.0f;
            coeff->a2 = 0.0f;
            continue;
        }
        // A gain of zero clears b0 and NOTHING ELSE, which is the difference
        // between muting a mode and stopping it. `b0` is how new signal gets
        // in; `a1` and `a2` are the pole, and a mode already in motion has to
        // keep its pole or it does not decay, it simply ceases.
        //
        // That distinction is what lets one bank hold a whole drum kit. A kit
        // arms the drum being struck and zeroes the gain of every other, so
        // one excitation plays one drum - and a crash struck four bars ago
        // goes on ringing underneath, because muting its input never touched
        // its recursion. Written the other way first, a kick silenced a
        // ringing crash outright: 7584 peak to 61.
        const double w0 = 2.0 * AUDIOIF_PI * (double)mode->frequency / rate;
        audioif_sincos_t sc;
        audioif_sincos(w0, &sc);
        const double r = audioif_modal_exp_neg(
            AUDIOIF_MODAL_LN1000 / ((double)mode->decay * rate));
        // y[n] = b0*x[n] - a1*y[n-1] - a2*y[n-2] has impulse response
        // r^n * sin(w0*(n+1)) / sin(w0); scaling b0 by sin(w0) makes the peak
        // of that response the caller's `gain`, which is the number a modal
        // table out of the literature actually publishes.
        coeff->b0 = (float)((double)mode->gain * sc.s);
        coeff->a1 = (float)(-2.0 * r * sc.c);
        coeff->a2 = (float)(r * r);
    }
    config->derived = true;
}

uint32_t audioif_modal_state_floats(const audioif_modal_config_t *config) {
    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    return config->mode_count * channels;
}

void audioif_modal_state_init(audioif_modal_state_t *state,
    const audioif_modal_config_t *config, float *s1, float *s2,
    uint32_t words) {
    state->s1 = s1;
    state->s2 = s2;
    state->words = words;
    (void)config;
    audioif_modal_reset(state);
}

void audioif_modal_reset(audioif_modal_state_t *state) {
    if (state->s1 == NULL || state->s2 == NULL) {
        return;
    }
    for (uint32_t i = 0; i < state->words; ++i) {
        state->s1[i] = 0.0f;
        state->s2[i] = 0.0f;
    }
}

bool audioif_modal_silent(const audioif_modal_state_t *state) {
    if (state->s1 == NULL || state->s2 == NULL) {
        return true;
    }
    for (uint32_t i = 0; i < state->words; ++i) {
        if (state->s1[i] != 0.0f || state->s2[i] != 0.0f) {
            return false;
        }
    }
    return true;
}

void audioif_modal_process_s16(const audioif_modal_config_t *config,
    audioif_modal_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames) {
    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    const uint32_t modes = config->mode_count;
    if (state->s1 == NULL || state->s2 == NULL ||
        state->words < modes * channels || config->coeffs == NULL) {
        if (out != in && frames > 0u) {
            memcpy(out, in, (size_t)frames * channels * sizeof(int16_t));
        }
        return;
    }
    const float mix = config->mix;
    const float dry = 1.0f - mix;
    const float gain = config->gain * mix;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const size_t index = (size_t)frame * channels + channel;
            const float x0 = (float)in[index];
            float sum = 0.0f;
            for (uint32_t m = 0; m < modes; ++m) {
                const uint32_t w = m * channels + channel;
                const audioif_modal_coeff_t *c = &config->coeffs[m];
                // Skip only a mode that is taking nothing in AND holding
                // nothing: a muted mode (b0 zero) that is still ringing has
                // to keep advancing, or muting it would be a cut rather than
                // a mute. After the flush below "holding nothing" is exactly
                // zero rather than nearly zero, so this test is never wrong
                // by a fraction of an LSB, and a resident-but-finished drum
                // costs a compare instead of a recursion.
                if ((c->b0 == 0.0f || x0 == 0.0f) &&
                    state->s1[w] == 0.0f && state->s2[w] == 0.0f) {
                    continue;
                }
                // Transposed direct form II -- see the state struct in the
                // header for why this shape and not direct form I.
                const float y0 = c->b0 * x0 + state->s1[w];
                state->s1[w] = -c->a1 * y0 + state->s2[w];
                state->s2[w] = -c->a2 * y0;
                flush_pair(&state->s1[w], &state->s2[w]);
                sum += y0;
            }
            out[index] = to_s16(dry * x0 + gain * sum);
        }
    }
}
