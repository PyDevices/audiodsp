// SPDX-License-Identifier: MIT

#include "shared/audioif_fp_contract.h"

#include "shared/audioif_filter_f32.h"

#include "shared/audioif_trig.h"

#include <math.h>
#include <string.h>

// ln(10)/40 -- the RBJ shelf/peak amplitude, A = 10^(dB/40). Written as a
// multiply into expf() the way audioif_dynamics.c:17 writes its own dB
// conversion, so the two agree about what a decibel is.
#define AUDIOIF_FILTER_F32_DB_TO_A 0.05756462732485115f

// Q has to stay off the unit circle or the tail this module exists to end
// never ends: the pole radius of an RBJ section is sqrt((1 - alpha) /
// (1 + alpha)) with alpha = sin(W0) / (2Q), so alpha -> 0 is a filter that
// rings for ever. At Q = 60 and 1 kHz / 48 kHz the state decays past the
// flush threshold in about 51,000 samples (roughly a second), which is a real
// bound; at Q = 600 it would be ten seconds and at Q = 6000 a hundred.
#define AUDIOIF_FILTER_F32_MIN_Q 0.05f
#define AUDIOIF_FILTER_F32_MAX_Q 60.0f

// Same argument one order down. c = (1 - t)/(1 + t) reaches exactly 1 at
// frequency 0, and a first-order all-pass with c = 1 holds its state for
// ever. Capped just below, so the worst case decays past the flush threshold
// in about 550,000 samples rather than never.
#define AUDIOIF_FILTER_F32_MAX_ALLPASS_C 0.9999f

