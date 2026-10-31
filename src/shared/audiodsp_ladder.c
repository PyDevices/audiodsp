// Runtime-neutral transistor ladder filter. See audiodsp_ladder.h for what the
// palette could not reach and why.
//
// `float` working precision, deliberately, to match audiodsp_dynamics.c and
// audiodsp_feedback_delay.c: this runs per sample on parts without an FPU at
// all, and doubles would cost more than the last few bits are worth.
//
// Two decisions are worth the space, because they are what make the filter a
// ladder rather than a resonant low-pass that looks like one.
//
// **The feedback has no delay in it.** Each stage is a topology-preserving
// (bilinear) one-pole, so four of them reach exactly -180 degrees at exactly
// the cutoff, with a gain of exactly 1/4 -- and the loop therefore sustains
// at a feedback of exactly 4, at exactly the cutoff, and not before. Put one
// sample of delay in the feedback path, which is the obvious way to write
// this, and none of those three sentences survives: the extra phase moves the
// 180-degree point below the cutoff, where each stage is louder, so the loop
// sustains early. Measured on the first draft of this file at 1 kHz and
// 48 kHz, it sustained at a feedback of 3.5, six percent below the cutoff --
// and worse the higher the cutoff goes, since the delay's phase is a fixed
// fraction of the sample rate while the filter's is not. "Self-oscillates at
// 4, at the cutoff" is the whole reason this node exists, so the loop is
// solved instead of delayed.
//
// **Solving it costs no division and no branch on the signal.** The loop
// equation is y + a*sat(y) = c, with a = resonance * g^4 and c everything
// the chain would output with the feedback disconnected. Where |y| >= 1 the
// saturator is constant and the equation is linear, so that branch is exact
// and closed-form. Inside, sat(y) = y - y^3/3 exactly, and the equation
// rearranges to y = (c + a*y^3/3)/(1 + a) -- a map whose derivative is
// a*y^2/(1+a), which is below 1 for every a and every |y| <= 1. So a fixed
// number of passes converges from any seed, with two precomputed constants
// and three multiplies a pass. Seeded by extrapolating the last two solved
// samples, four passes leave the residue far below the noise floor of the
// int16 the result is written back into.
//
// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"
#include "shared/audiodsp_hot.h"

#include "shared/audiodsp_ladder.h"
#include "shared/audiodsp_exp2.h"

#include <math.h>
#include <stddef.h>

#define AUDIODSP_LADDER_PI 3.14159265358979323846f

//: The feedback at which the loop sustains. Not a tunable: it falls out of
//: four one-poles each contributing 45 degrees and 1/sqrt(2) at the cutoff.
#define AUDIODSP_LADDER_SELF_OSCILLATION 4.0f

