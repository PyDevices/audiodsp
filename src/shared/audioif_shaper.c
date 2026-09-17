// Runtime-neutral oversampled table waveshaper. See audioif_shaper.h for what
// it is for and why none of it could be composed out of the palette.
//
// `float` working precision, deliberately, to match audioif_dynamics.c and
// audioif_feedback_delay.c: this runs per *oversampled* sample on parts
// without an FPU at all, and the table it reads is int16 to begin with.
//
// SPDX-License-Identifier: MIT

#include "shared/audioif_fp_contract.h"

#include "shared/audioif_shaper.h"

#include <string.h>

// Two-path polyphase all-pass half-band coefficients: branch 0 (the even
// output phase) and branch 1 (the odd one), each a cascade of first-order
// all-passes (a + z^-1)/(1 + a z^-1) running at that branch's own rate.
//
// Designed here rather than quoted from anywhere: tools/design_halfband.py
// runs a minimax search over the four coefficients for the stopband peak of
//
//     H(w) = ( A0(e^{j2w}) + e^{-jw} A1(e^{j2w}) ) / 2
//
// over w in [0.5833*pi, pi]. That stopband edge is what a 48 kHz base rate
// needs of the *last* decimation stage: it runs at 96 kHz, folds everything
// above 24 kHz back down, and has to be clear of it by 20 kHz -- so the
// transition band is 20..28 kHz and the passband reaches 0.4167*pi. The
// earlier stages of a 4x or 8x chain have far more room, and reuse the same
// four numbers rather than carrying a second table.
//
// Measured with these four values (tools/design_halfband.py --verify):
// stopband peak -64.57 dB, passband ripple under 0.0001 dB out to 0.4167*pi,
// and exactly 0.7071 at pi/2, which is the half-band condition. Every
// coefficient is in (0,1), so every pole sits at |z| = sqrt(a) < 1 and each
// branch is stable.
//
// Four is enough, and that is a measurement rather than a preference: with
// an identity curve loaded, the whole up/down chain's non-harmonic energy is
// 88-89 dB below the fundamental at every factor -- the int16 output's own
// quantisation floor. A six-coefficient design (-68.8 dB in the same search)
// would move nothing that can be measured through this node.
static const float audioif_shaper_hb_coefficients[2][AUDIOIF_SHAPER_HB_SECTIONS] = {
    { 0.0903576217f, 0.5794053552f },
    { 0.3127017166f, 0.8516878519f },
};