static float clampf(float value, float low, float high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

// Round half away from zero, then clamp -- `audioif_feedback_delay.c`'s
// to_s16(), kept identical so two own-module nodes in one chain quantize the
// same way.
static int16_t to_s16(float value) {
    if (value > 32767.0f) {
        return 32767;
    }
    if (value < -32768.0f) {
        return -32768;
    }
    return (int16_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

// The whole point of the module, in three lines. A `float` recursion decays
// geometrically and so never actually arrives; this is what makes it arrive.
static float flush(float value) {
    if (value > -AUDIOIF_FILTER_F32_FLUSH &&
        value < AUDIOIF_FILTER_F32_FLUSH) {
        return 0.0f;
    }
    return value;
}

// ---------------------------------------------------------------- biquad --

void audioif_biquad_f32_config_init(audioif_biquad_f32_config_t *config,
    uint32_t sample_rate, uint32_t channel_count) {
    memset(config, 0, sizeof(*config));
    config->sample_rate = sample_rate < 1u ? 1u : sample_rate;
    config->channel_count = channel_count == 1u ? 1u : 2u;
    config->mode = AUDIOIF_BIQUAD_F32_LOW_PASS;
    config->frequency = 1000.0f;
    config->q = 0.7071067811865475f;
    config->gain_db = 0.0f;
    config->mix = 1.0f;
    config->derived = false;
}

void audioif_biquad_f32_configure(audioif_biquad_f32_config_t *config,
    audioif_biquad_f32_option_t option, float value) {
    switch (option) {
        case AUDIOIF_BIQUAD_F32_OPT_MODE: {
            int32_t mode = (int32_t)value;
            if (mode < AUDIOIF_BIQUAD_F32_LOW_PASS) {
                mode = AUDIOIF_BIQUAD_F32_LOW_PASS;
            }
            if (mode > AUDIOIF_BIQUAD_F32_HIGH_SHELF) {
                mode = AUDIOIF_BIQUAD_F32_HIGH_SHELF;
            }
            config->mode = mode;
            break;
        }
        case AUDIOIF_BIQUAD_F32_OPT_FREQUENCY:
            // Nyquist itself is not allowed: W0 = pi puts sin(W0) at zero and
            // the section degenerates.
            config->frequency = clampf(value, 0.0f,
                (float)config->sample_rate * 0.4999f);
            break;
        case AUDIOIF_BIQUAD_F32_OPT_Q:
            config->q = clampf(value, AUDIOIF_FILTER_F32_MIN_Q,
                AUDIOIF_FILTER_F32_MAX_Q);
            break;
        case AUDIOIF_BIQUAD_F32_OPT_GAIN_DB:
            config->gain_db = clampf(value, -60.0f, 60.0f);
            break;
        case AUDIOIF_BIQUAD_F32_OPT_MIX:
            config->mix = clampf(value, 0.0f, 1.0f);
            break;
        default:
            break;
    }
}

void audioif_biquad_f32_config_finish(audioif_biquad_f32_config_t *config) {
    if (config->derived &&
        config->derived_mode == config->mode &&
        config->derived_sample_rate == config->sample_rate &&
        config->derived_frequency == config->frequency &&
        config->derived_q == config->q &&
        config->derived_gain_db == config->gain_db) {
        return;
    }

    // The cookbook algebra, copied rather than shared: `audioif_biquad.c` is
    // ported CircuitPython and frozen, and what it does after this point --
    // pick a fixed-point shift and quantize into it -- is the half this
    // module exists to avoid. What *is* shared is the trig
    // (audioif_trig.c:72), so at one W0 both kernels start from the same sine
    // and cosine and a comparison between them measures the arithmetic rather
    // than two different libm builds.
    const double W0 = (double)config->frequency * (2 * AUDIOIF_PI) /
        (double)config->sample_rate;
    const double Q = (double)config->q;
    const double A = (double)expf(config->gain_db *
        AUDIOIF_FILTER_F32_DB_TO_A);
    audioif_sincos_t sc;
    audioif_sincos_reflect(W0, &sc);
    const double alpha = sc.s / (2 * Q);
    double a0, a1, a2, b0, b1, b2;
    if (config->mode < AUDIOIF_BIQUAD_F32_PEAKING_EQ) {
        a0 = 1 + alpha;
        a1 = -2 * sc.c;
        a2 = 1 - alpha;
        if (config->mode == AUDIOIF_BIQUAD_F32_LOW_PASS) {
            b2 = b0 = (1 - sc.c) * .5;
            b1 = 1 - sc.c;
        } else if (config->mode == AUDIOIF_BIQUAD_F32_HIGH_PASS) {
            b2 = b0 = (1 + sc.c) * .5;
            b1 = -(1 + sc.c);
        } else if (config->mode == AUDIOIF_BIQUAD_F32_BAND_PASS) {
            b0 = alpha;
            b1 = 0;
            b2 = -b0;
        } else {
            b0 = 1;
            b1 = -2 * sc.c;
            b2 = 1;
        }
    } else if (config->mode == AUDIOIF_BIQUAD_F32_PEAKING_EQ) {
        // `1 - alpha * A`, the sign audioif_biquad.c:87 corrected upstream to
        // (docs/upstream-diff.md, "synthio.Biquad's PEAKING_EQ"): with the
        // plus, numerator and denominator stop summing alike at DC and a
        // +6 dB bell at 1 kHz comes out about +21 dB down there.
        b0 = 1 + alpha * A;
        b1 = -2 * sc.c;
        b2 = 1 - alpha * A;
        a0 = 1 + alpha / A;
        a1 = -2 * sc.c;
        a2 = 1 - alpha / A;
    } else {
        // sqrt() rather than audioif_biquad.c:26's Newton-step reciprocal
        // approximation. That trick buys speed on a part with no divider and
        // costs about 0.2% of A; it is there because the result is about to be
        // quantized into 23 bits anyway. Nothing here is quantized, and IEEE
        // sqrt is correctly rounded, so this is both more accurate and the
        // same on every target.
        const double root = sqrt(A);
        if (config->mode == AUDIOIF_BIQUAD_F32_LOW_SHELF) {
            b0 = A * ((A + 1) - (A - 1) * sc.c + 2 * root * alpha);
            b1 = 2 * A * ((A - 1) - (A + 1) * sc.c);
            b2 = A * ((A + 1) - (A - 1) * sc.c - 2 * root * alpha);
            a0 = (A + 1) + (A - 1) * sc.c + 2 * root * alpha;
            a1 = -2 * ((A - 1) + (A + 1) * sc.c);
            a2 = (A + 1) + (A - 1) * sc.c - 2 * root * alpha;
        } else {
            b0 = A * ((A + 1) + (A - 1) * sc.c + 2 * root * alpha);
            b1 = -2 * A * ((A - 1) + (A + 1) * sc.c);
            b2 = A * ((A + 1) + (A - 1) * sc.c - 2 * root * alpha);
            a0 = (A + 1) - (A - 1) * sc.c + 2 * root * alpha;
            a1 = 2 * ((A - 1) - (A + 1) * sc.c);
            a2 = (A + 1) - (A - 1) * sc.c - 2 * root * alpha;
        }
    }
    const double reciprocal = 1 / a0;
    config->b0 = (float)(b0 * reciprocal);
    config->b1 = (float)(b1 * reciprocal);
    config->b2 = (float)(b2 * reciprocal);
    config->a1 = (float)(a1 * reciprocal);
    config->a2 = (float)(a2 * reciprocal);

    config->derived_mode = config->mode;
    config->derived_sample_rate = config->sample_rate;
    config->derived_frequency = config->frequency;
    config->derived_q = config->q;
    config->derived_gain_db = config->gain_db;
    config->derived = true;
}

void audioif_biquad_f32_state_init(audioif_biquad_f32_state_t *state) {
    memset(state, 0, sizeof(*state));
}

void audioif_biquad_f32_reset(audioif_biquad_f32_state_t *state) {
    memset(state, 0, sizeof(*state));
}

void audioif_biquad_f32_process_s16(const audioif_biquad_f32_config_t *config,
    audioif_biquad_f32_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames) {
    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    const float b0 = config->b0, b1 = config->b1, b2 = config->b2;
    const float a1 = config->a1, a2 = config->a2;
    const float mix = config->mix;
    const float dry = 1.0f - mix;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const size_t index = (size_t)frame * channels + channel;
            const float x0 = (float)in[index];
            // Transposed direct form II -- see the state struct for why this
            // shape and not direct form I (audioif#64).
            const float y0 = b0 * x0 + state->s1[channel];
            // Both words ARE the memory, so both need the denormal flush that
            // direct form I only had to give `y`. Flushing before the store
            // means the exact zero is what the next sample reads, which is what
            // makes the tail arrive rather than decay forever.
            state->s1[channel] =
                flush(b1 * x0 - a1 * y0 + state->s2[channel]);
            state->s2[channel] = flush(b2 * x0 - a2 * y0);
            out[index] = to_s16(dry * x0 + mix * y0);
        }
    }
}

// -------------------------------------------------------------- all-pass --

void audioif_allpass_f32_config_init(audioif_allpass_f32_config_t *config,
    uint32_t sample_rate, uint32_t channel_count, uint32_t stages) {
    memset(config, 0, sizeof(*config));
    config->sample_rate = sample_rate < 1u ? 1u : sample_rate;
    config->channel_count = channel_count == 1u ? 1u : 2u;
    if (stages < 1u) {
        stages = 1u;
    }
    if (stages > AUDIOIF_FILTER_F32_MAX_STAGES) {
        stages = AUDIOIF_FILTER_F32_MAX_STAGES;
    }
    config->stages = stages;
    config->frequency = 1000.0f;
    config->feedback = 0.0f;
    config->mix = 1.0f;
    config->derived = false;
}

void audioif_allpass_f32_configure(audioif_allpass_f32_config_t *config,
    audioif_allpass_f32_option_t option, float value) {
    switch (option) {
        case AUDIOIF_ALLPASS_F32_OPT_FREQUENCY:
            config->frequency = clampf(value, 0.0f,
                (float)config->sample_rate * 0.4999f);
            break;
        case AUDIOIF_ALLPASS_F32_OPT_FEEDBACK:
            config->feedback = clampf(value, -0.99f, 0.99f);
            break;
        case AUDIOIF_ALLPASS_F32_OPT_MIX:
            config->mix = clampf(value, 0.0f, 1.0f);
            break;
        default:
            break;
    }
}

void audioif_allpass_f32_config_finish(audioif_allpass_f32_config_t *config) {
    if (config->derived &&
        config->derived_sample_rate == config->sample_rate &&
        config->derived_frequency == config->frequency) {
        return;
    }
    // The bilinear transform, done properly. With t = tan(pi f / fs), a
    // stage's phase is -2*atan(((1 - c)/(1 + c)) * tan(w/2)), so c = (t - 1)
    // / (t + 1) puts its -90 degree point exactly at f.
    //
    // `audioif_phaser.c` reaches the same convention by a different route and
    // stops one approximation short: its coefficient is
    // (1 - f/nyquist)/(1 + f/nyquist) (`:14`) and its recursion multiplies by
    // the *negative* of it (`:32`), so in this module's terms its stage
    // coefficient is -(1 - f/nyquist)/(1 + f/nyquist) -- the small-angle form
    // of (t - 1)/(t + 1), with f/nyquist standing in for tan(pi f / fs). That
    // lands the break at (fs/pi)*atan(2f/fs), about 0.64 f low down, which is
    // why the ported node's `frequency` is not the notch frequency and why
    // its class has to pre-warp in Python (audiofilters/Phaser.c:210).
    //
    // tan is sin/cos from the shared deterministic pair rather than libm's,
    // for the reason audioif_trig.h gives: one polynomial evaluated in one
    // order is one function on every target, and this coefficient is inside a
    // recursion whose golden is hashed byte for byte.
    audioif_sincos_t sc;
    const double half = (double)config->frequency * AUDIOIF_PI /
        (double)config->sample_rate;
    audioif_sincos_reflect(half, &sc);
    const double t = sc.c == 0.0 ? 1e9 : sc.s / sc.c;
    config->coefficient = clampf((float)((t - 1.0) / (t + 1.0)),
        -AUDIOIF_FILTER_F32_MAX_ALLPASS_C, AUDIOIF_FILTER_F32_MAX_ALLPASS_C);
    config->derived_sample_rate = config->sample_rate;
    config->derived_frequency = config->frequency;
    config->derived = true;
}

void audioif_allpass_f32_state_init(audioif_allpass_f32_state_t *state,
    float *stage_state, uint32_t stage_state_count) {
    state->stage_state = stage_state;
    state->stage_state_count = stage_state == NULL ? 0u : stage_state_count;
    state->loop[0] = state->loop[1] = 0.0f;
    state->coefficient = 0.0f;
    state->primed = false;
    if (state->stage_state != NULL) {
        memset(state->stage_state, 0,
            (size_t)state->stage_state_count * sizeof(float));
    }
}

void audioif_allpass_f32_reset(audioif_allpass_f32_state_t *state) {
    audioif_allpass_f32_state_init(state, state->stage_state,
        state->stage_state_count);
}

void audioif_allpass_f32_process_s16(
    const audioif_allpass_f32_config_t *config,
    audioif_allpass_f32_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames) {
    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    const uint32_t stages = config->stages;
    if (state->stage_state == NULL ||
        state->stage_state_count < channels * stages || frames == 0u) {
        if (out != in) {
            memcpy(out, in, (size_t)frames * channels * sizeof(int16_t));
        }
        return;
    }
    const float mix = config->mix;
    const float dry = 1.0f - mix;
    const float feedback = config->feedback;
    const float target = config->coefficient;
    // The block's value arrives at the block boundary; the coefficient walks
    // there over the block instead of stepping. That is the difference
    // between a sweep that glides and one that buzzes at the block rate --
    // 187 Hz at 48 kHz with 256-frame blocks, right in the middle of the
    // range a phaser sweeps through.
    if (!state->primed) {
        state->coefficient = target;
        state->primed = true;
    }
    const float step = (target - state->coefficient) / (float)frames;
    float coefficient = state->coefficient;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        const float c = coefficient;
        coefficient += step;
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const size_t index = (size_t)frame * channels + channel;
            const float source = (float)in[index];
            float value = source + feedback * state->loop[channel];
            float *lane = state->stage_state + (size_t)channel * stages;
            for (uint32_t stage = 0; stage < stages; ++stage) {
                // Transposed direct form II of H(z) = (c + z^-1)/(1 + c z^-1)
                // -- two multiplies and one state word per stage, the same
                // count audioif_phaser.c:32-35 pays in int16.
                const float stage_out = c * value + lane[stage];
                lane[stage] = flush(value - c * stage_out);
                value = stage_out;
            }
            state->loop[channel] = flush(value);
            out[index] = to_s16(dry * source + mix * value);
        }
    }
    state->coefficient = target;
}