static float AUDIODSP_GRAPH_HOT clampf(float value, float low, float high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

// The odd cubic soft-clip, the same curve audiodsp_feedback_delay.c puts in
// its loop, in the +-1 domain rather than the int16 one. Odd, so it makes
// third-harmonic thickening and no second; unity slope at the origin, so it
// does not move the feedback at which the loop sustains; two multiplies,
// where a tanh would be a library call per sample. Its value past the clamp
// is 1 - 1/3, which is what the linear branch of the solver is written
// against.
static float AUDIODSP_GRAPH_HOT ladder_saturate(float value) {
    const float clipped = clampf(value, -1.0f, 1.0f);
    return clipped - clipped * clipped * clipped * (1.0f / 3.0f);
}

// Everything that follows from g, for the configured cutoff and for the
// cutoff input alike, so the two run the same arithmetic.
static void AUDIODSP_GRAPH_HOT ladder_finish_coeffs(audiodsp_ladder_coeffs_t *coeffs,
    float resonance, float g) {
    coeffs->g = g;
    const float squared = g * g;
    coeffs->g4 = squared * squared;
    coeffs->loop_gain = resonance * coeffs->g4;
    coeffs->inv_one_plus_loop = 1.0f / (1.0f + coeffs->loop_gain);
    coeffs->loop_over_three =
        coeffs->loop_gain * coeffs->inv_one_plus_loop * (1.0f / 3.0f);
}

// Everything the per-sample loop reads, from everything the caller set.
// One tanf() and one division, per set() rather than per sample.
static void ladder_update_coefficients(audiodsp_ladder_config_t *config) {
    const float rate =
        (float)config->sample_rate * (float)config->oversample;
    // Below the top of the band tan() is still finite and g still short of 1.
    // A cutoff above the OUTPUT Nyquist is meaningful when oversampling: it
    // is what "wide open" means.
    const float cutoff = clampf(config->cutoff_hz, 1.0f, rate * 0.49f);
    const float warped = tanf(AUDIODSP_LADDER_PI * cutoff / rate);
    ladder_finish_coeffs(&config->coeffs, config->resonance,
        warped / (1.0f + warped));
    config->output_gain = 1.0f + config->passband_comp * config->resonance;
    config->cutoff_scale = config->cutoff_octaves * (1.0f / 32768.0f);
    config->pi_over_rate = AUDIODSP_LADDER_PI / rate;
    config->cutoff_max = rate * 0.49f;
    ++config->revision;
}

// sin(w) and cos(w) for w in (0, 0.49 pi], as series in w: the cutoff input
// derives g every sample, and libm's tanf is neither the same bits on every
// target nor cheap on a part without one in hardware. Seven terms each, which
// leaves the error under 1e-9 at the top of the range and relative, not
// absolute, at the bottom, where g is small and a cancellation would cost it
// its digits.
static float AUDIODSP_GRAPH_HOT ladder_sin(float w) {
    const float w2 = w * w;
    return w * (1.0f + w2 * (-1.0f / 6.0f + w2 * (1.0f / 120.0f
        + w2 * (-1.0f / 5040.0f + w2 * (1.0f / 362880.0f
        + w2 * (-1.0f / 39916800.0f + w2 * (1.0f / 6227020800.0f)))))));
}

static float AUDIODSP_GRAPH_HOT ladder_cos(float w) {
    const float w2 = w * w;
    return 1.0f + w2 * (-0.5f + w2 * (1.0f / 24.0f + w2 * (-1.0f / 720.0f
        + w2 * (1.0f / 40320.0f + w2 * (-1.0f / 3628800.0f
        + w2 * (1.0f / 479001600.0f + w2 * (-1.0f / 87178291200.0f)))))));
}

// The coefficients for one sample of the cutoff input. tan/(1 + tan) is
// sin/(sin + cos), which is one division and no tan.
static void AUDIODSP_GRAPH_HOT ladder_cutoff_coeffs(const audiodsp_ladder_config_t *config,
    int32_t value, audiodsp_ladder_coeffs_t *coeffs) {
    const float cutoff = clampf(config->cutoff_hz *
        audiodsp_exp2_poly((float)value * config->cutoff_scale),
        1.0f, config->cutoff_max);
    const float w = cutoff * config->pi_over_rate;
    const float sine = ladder_sin(w);
    ladder_finish_coeffs(coeffs, config->resonance,
        sine / (sine + ladder_cos(w)));
}

void audiodsp_ladder_config_init(audiodsp_ladder_config_t *config,
    uint32_t sample_rate) {
    config->sample_rate = sample_rate ? sample_rate : 48000;
    config->channel_count = 2;
    config->cutoff_hz = 1000.0f;
    config->resonance = 0.0f;
    config->drive = 1.0f;
    config->passband_comp = 0.0f;
    config->mix = 1.0f;
    // Ten octaves for a full-scale input, the same as audiooscillator's
    // pitch input, so one wire moves an oscillator and a filter alike.
    config->cutoff_octaves = 10.0f;
    config->revision = 0;
    config->poles = (uint8_t)AUDIODSP_LADDER_STAGES;
    // 2x by default. The filter is nonlinear, so it makes harmonics of
    // whatever it is given, and the ones above the band fold back. Half the
    // cost is the wrong saving on the one node whose job is to distort.
    config->oversample = 2;
    ladder_update_coefficients(config);
}

void audiodsp_ladder_set_channel_count(audiodsp_ladder_config_t *config,
    uint32_t channel_count) {
    config->channel_count = channel_count == 1u ? 1u : 2u;
}

void audiodsp_ladder_configure(audiodsp_ladder_config_t *config,
    audiodsp_ladder_option_t option, float value) {
    switch (option) {
        case AUDIODSP_LADDER_OPT_CUTOFF_HZ:
            // Stored as asked for, clamped where it is used: `oversample`
            // moves the rate this is measured against, so a cutoff clamped
            // here would be silently wrong after the next set().
            config->cutoff_hz = value;
            break;
        case AUDIODSP_LADDER_OPT_RESONANCE:
            // Past 4 the loop sustains; a little past it is where a ladder
            // is usually played, so the ceiling is above the threshold
            // rather than at it. It is not open-ended: the saturator bounds
            // the tone at any feedback, but the louder the loop runs the
            // more of what comes out is the filter's own voice and the less
            // is the signal.
            config->resonance =
                clampf(value, 0.0f, AUDIODSP_LADDER_SELF_OSCILLATION + 0.2f);
            break;
        case AUDIODSP_LADDER_OPT_DRIVE:
            // A linear gain into the loop, 1 meaning unity, so a caller
            // holding decibels converts once in Python rather than having
            // this file guess which convention it meant.
            config->drive = clampf(value, 0.0f, 64.0f);
            break;
        case AUDIODSP_LADDER_OPT_POLES: {
            // Which stage the output is tapped from. The feedback is always
            // the fourth, so the resonance and the self-oscillation are the
            // same filter's at every tap -- only the slope above the cutoff
            // changes, at 6 dB an octave a stage.
            const int poles = (int)(value + 0.5f);
            config->poles = (uint8_t)(poles < 1 ? 1 :
                (poles > (int)AUDIODSP_LADDER_STAGES ?
                    (int)AUDIODSP_LADDER_STAGES : poles));
            break;
        }
        case AUDIODSP_LADDER_OPT_PASSBAND_COMP:
            // The feedback subtracts from the passband as well as peaking
            // the cutoff: at a feedback of 4 the whole band below is 14 dB
            // down. 1 gives all of it back, 0 leaves the filter as the
            // circuit behaves. Default 0, so this option changes nothing
            // until it is asked for.
            config->passband_comp = clampf(value, 0.0f, 1.0f);
            break;
        case AUDIODSP_LADDER_OPT_OVERSAMPLE:
            config->oversample = value >= 1.5f ? 2u : 1u;
            break;
        case AUDIODSP_LADDER_OPT_MIX:
            // A plain crossfade, 0 dry and 1 filtered -- deliberately NOT
            // audioecho's and audiodelays.Echo's 0..2 convention, where the
            // dry stays at unity until 1. That convention exists because a
            // delay's wet signal is something added to a signal that is
            // still there; a filter's wet signal is that signal, changed,
            // and "the dry plus all of the filtered" is not a setting any
            // filter has. 0 is a wire, sample for sample.
            config->mix = clampf(value, 0.0f, 1.0f);
            break;
        case AUDIODSP_LADDER_OPT_CUTOFF_OCTAVES:
            // Any sign: a negative depth closes the filter as the input
            // rises. However large, the cutoff it reaches is clamped to the
            // same band `cutoff_hz` is.
            config->cutoff_octaves = value;
            break;
    }
    ladder_update_coefficients(config);
}

void audiodsp_ladder_config_finish(audiodsp_ladder_config_t *config) {
    ladder_update_coefficients(config);
}

void audiodsp_ladder_state_init(audiodsp_ladder_state_t *state) {
    for (uint32_t channel = 0; channel < 2u; ++channel) {
        for (uint32_t stage = 0; stage < AUDIODSP_LADDER_STAGES; ++stage) {
            state->integrator[channel][stage] = 0.0f;
        }
        state->last[channel] = 0.0f;
        state->previous_last[channel] = 0.0f;
        state->previous_input[channel] = 0.0f;
    }
    // No int16 is INT32_MIN, so the first cutoff sample always derives its
    // coefficients.
    state->cutoff_last = INT32_MIN;
    state->cutoff_revision = 0;
}

void audiodsp_ladder_reset(audiodsp_ladder_state_t *state) {
    audiodsp_ladder_state_init(state);
}

// Solve y + a*sat(y) = c for y. See the file header for why this is a solve
// and not a delay, and why it converges.
static float AUDIODSP_GRAPH_HOT ladder_solve(const audiodsp_ladder_coeffs_t *coeffs, float c,
    float seed) {
    const float saturated = coeffs->loop_gain * (2.0f / 3.0f);
    // Where the saturator is clamped the equation is linear, so these two
    // tests are exact, not approximations: y >= 1 exactly when
    // c >= 1 + a*sat(1).
    float y = c - saturated;
    if (y >= 1.0f) {
        return y;
    }
    y = c + saturated;
    if (y <= -1.0f) {
        return y;
    }
    const float base = c * coeffs->inv_one_plus_loop;
    y = seed;
    for (int step = 0; step < AUDIODSP_LADDER_SOLVE_STEPS; ++step) {
        y = clampf(y, -1.0f, 1.0f);
        y = base + coeffs->loop_over_three * (y * y * y);
    }
    // The solution is inside the clamp by construction -- the two branches
    // above took every case where it is not.
    return clampf(y, -1.0f, 1.0f);
}

// One sample through the ladder, at whatever rate the caller is running it.
// `x` is already driven and in the +-1 domain; the return is the tapped
// stage, before the passband compensation and the mix.
static float AUDIODSP_GRAPH_HOT ladder_step(const audiodsp_ladder_config_t *config,
    const audiodsp_ladder_coeffs_t *coeffs, audiodsp_ladder_state_t *state,
    uint32_t channel, float x) {
    float *integrator = state->integrator[channel];
    const float g = coeffs->g;
    const float one_minus_g = 1.0f - g;

    // What each stage contributes to its own output with no input: the state
    // is kept unscaled, so this is recomputed rather than stored. It costs
    // one multiply a stage and it is what lets `cutoff_hz` move mid-block
    // without the state meaning something else afterwards.
    float held[AUDIODSP_LADDER_STAGES];
    for (uint32_t stage = 0; stage < AUDIODSP_LADDER_STAGES; ++stage) {
        held[stage] = one_minus_g * integrator[stage];
    }

    // c is what the fourth stage would put out this sample with the feedback
    // disconnected: the driven input through four stages of gain, plus what
    // the four states are already holding.
    const float carried =
        ((held[0] * g + held[1]) * g + held[2]) * g + held[3];
    const float c = coeffs->g4 * x + carried;

    // Extrapolating the last two solved samples costs one multiply and one
    // subtract and removes most of the solver's work: at audio rates the
    // fourth stage's output is a smooth curve, and a straight line through
    // its last two points lands very close to the next.
    const float seed =
        2.0f * state->last[channel] - state->previous_last[channel];
    const float solved = ladder_solve(coeffs, c, seed);
    state->previous_last[channel] = state->last[channel];
    state->last[channel] = solved;

    // The ladder's input node. Computed from the solved output through the
    // saturator, not by rearranging the loop equation for it: at a low
    // cutoff g^4 is 1e-8 and the rearrangement is a subtraction of two
    // numbers of order 1 to get one of order 1e-8, which single precision
    // does not have the digits for.
    float value = x - config->resonance * ladder_saturate(solved);

    float outputs[AUDIODSP_LADDER_STAGES];
    for (uint32_t stage = 0; stage < AUDIODSP_LADDER_STAGES; ++stage) {
        const float output = g * value + held[stage];
        // z <- 2y - z, the trapezoidal integrator's update written without a
        // second multiply.
        integrator[stage] = output + output - integrator[stage];
        outputs[stage] = output;
        value = output;
    }
    return outputs[config->poles - 1u];
}

static int16_t AUDIODSP_GRAPH_HOT to_s16(float value) {
    if (value > 32767.0f) {
        return 32767;
    }
    if (value < -32768.0f) {
        return -32768;
    }
    return (int16_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

// One frame, every channel, at the coefficients given. Inline, so the
// unmodulated loop below compiles to what it was before the cutoff input.
static inline void AUDIODSP_GRAPH_HOT ladder_frame(const audiodsp_ladder_config_t *config,
    const audiodsp_ladder_coeffs_t *coeffs, audiodsp_ladder_state_t *state,
    int16_t *out, const int16_t *in, uint32_t channels, bool doubled,
    float drive, float mix, float gain) {
    for (uint32_t channel = 0; channel < channels; ++channel) {
        const float dry = (float)in[channel] * (1.0f / 32768.0f);
        float wet;
        if (doubled) {
            // Linear interpolation up, a two-tap average down: half a
            // sample of group delay for the pair, which is why the node
            // reports no latency. A nonlinearity makes harmonics of what
            // it is given and the ones above the band fold back, so the
            // point is to make them above a band twice as wide and then
            // throw that half away.
            const float middle =
                0.5f * (state->previous_input[channel] + dry);
            const float first =
                ladder_step(config, coeffs, state, channel, middle * drive);
            const float second =
                ladder_step(config, coeffs, state, channel, dry * drive);
            wet = 0.5f * (first + second);
        } else {
            wet = ladder_step(config, coeffs, state, channel, dry * drive);
        }
        // Kept whether or not it was used, so turning oversampling on
        // mid-stream does not start from a stale sample.
        state->previous_input[channel] = dry;
        wet *= gain;
        out[channel] = to_s16((dry + (wet - dry) * mix) * 32768.0f);
    }
}

void AUDIODSP_GRAPH_HOT audiodsp_ladder_process_s16(const audiodsp_ladder_config_t *config,
    audiodsp_ladder_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames) {
    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    const bool doubled = config->oversample >= 2u;
    const float drive = config->drive;
    const float mix = config->mix;
    const float gain = config->output_gain;
    const audiodsp_ladder_coeffs_t *coeffs = &config->coeffs;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        ladder_frame(config, coeffs, state, &out[frame * channels],
            &in[frame * channels], channels, doubled, drive, mix, gain);
    }
}

void AUDIODSP_GRAPH_HOT audiodsp_ladder_process_cutoff_s16(const audiodsp_ladder_config_t *config,
    audiodsp_ladder_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames, const int16_t *cutoff, uint32_t cutoff_stride) {
    if (cutoff == NULL) {
        audiodsp_ladder_process_s16(config, state, out, in, frames);
        return;
    }
    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    const bool doubled = config->oversample >= 2u;
    const float drive = config->drive;
    const float mix = config->mix;
    const float gain = config->output_gain;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        const int32_t value = cutoff[frame * cutoff_stride];
        const audiodsp_ladder_coeffs_t *coeffs = &config->coeffs;
        // Zero is the filter as configured, with the configured set's own
        // bits, so a silent input renders exactly what no input does.
        if (value != 0) {
            if (value != state->cutoff_last ||
                config->revision != state->cutoff_revision) {
                state->cutoff_last = value;
                state->cutoff_revision = config->revision;
                ladder_cutoff_coeffs(config, value, &state->cutoff_coeffs);
            }
            coeffs = &state->cutoff_coeffs;
        }
        ladder_frame(config, coeffs, state, &out[frame * channels],
            &in[frame * channels], channels, doubled, drive, mix, gain);
    }
}