static float clampf(float value, float low, float high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static int16_t to_s16(float value) {
    if (value > 32767.0f) {
        return 32767;
    }
    if (value < -32768.0f) {
        return -32768;
    }
    return (int16_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

void audioif_shaper_set_oversample(audioif_shaper_config_t *config,
    uint32_t oversample) {
    uint32_t stages = 0;
    while (stages < AUDIOIF_SHAPER_MAX_STAGES && (2u << stages) <= oversample) {
        ++stages;
    }
    config->stages = stages;
    config->oversample = 1u << stages;
}

void audioif_shaper_set_channel_count(audioif_shaper_config_t *config,
    uint32_t channel_count) {
    config->channel_count = channel_count == 1u ? 1u : 2u;
}

void audioif_shaper_set_curve(audioif_shaper_config_t *config,
    const int16_t *curve, uint32_t points) {
    if (curve == NULL || points < 2u) {
        return;
    }
    config->curve = curve;
    config->curve_points = points;
}

void audioif_shaper_config_init(audioif_shaper_config_t *config,
    uint32_t sample_rate, uint32_t oversample) {
    config->sample_rate = sample_rate ? sample_rate : 48000;
    config->channel_count = 2;
    audioif_shaper_set_oversample(config, oversample);
    config->curve = NULL;
    config->curve_points = 0;
    config->pre_gain = 1.0f;
    config->bias = 0.0f;
    config->post_gain = 1.0f;
    config->mix = 1.0f;
    config->hysteresis = 0.0f;
    config->hysteresis_width = 0.02f;
    config->hysteresis_bias = 0.0f;
    audioif_shaper_config_finish(config);
}

void audioif_shaper_configure(audioif_shaper_config_t *config,
    audioif_shaper_option_t option, float value) {
    switch (option) {
        case AUDIOIF_SHAPER_OPT_PRE_GAIN:
            // Wide open on purpose: the drive knob is gain into a normalised
            // curve, and a fuzz asks for 40 dB of it. The curve's own ends
            // are what stop it running away.
            config->pre_gain = clampf(value, -1000.0f, 1000.0f);
            break;
        case AUDIOIF_SHAPER_OPT_BIAS:
            config->bias = clampf(value, -1.0f, 1.0f);
            break;
        case AUDIOIF_SHAPER_OPT_POST_GAIN:
            config->post_gain = clampf(value, -1000.0f, 1000.0f);
            break;
        case AUDIOIF_SHAPER_OPT_MIX:
            config->mix = clampf(value, 0.0f, 1.0f);
            break;
        case AUDIOIF_SHAPER_OPT_HYSTERESIS:
            config->hysteresis = clampf(value, 0.0f, 1.0f);
            break;
        case AUDIOIF_SHAPER_OPT_HYSTERESIS_WIDTH:
            // Half the curve's whole input span is the ceiling: past that the
            // operator would never reach either end of the table.
            config->hysteresis_width = clampf(value, 0.0f, 0.5f);
            break;
        case AUDIOIF_SHAPER_OPT_HYSTERESIS_BIAS:
            config->hysteresis_bias = clampf(value, -1.0f, 1.0f);
            break;
    }
}

void audioif_shaper_config_finish(audioif_shaper_config_t *config) {
    // `hysteresis_width` is a half-width in *input* units -- a coercivity, a
    // fraction of full scale at the node's input -- but the operator runs
    // after `pre_gain`, so it is converted here, once, rather than per
    // sample. Without that factor the drive knob would smear the loop: at
    // twice the gain the same absolute half-width covers half as much of the
    // input's swing, and the enclosed area would *fall* as drive rose, which
    // is the direction the palette's two memory elements already fail in
    // (audioecho.FeedbackDelay's area is largest with its nonlinearity off;
    // audiodynamics.Dynamics' shrinks with drive). With it, the loop width
    // in input units is 2 * hysteresis * hysteresis_width whatever the drive,
    // and the only thing that moves it is the knob that is meant to.
    float gain = config->pre_gain < 0.0f ? -config->pre_gain :
        config->pre_gain;
    float width = config->hysteresis * config->hysteresis_width * gain;
    if (width > 1.0f) {
        width = 1.0f;
    }
    config->width_up = width * (1.0f + config->hysteresis_bias);
    config->width_down = width * (1.0f - config->hysteresis_bias);
}

void audioif_shaper_state_init(audioif_shaper_state_t *state) {
    memset(state, 0, sizeof(*state));
}

void audioif_shaper_reset(audioif_shaper_state_t *state) {
    audioif_shaper_state_init(state);
}

// One branch of one half-band: a cascade of first-order all-passes,
// y[n] = a*(x[n] - y[n-1]) + x[n-1], each running at the branch's own rate
// (which is half the half-band's output rate -- that is what makes A(z^2)
// out of a first-order section).
static float allpass_branch(audioif_shaper_halfband_t *halfband,
    uint32_t branch, float value) {
    for (uint32_t section = 0; section < AUDIOIF_SHAPER_HB_SECTIONS;
         ++section) {
        const float a = audioif_shaper_hb_coefficients[branch][section];
        const float y = a * (value - halfband->y_prev[branch][section]) +
            halfband->x_prev[branch][section];
        halfband->x_prev[branch][section] = value;
        halfband->y_prev[branch][section] = y;
        value = y;
    }
    return value;
}

// Interpolate by two. Zero-stuffing and filtering would halve the amplitude;
// the polyphase form hands out each branch's own full-scale output instead,
// which is the factor of two the interpolation needs, for free.
static void halfband_up(audioif_shaper_halfband_t *halfband, float value,
    float *out) {
    out[0] = allpass_branch(halfband, 0, value);
    out[1] = allpass_branch(halfband, 1, value);
}

// Decimate by two. The odd branch's contribution to output n is its result
// from input pair n-1 -- the z^-1 in H(z) = (A0(z^2) + z^-1 A1(z^2))/2 -- so
// it is computed here and used on the next call.
static float halfband_down(audioif_shaper_halfband_t *halfband, float even,
    float odd) {
    const float value = 0.5f * (allpass_branch(halfband, 0, even) +
        halfband->hold);
    halfband->hold = allpass_branch(halfband, 1, odd);
    return value;
}

// The table, read with linear interpolation between neighbours. Input is
// clamped to -1..+1: the curve's ends are the circuit's rails, and a signal
// past them stays there rather than wrapping.
static float curve_lookup(const audioif_shaper_config_t *config, float value) {
    value = clampf(value, -1.0f, 1.0f);
    if (config->curve == NULL || config->curve_points < 2u) {
        return value;
    }
    const float last = (float)(config->curve_points - 1u);
    const float position = (value + 1.0f) * 0.5f * last;
    uint32_t index = (uint32_t)position;
    if (index >= config->curve_points - 1u) {
        index = config->curve_points - 2u;
    }
    const float fraction = position - (float)index;
    const float low = (float)config->curve[index] * (1.0f / 32768.0f);
    const float high = (float)config->curve[index + 1u] * (1.0f / 32768.0f);
    return low + fraction * (high - low);
}

// The play (backlash) operator, then the table. With `hysteresis` at zero the
// operator is skipped outright, and the node is a static table sample for
// sample -- which is what makes the option additive: a rendering with it off
// is the same bytes as one from a build that never had it.
static float shape_sample(const audioif_shaper_config_t *config,
    audioif_shaper_state_t *state, uint32_t channel, float value) {
    if (config->hysteresis > 0.0f) {
        float position = state->play[channel];
        if (value > position + config->width_up) {
            position = value - config->width_up;
        } else if (value < position - config->width_down) {
            position = value + config->width_down;
        }
        state->play[channel] = position;
        value = position;
    }
    return curve_lookup(config, value);
}

void audioif_shaper_process_s16(const audioif_shaper_config_t *config,
    audioif_shaper_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames) {
    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    const uint32_t stages = config->stages;
    const float wet_gain = config->mix;
    const float dry_gain = 1.0f - config->mix;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const float source = (float)in[frame * channels + channel];
            float oversampled[AUDIOIF_SHAPER_MAX_OVERSAMPLE];
            float expanded[AUDIOIF_SHAPER_MAX_OVERSAMPLE];
            // pre_gain and bias are linear, so they cost less applied once
            // here than once per oversampled sample, and mean exactly the
            // same thing: the interpolator passes DC at unity.
            oversampled[0] = config->pre_gain * source * (1.0f / 32768.0f) +
                config->bias;
            uint32_t count = 1u;
            for (uint32_t stage = 0; stage < stages; ++stage) {
                for (uint32_t i = 0; i < count; ++i) {
                    halfband_up(&state->up[channel][stage], oversampled[i],
                        &expanded[i * 2u]);
                }
                count *= 2u;
                for (uint32_t i = 0; i < count; ++i) {
                    oversampled[i] = expanded[i];
                }
            }
            for (uint32_t i = 0; i < count; ++i) {
                oversampled[i] = shape_sample(config, state, channel,
                    oversampled[i]);
            }
            for (uint32_t stage = stages; stage-- > 0;) {
                count /= 2u;
                for (uint32_t i = 0; i < count; ++i) {
                    oversampled[i] = halfband_down(
                        &state->down[channel][stage], oversampled[i * 2u],
                        oversampled[i * 2u + 1u]);
                }
            }
            const float wet = oversampled[0] * config->post_gain * 32768.0f;
            out[frame * channels + channel] =
                to_s16(dry_gain * source + wet_gain * wet);
        }
    }
}
