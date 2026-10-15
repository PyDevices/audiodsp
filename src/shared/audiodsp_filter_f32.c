// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_filter_f32.h"

#include "shared/audiodsp_trig.h"

#include <math.h>
#include <string.h>

// ln(10)/40 -- the RBJ shelf/peak amplitude, A = 10^(dB/40). Written as a
// multiply into expf() the way audiodsp_dynamics.c:17 writes its own dB
// conversion, so the two agree about what a decibel is.
#define AUDIODSP_FILTER_F32_DB_TO_A 0.05756462732485115f

// Q has to stay off the unit circle or the tail this module exists to end
// never ends: the pole radius of an RBJ section is sqrt((1 - alpha) /
// (1 + alpha)) with alpha = sin(W0) / (2Q), so alpha -> 0 is a filter that
// rings for ever. At Q = 60 and 1 kHz / 48 kHz the state decays past the
// flush threshold in about 51,000 samples (roughly a second), which is a real
// bound; at Q = 600 it would be ten seconds and at Q = 6000 a hundred.
#define AUDIODSP_FILTER_F32_MIN_Q 0.05f
#define AUDIODSP_FILTER_F32_MAX_Q 60.0f

// Same argument one order down. c = (1 - t)/(1 + t) reaches exactly 1 at
// frequency 0, and a first-order all-pass with c = 1 holds its state for
// ever. Capped just below, so the worst case decays past the flush threshold
// in about 550,000 samples rather than never.
#define AUDIODSP_FILTER_F32_MAX_ALLPASS_C 0.9999f

static float clampf(float value, float low, float high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

// Round half away from zero, then clamp -- `audiodsp_feedback_delay.c`'s
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
    if (value > -AUDIODSP_FILTER_F32_FLUSH &&
        value < AUDIODSP_FILTER_F32_FLUSH) {
        return 0.0f;
    }
    return value;
}

// ---------------------------------------------------------------- biquad --

void audiodsp_biquad_f32_config_init(audiodsp_biquad_f32_config_t *config,
    uint32_t sample_rate, uint32_t channel_count) {
    memset(config, 0, sizeof(*config));
    config->sample_rate = sample_rate < 1u ? 1u : sample_rate;
    config->channel_count = channel_count == 1u ? 1u : 2u;
    config->mode = AUDIODSP_BIQUAD_F32_LOW_PASS;
    config->frequency = 1000.0f;
    config->q = 0.7071067811865475f;
    config->gain_db = 0.0f;
    config->mix = 1.0f;
    config->derived = false;
}

void audiodsp_biquad_f32_configure(audiodsp_biquad_f32_config_t *config,
    audiodsp_biquad_f32_option_t option, float value) {
    switch (option) {
        case AUDIODSP_BIQUAD_F32_OPT_MODE: {
            int32_t mode = (int32_t)value;
            if (mode < AUDIODSP_BIQUAD_F32_LOW_PASS) {
                mode = AUDIODSP_BIQUAD_F32_LOW_PASS;
            }
            if (mode > AUDIODSP_BIQUAD_F32_HIGH_SHELF) {
                mode = AUDIODSP_BIQUAD_F32_HIGH_SHELF;
            }
            config->mode = mode;
            break;
        }
        case AUDIODSP_BIQUAD_F32_OPT_FREQUENCY:
            // Nyquist itself is not allowed: W0 = pi puts sin(W0) at zero and
            // the section degenerates.
            config->frequency = clampf(value, 0.0f,
                (float)config->sample_rate * 0.4999f);
            break;
        case AUDIODSP_BIQUAD_F32_OPT_Q:
            config->q = clampf(value, AUDIODSP_FILTER_F32_MIN_Q,
                AUDIODSP_FILTER_F32_MAX_Q);
            break;
        case AUDIODSP_BIQUAD_F32_OPT_GAIN_DB:
            config->gain_db = clampf(value, -60.0f, 60.0f);
            break;
        case AUDIODSP_BIQUAD_F32_OPT_MIX:
            config->mix = clampf(value, 0.0f, 1.0f);
            break;
        default:
            break;
    }
}

// The section as a trapezoidal state-variable filter, from its normalized
// five. Every RBJ shape is the bilinear transform of a second-order analog
// prototype, and so is the SVF, so one exists for every mode and setting with
// a corner above 0 Hz -- found here from the coefficients alone, with nothing
// mode-specific to keep in step with the cookbook above.
//
// With D0 = 1 + g*k + g^2, the SVF's denominator is
// [D0, 2g^2 - 2, 1 - g*k + g^2] / D0, its band numerator g*[1, 0, -1] / D0 and
// its low numerator g^2*[1, 2, 1] / D0. Matching the first to [1, a1, a2]
// gives g and k; writing [b0, b1, b2] * D0 as m0*denominator + m1*band +
// m2*low gives the three mixes. In double, from the doubles the five were
// rounded from: 1 + a1 + a2 is a small difference of large numbers at a low
// corner, and float would lose g to it.
//
// Below AUDIODSP_FILTER_F32_MIN_SVF_G2 the section is not treated as having
// one. That is 0 Hz, where alpha = 0 puts a double pole on the unit circle
// with the zeros on top of it, and nothing above about 0.02 Hz at 48 kHz.
#define AUDIODSP_FILTER_F32_MIN_SVF_G2 1e-12

static void biquad_svf_from_rbj(audiodsp_biquad_f32_config_t *config,
    double b0, double b1, double b2, double a1, double a2) {
    const double denominator = 1 - a1 + a2;
    config->svf_valid = false;
    if (!(denominator > 0)) {
        return;
    }
    const double g2 = (1 + a1 + a2) / denominator;
    if (!(g2 > AUDIODSP_FILTER_F32_MIN_SVF_G2)) {
        return;
    }
    const double g = sqrt(g2);
    const double gk = 2 * (1 - a2) / denominator;
    if (!(gk > 0)) {
        return;
    }
    const double d0 = 4 / denominator;
    const double p0 = b0 * d0, p1 = b1 * d0, p2 = b2 * d0;
    const double m0 = (p0 - p1 + p2) / 4;
    config->svf_g = (float)g;
    config->svf_k = (float)(gk / g);
    config->svf_m0 = (float)m0;
    config->svf_m1 = (float)((p0 - p2 - 2 * m0 * gk) / (2 * g));
    config->svf_m2 = (float)((p0 + p2 - m0 * (2 + 2 * g2)) / (2 * g2));
    config->svf_valid = true;
}

void audiodsp_biquad_f32_config_finish(audiodsp_biquad_f32_config_t *config) {
    if (config->derived &&
        config->derived_mode == config->mode &&
        config->derived_sample_rate == config->sample_rate &&
        config->derived_frequency == config->frequency &&
        config->derived_q == config->q &&
        config->derived_gain_db == config->gain_db) {
        return;
    }

    // The cookbook algebra, copied rather than shared: `audiodsp_biquad.c` is
    // ported CircuitPython and frozen, and what it does after this point --
    // pick a fixed-point shift and quantize into it -- is the half this
    // module exists to avoid. What *is* shared is the trig
    // (audiodsp_trig.c:72), so at one W0 both kernels start from the same sine
    // and cosine and a comparison between them measures the arithmetic rather
    // than two different libm builds.
    const double W0 = (double)config->frequency * (2 * AUDIODSP_PI) /
        (double)config->sample_rate;
    const double Q = (double)config->q;
    const double A = (double)expf(config->gain_db *
        AUDIODSP_FILTER_F32_DB_TO_A);
    audiodsp_sincos_t sc;
    audiodsp_sincos_reflect(W0, &sc);
    const double alpha = sc.s / (2 * Q);
    double a0, a1, a2, b0, b1, b2;
    if (config->mode < AUDIODSP_BIQUAD_F32_PEAKING_EQ) {
        a0 = 1 + alpha;
        a1 = -2 * sc.c;
        a2 = 1 - alpha;
        if (config->mode == AUDIODSP_BIQUAD_F32_LOW_PASS) {
            b2 = b0 = (1 - sc.c) * .5;
            b1 = 1 - sc.c;
        } else if (config->mode == AUDIODSP_BIQUAD_F32_HIGH_PASS) {
            b2 = b0 = (1 + sc.c) * .5;
            b1 = -(1 + sc.c);
        } else if (config->mode == AUDIODSP_BIQUAD_F32_BAND_PASS) {
            b0 = alpha;
            b1 = 0;
            b2 = -b0;
        } else {
            b0 = 1;
            b1 = -2 * sc.c;
            b2 = 1;
        }
    } else if (config->mode == AUDIODSP_BIQUAD_F32_PEAKING_EQ) {
        // `1 - alpha * A`, the sign audiodsp_biquad.c:87 corrected upstream to
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
        // sqrt() rather than audiodsp_biquad.c:26's Newton-step reciprocal
        // approximation. That trick buys speed on a part with no divider and
        // costs about 0.2% of A; it is there because the result is about to be
        // quantized into 23 bits anyway. Nothing here is quantized, and IEEE
        // sqrt is correctly rounded, so this is both more accurate and the
        // same on every target.
        const double root = sqrt(A);
        if (config->mode == AUDIODSP_BIQUAD_F32_LOW_SHELF) {
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
    biquad_svf_from_rbj(config, b0 * reciprocal, b1 * reciprocal,
        b2 * reciprocal, a1 * reciprocal, a2 * reciprocal);

    config->derived_mode = config->mode;
    config->derived_sample_rate = config->sample_rate;
    config->derived_frequency = config->frequency;
    config->derived_q = config->q;
    config->derived_gain_db = config->gain_db;
    config->derived = true;
}

void audiodsp_biquad_f32_state_init(audiodsp_biquad_f32_state_t *state) {
    memset(state, 0, sizeof(*state));
}

void audiodsp_biquad_f32_reset(audiodsp_biquad_f32_state_t *state) {
    memset(state, 0, sizeof(*state));
}

// ------------------------------------------------- moving the coefficients --

// The SVF's per-frame constants at (g, k), Simper's a1/a2/a3.
typedef struct { double a1, a2, a3; } biquad_svf_gains_t;

static biquad_svf_gains_t biquad_svf_gains(double g, double k) {
    biquad_svf_gains_t gains;
    gains.a1 = 1 / (1 + g * (g + k));
    gains.a2 = g * gains.a1;
    gains.a3 = g * gains.a2;
    return gains;
}

// The two rows that say what an SVF state does to the output with no input
// coming in: the next output, and the one after. Transposed direct form II's
// two words are exactly those two outputs (less a1 times the first, in the
// second), which is what lets the memory pass between the two forms.
static void biquad_svf_observe(double g, double k, double m1, double m2,
    double rows[2][2]) {
    const biquad_svf_gains_t s = biquad_svf_gains(g, k);
    // Output from the integrators: m1*v1 + m2*v2 with the input at zero.
    const double c0 = m1 * s.a1 + m2 * s.a2;
    const double c1 = -m1 * s.a2 + m2 * (1 - s.a3);
    // One frame of the integrators with the input at zero.
    const double t00 = 2 * s.a1 - 1, t01 = -2 * s.a2;
    const double t10 = 2 * s.a2, t11 = 1 - 2 * s.a3;
    rows[0][0] = c0;
    rows[0][1] = c1;
    rows[1][0] = c0 * t00 + c1 * t10;
    rows[1][1] = c0 * t01 + c1 * t11;
}

// Direct form's memory, as the SVF of the section it belongs to. Where the
// output cannot see the integrators -- a 0 dB peak or shelf, whose response
// is a wire -- there is nothing to carry and they start empty.
static void biquad_enter_svf(audiodsp_biquad_f32_state_t *state,
    uint32_t channels) {
    double rows[2][2];
    biquad_svf_observe(state->svf_g, state->svf_k, state->svf_m1,
        state->svf_m2, rows);
    const double det = rows[0][0] * rows[1][1] - rows[0][1] * rows[1][0];
    const double scale = (rows[0][0] * rows[0][0] + rows[0][1] * rows[0][1]) *
        (rows[1][0] * rows[1][0] + rows[1][1] * rows[1][1]);
    const bool observable = det != 0 && det * det > 1e-18 * scale;
    for (uint32_t channel = 0; channel < channels; ++channel) {
        if (!observable) {
            state->ic1[channel] = state->ic2[channel] = 0.0f;
            continue;
        }
        const double y0 = state->s1[channel];
        const double y1 = (double)state->s2[channel] -
            (double)state->a1 * y0;
        state->ic1[channel] = flush((float)((rows[1][1] * y0 -
            rows[0][1] * y1) / det));
        state->ic2[channel] = flush((float)((rows[0][0] * y1 -
            rows[1][0] * y0) / det));
    }
}

// The way back, at the end of a move, into the section now in force.
static void biquad_leave_svf(audiodsp_biquad_f32_state_t *state,
    uint32_t channels) {
    double rows[2][2];
    biquad_svf_observe(state->svf_g, state->svf_k, state->svf_m1,
        state->svf_m2, rows);
    for (uint32_t channel = 0; channel < channels; ++channel) {
        const double i1 = state->ic1[channel], i2 = state->ic2[channel];
        const double y0 = rows[0][0] * i1 + rows[0][1] * i2;
        const double y1 = rows[1][0] * i1 + rows[1][1] * i2;
        state->s1[channel] = flush((float)y0);
        state->s2[channel] = flush((float)(y1 + (double)state->a1 * y0));
    }
}

// Frames in a move: the power of two nearest AUDIODSP_BIQUAD_F32_RAMP_MS.
// A power of two because the walk's per-frame ratio is then a chain of
// square roots, which IEEE rounds correctly everywhere -- pow() is three
// functions on three targets that nearly agree, and this has to render the
// same bytes on all of them (see audiodsp_trig.h).
static uint32_t biquad_ramp_shift(uint32_t sample_rate) {
    const double frames = (double)sample_rate *
        (double)AUDIODSP_BIQUAD_F32_RAMP_MS / 1000.0;
    uint32_t shift = 0;
    while (shift < 20u &&
           (double)(1u << shift) * 1.4142135623730951 < frames) {
        ++shift;
    }
    return shift;
}

static double biquad_root_chain(double ratio, uint32_t shift) {
    for (uint32_t index = 0; index < shift; ++index) {
        ratio = sqrt(ratio);
    }
    return ratio;
}

static void biquad_take(audiodsp_biquad_f32_state_t *state,
    const audiodsp_biquad_f32_config_t *config) {
    state->b0 = config->b0;
    state->b1 = config->b1;
    state->b2 = config->b2;
    state->a1 = config->a1;
    state->a2 = config->a2;
    state->svf_g = config->svf_g;
    state->svf_k = config->svf_k;
    state->svf_m0 = config->svf_m0;
    state->svf_m1 = config->svf_m1;
    state->svf_m2 = config->svf_m2;
    state->svf_valid = config->svf_valid;
}

static bool biquad_moved(const audiodsp_biquad_f32_state_t *state,
    const audiodsp_biquad_f32_config_t *config) {
    return state->b0 != config->b0 || state->b1 != config->b1 ||
           state->b2 != config->b2 || state->a1 != config->a1 ||
           state->a2 != config->a2;
}

// A new set of five arrived. Start the walk to them from wherever the
// output is now: the section in force, or the point a walk already under way
// has reached.
static void biquad_begin_move(const audiodsp_biquad_f32_config_t *config,
    audiodsp_biquad_f32_state_t *state, uint32_t channels) {
    if (state->ramp_left == 0u) {
        if (state->svf_valid && config->svf_valid) {
            biquad_enter_svf(state, channels);
            state->g = state->svf_g;
            state->k = state->svf_k;
            state->m0 = state->svf_m0;
            state->m1 = state->svf_m1;
            state->m2 = state->svf_m2;
        }
    }
    const bool walk = config->svf_valid &&
        (state->ramp_left != 0u || state->svf_valid);
    biquad_take(state, config);
    if (!walk) {
        // To or from 0 Hz. The section there is a wire or silence whatever
        // its memory holds -- and the memory cannot decay, its poles being on
        // the unit circle -- so there is nothing to carry across, and carrying
        // it is how a jump to 0 Hz used to run away.
        memset(state->s1, 0, sizeof(state->s1));
        memset(state->s2, 0, sizeof(state->s2));
        state->ramp_left = 0u;
        return;
    }
    const uint32_t shift = biquad_ramp_shift(config->sample_rate);
    const uint32_t frames = 1u << shift;
    state->g_step = (float)biquad_root_chain(
        (double)config->svf_g / (double)state->g, shift);
    state->k_step = (float)biquad_root_chain(
        (double)config->svf_k / (double)state->k, shift);
    state->m0_step = (config->svf_m0 - state->m0) / (float)frames;
    state->m1_step = (config->svf_m1 - state->m1) / (float)frames;
    state->m2_step = (config->svf_m2 - state->m2) / (float)frames;
    state->ramp_left = frames;
}

// Up to `frames` frames of the walk, one SVF step each. Returns how many it
// ran; when the walk ends inside them, the memory goes back to direct form.
static uint32_t biquad_process_move(audiodsp_biquad_f32_state_t *state,
    uint32_t channels, float dry, float mix, int16_t *out, const int16_t *in,
    uint32_t frames) {
    const uint32_t run = frames < state->ramp_left ? frames : state->ramp_left;
    float g = state->g, k = state->k;
    float m0 = state->m0, m1 = state->m1, m2 = state->m2;
    for (uint32_t frame = 0; frame < run; ++frame) {
        g *= state->g_step;
        k *= state->k_step;
        m0 += state->m0_step;
        m1 += state->m1_step;
        m2 += state->m2_step;
        const float s1 = 1.0f / (1.0f + g * (g + k));
        const float s2 = g * s1;
        const float s3 = g * s2;
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const size_t index = (size_t)frame * channels + channel;
            const float x0 = (float)in[index];
            const float ic1 = state->ic1[channel], ic2 = state->ic2[channel];
            const float v3 = x0 - ic2;
            const float v1 = s1 * ic1 + s2 * v3;
            const float v2 = ic2 + s2 * ic1 + s3 * v3;
            state->ic1[channel] = flush(2.0f * v1 - ic1);
            state->ic2[channel] = flush(2.0f * v2 - ic2);
            const float y0 = m0 * x0 + m1 * v1 + m2 * v2;
            out[index] = to_s16(dry * x0 + mix * y0);
        }
    }
    state->g = g;
    state->k = k;
    state->m0 = m0;
    state->m1 = m1;
    state->m2 = m2;
    state->ramp_left -= run;
    if (state->ramp_left == 0u) {
        biquad_leave_svf(state, channels);
    }
    return run;
}

void audiodsp_biquad_f32_process_s16(const audiodsp_biquad_f32_config_t *config,
    audiodsp_biquad_f32_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames) {
    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    const float mix = config->mix;
    const float dry = 1.0f - mix;
    if (!state->primed) {
        biquad_take(state, config);
        state->primed = true;
    } else if (biquad_moved(state, config)) {
        biquad_begin_move(config, state, channels);
    }
    if (state->ramp_left != 0u) {
        const uint32_t done = biquad_process_move(state, channels, dry, mix,
            out, in, frames);
        out += (size_t)done * channels;
        in += (size_t)done * channels;
        frames -= done;
    }
    const float b0 = config->b0, b1 = config->b1, b2 = config->b2;
    const float a1 = config->a1, a2 = config->a2;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const size_t index = (size_t)frame * channels + channel;
            const float x0 = (float)in[index];
            // Transposed direct form II -- see the state struct for why this
            // shape and not direct form I (audiodsp#64).
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

void audiodsp_allpass_f32_config_init(audiodsp_allpass_f32_config_t *config,
    uint32_t sample_rate, uint32_t channel_count, uint32_t stages) {
    memset(config, 0, sizeof(*config));
    config->sample_rate = sample_rate < 1u ? 1u : sample_rate;
    config->channel_count = channel_count == 1u ? 1u : 2u;
    if (stages < 1u) {
        stages = 1u;
    }
    if (stages > AUDIODSP_FILTER_F32_MAX_STAGES) {
        stages = AUDIODSP_FILTER_F32_MAX_STAGES;
    }
    config->stages = stages;
    config->frequency = 1000.0f;
    config->feedback = 0.0f;
    config->mix = 1.0f;
    config->derived = false;
}

void audiodsp_allpass_f32_configure(audiodsp_allpass_f32_config_t *config,
    audiodsp_allpass_f32_option_t option, float value) {
    switch (option) {
        case AUDIODSP_ALLPASS_F32_OPT_FREQUENCY:
            config->frequency = clampf(value, 0.0f,
                (float)config->sample_rate * 0.4999f);
            break;
        case AUDIODSP_ALLPASS_F32_OPT_FEEDBACK:
            config->feedback = clampf(value, -0.99f, 0.99f);
            break;
        case AUDIODSP_ALLPASS_F32_OPT_MIX:
            config->mix = clampf(value, 0.0f, 1.0f);
            break;
        default:
            break;
    }
}

void audiodsp_allpass_f32_config_finish(audiodsp_allpass_f32_config_t *config) {
    if (config->derived &&
        config->derived_sample_rate == config->sample_rate &&
        config->derived_frequency == config->frequency) {
        return;
    }
    // The bilinear transform, done properly. With t = tan(pi f / fs), a
    // stage's phase is -2*atan(((1 - c)/(1 + c)) * tan(w/2)), so c = (t - 1)
    // / (t + 1) puts its -90 degree point exactly at f.
    //
    // `audiodsp_phaser.c` reaches the same convention by a different route and
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
    // for the reason audiodsp_trig.h gives: one polynomial evaluated in one
    // order is one function on every target, and this coefficient is inside a
    // recursion whose golden is hashed byte for byte.
    audiodsp_sincos_t sc;
    const double half = (double)config->frequency * AUDIODSP_PI /
        (double)config->sample_rate;
    audiodsp_sincos_reflect(half, &sc);
    const double t = sc.c == 0.0 ? 1e9 : sc.s / sc.c;
    config->coefficient = clampf((float)((t - 1.0) / (t + 1.0)),
        -AUDIODSP_FILTER_F32_MAX_ALLPASS_C, AUDIODSP_FILTER_F32_MAX_ALLPASS_C);
    config->derived_sample_rate = config->sample_rate;
    config->derived_frequency = config->frequency;
    config->derived = true;
}

void audiodsp_allpass_f32_state_init(audiodsp_allpass_f32_state_t *state,
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

void audiodsp_allpass_f32_reset(audiodsp_allpass_f32_state_t *state) {
    audiodsp_allpass_f32_state_init(state, state->stage_state,
        state->stage_state_count);
}

void audiodsp_allpass_f32_process_s16(
    const audiodsp_allpass_f32_config_t *config,
    audiodsp_allpass_f32_state_t *state, int16_t *out, const int16_t *in,
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
                // count audiodsp_phaser.c:32-35 pays in int16.
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
