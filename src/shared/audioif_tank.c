// Runtime-neutral reverberation tank. See audioif_tank.h for what the palette
// could not do and why this is a module of its own rather than an argument on
// `audiofreeverb.Freeverb`.
//
// `float` working precision over `int16` lines, deliberately, exactly as in
// audioif_feedback_delay.c: this runs per sample on parts without an FPU at
// all, and doubles would cost more than the last few bits are worth in a
// recirculating network that stores its lines as int16 anyway.
//
// SPDX-License-Identifier: MIT

#include "shared/audioif_fp_contract.h"

#include "shared/audioif_tank.h"

#include <math.h>
#include <string.h>

#define AUDIOIF_TANK_PI 3.14159265358979323846f

//: The tilt control's pivot. A plate's tone knob is one control, not a
//: frequency and a gain, so the frequency is fixed here and documented rather
//: than being a thirteenth option nobody would move.
#define AUDIOIF_TANK_TONE_PIVOT_HZ 1000.0f

//: ln(10)/40: half a decibel-to-linear conversion, since the tilt puts half
//: the asked-for change at each end.
#define AUDIOIF_TANK_DB_TO_LN 0.05756462732485115f

//: The rate Dattorro's published table is written at. Everything scales from
//: it, so a network built at 48 kHz is his network and not a transposed one.
#define AUDIOIF_TANK_REFERENCE_RATE 29761.0f

//: The four all-pass coefficients as ratios of the one knob. Dattorro fixes
//: input diffusion 1 at 0.75, input diffusion 2 at 0.625, decay diffusion 1 at
//: 0.70 and decay diffusion 2 at 0.50; these are those four over the first, so
//: `diffusion=0.75` reproduces his numbers exactly and the knob moves all four
//: together the way a "diffusion" control is expected to.
#define AUDIOIF_TANK_INPUT_DIFFUSION_2 0.83333333f
#define AUDIOIF_TANK_DECAY_DIFFUSION_1 0.93333333f
#define AUDIOIF_TANK_DECAY_DIFFUSION_2 0.66666667f

//: Dattorro's line lengths at 29761 Hz, in this module's table order: four
//: input diffusers, then half A (modulated all-pass, delay, all-pass, delay),
//: then half B.
static const uint16_t audioif_tank_reference_lines[AUDIOIF_TANK_LINES] = {
    142u, 107u, 379u, 277u,
    672u, 4453u, 1800u, 3720u,
    908u, 4217u, 2656u, 3163u,
};

//: Dattorro's output taps at 29761 Hz: channel, line, offset, gain. Seven per
//: channel, each channel reading mostly from the *other* half, which is what
//: puts the two ears in different places in the same room.
static const struct {
    uint8_t channel;
    uint8_t line;
    uint16_t offset;
    float gain;
} audioif_tank_reference_taps[AUDIOIF_TANK_DEFAULT_TAPS] = {
    { 0u,  9u,  266u,  0.6f },
    { 0u,  9u, 2974u,  0.6f },
    { 0u, 10u, 1913u, -0.6f },
    { 0u, 11u, 1996u,  0.6f },
    { 0u,  5u, 1990u, -0.6f },
    { 0u,  6u,  187u, -0.6f },
    { 0u,  7u, 1066u, -0.6f },
    { 1u,  5u,  353u,  0.6f },
    { 1u,  5u, 3627u,  0.6f },
    { 1u,  6u, 1228u, -0.6f },
    { 1u,  7u, 2673u,  0.6f },
    { 1u,  9u, 2111u, -0.6f },
    { 1u, 10u,  335u, -0.6f },
    { 1u, 11u,  121u, -0.6f },
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

// 1 - exp(-2*pi*fc/fs), the one-pole coefficient every filter here uses. Zero
// means "no filter", which is why a cutoff of zero has to fall out here rather
// than being special-cased in the loop. Same function as
// audioif_feedback_delay.c's, and deliberately the same shape.
static float one_pole_coefficient(float hz, uint32_t sample_rate) {
    if (hz <= 0.0f || sample_rate == 0) {
        return 0.0f;
    }
    float coefficient =
        1.0f - expf(-2.0f * AUDIOIF_TANK_PI * hz / (float)sample_rate);
    return clampf(coefficient, 0.0f, 1.0f);
}

static uint32_t scale_to_rate(uint32_t reference, uint32_t sample_rate) {
    const float scaled = (float)reference * (float)sample_rate /
        AUDIOIF_TANK_REFERENCE_RATE;
    if (scaled < 1.0f) {
        return 1u;
    }
    return (uint32_t)(scaled + 0.5f);
}

void audioif_tank_default_delays(uint32_t sample_rate, uint32_t *delays) {
    for (uint32_t line = 0; line < AUDIOIF_TANK_LINES; ++line) {
        uint32_t frames =
            scale_to_rate(audioif_tank_reference_lines[line], sample_rate);
        if (frames < AUDIOIF_TANK_MIN_LINE) {
            frames = AUDIOIF_TANK_MIN_LINE;
        }
        delays[line] = frames;
    }
}

uint32_t audioif_tank_default_taps(uint32_t sample_rate, float *taps) {
    uint32_t lines[AUDIOIF_TANK_LINES];
    audioif_tank_default_delays(sample_rate, lines);
    for (uint32_t tap = 0; tap < AUDIOIF_TANK_DEFAULT_TAPS; ++tap) {
        const uint32_t line = audioif_tank_reference_taps[tap].line;
        uint32_t offset =
            scale_to_rate(audioif_tank_reference_taps[tap].offset, sample_rate);
        if (offset >= lines[line]) {
            offset = lines[line] - 1u;
        }
        taps[tap * 4u + 0u] = (float)audioif_tank_reference_taps[tap].channel;
        taps[tap * 4u + 1u] = (float)line;
        taps[tap * 4u + 2u] = (float)offset;
        taps[tap * 4u + 3u] = audioif_tank_reference_taps[tap].gain;
    }
    return AUDIOIF_TANK_DEFAULT_TAPS;
}

audioif_tank_status_t audioif_tank_set_delays(audioif_tank_config_t *config,
    const uint32_t *frames, uint32_t count) {
    if (count != AUDIOIF_TANK_LINES) {
        return AUDIOIF_TANK_ERR_COUNT;
    }
    uint32_t total = 0;
    for (uint32_t line = 0; line < AUDIOIF_TANK_LINES; ++line) {
        if (frames[line] < AUDIOIF_TANK_MIN_LINE) {
            return AUDIOIF_TANK_ERR_LENGTH;
        }
        // 4 M frames a line, 8 M over the whole network: 16 MB of int16, well
        // past anything a board has and still nowhere near a uint32 overflow.
        if (frames[line] > (1u << 22)) {
            return AUDIOIF_TANK_ERR_TOTAL;
        }
        total += frames[line];
        if (total > (1u << 23)) {
            return AUDIOIF_TANK_ERR_TOTAL;
        }
    }
    for (uint32_t line = 0; line < AUDIOIF_TANK_LINES; ++line) {
        config->line_frames[line] = frames[line];
    }
    // A shorter network can leave a tap past the end of its line; pull them
    // back rather than reading out of bounds later.
    for (uint32_t tap = 0; tap < config->tap_count; ++tap) {
        const uint32_t length = config->line_frames[config->tap_line[tap]];
        if (config->tap_offset[tap] >= length) {
            config->tap_offset[tap] = length - 1u;
        }
    }
    return AUDIOIF_TANK_OK;
}

audioif_tank_status_t audioif_tank_set_taps(audioif_tank_config_t *config,
    const float *values, uint32_t count) {
    if (count % 4u != 0u || count > AUDIOIF_TANK_MAX_TAPS * 4u) {
        return AUDIOIF_TANK_ERR_COUNT;
    }
    const uint32_t taps = count / 4u;
    for (uint32_t tap = 0; tap < taps; ++tap) {
        const float channel = values[tap * 4u + 0u];
        const float line = values[tap * 4u + 1u];
        const float offset = values[tap * 4u + 2u];
        if (channel < 0.0f || channel > 1.0f) {
            return AUDIOIF_TANK_ERR_CHANNEL;
        }
        if (line < 0.0f || line >= (float)AUDIOIF_TANK_LINES) {
            return AUDIOIF_TANK_ERR_LINE;
        }
        if (offset < 0.0f ||
            offset >= (float)config->line_frames[(uint32_t)line]) {
            return AUDIOIF_TANK_ERR_OFFSET;
        }
    }
    for (uint32_t tap = 0; tap < taps; ++tap) {
        config->tap_channel[tap] = (uint8_t)values[tap * 4u + 0u];
        config->tap_line[tap] = (uint8_t)values[tap * 4u + 1u];
        config->tap_offset[tap] = (uint32_t)values[tap * 4u + 2u];
        config->tap_gain[tap] = values[tap * 4u + 3u];
    }
    config->tap_count = taps;
    return AUDIOIF_TANK_OK;
}

void audioif_tank_config_init(audioif_tank_config_t *config,
    uint32_t sample_rate, float max_predelay_ms) {
    memset(config, 0, sizeof(*config));
    config->sample_rate = sample_rate ? sample_rate : 48000u;
    config->channel_count = 2u;
    audioif_tank_default_delays(config->sample_rate, config->line_frames);
    float taps[AUDIOIF_TANK_DEFAULT_TAPS * 4u];
    const uint32_t count =
        audioif_tank_default_taps(config->sample_rate, taps);
    (void)audioif_tank_set_taps(config, taps, count * 4u);
    float predelay = clampf(max_predelay_ms, 0.0f, 10000.0f) *
        (float)config->sample_rate / 1000.0f;
    config->predelay_frames_max = predelay < 1.0f ? 1u : (uint32_t)predelay;
    config->predelay_frames = 0u;
    // Everything a filter defaults to zero, which is this module's word for
    // "out of the path" -- audioecho.FeedbackDelay's convention, so a bare
    // `Tank()` is Dattorro's network with nothing added to it.
    config->decay = 0.5f;
    config->diffusion = 0.75f;
    config->width = 1.0f;
    config->mix = 0.3f;
    config->tone_low_gain = 1.0f;
    config->tone_high_gain = 1.0f;
}

void audioif_tank_set_channel_count(audioif_tank_config_t *config,
    uint32_t channel_count) {
    config->channel_count = channel_count == 1u ? 1u : 2u;
}

void audioif_tank_configure(audioif_tank_config_t *config,
    audioif_tank_option_t option, float value) {
    switch (option) {
        case AUDIOIF_TANK_OPT_DECAY:
            // Stops short of 1. The round trip multiplies by `decay` four
            // times, so even 0.999 is a long but finite tail; unity would be
            // a network that never stops.
            config->decay = clampf(value, 0.0f, 0.999f);
            break;
        case AUDIOIF_TANK_OPT_DIFFUSION:
            // 0.9 rather than 1: an all-pass with unity coefficient has its
            // pole on the unit circle and rings forever.
            config->diffusion = clampf(value, 0.0f, 0.9f);
            break;
        case AUDIOIF_TANK_OPT_DAMPING_HZ:
            config->damping_hz = value;
            config->damping_coef =
                one_pole_coefficient(value, config->sample_rate);
            break;
        case AUDIOIF_TANK_OPT_BANDWIDTH_HZ:
            config->bandwidth_hz = value;
            config->bandwidth_coef =
                one_pole_coefficient(value, config->sample_rate);
            break;
        case AUDIOIF_TANK_OPT_LOW_CUT_HZ:
            config->low_cut_hz = value;
            config->low_cut_coef =
                one_pole_coefficient(value, config->sample_rate);
            break;
        case AUDIOIF_TANK_OPT_PREDELAY_MS: {
            config->predelay_ms = value < 0.0f ? 0.0f : value;
            float frames = config->predelay_ms *
                (float)config->sample_rate / 1000.0f;
            const float ceiling = (float)config->predelay_frames_max;
            config->predelay_frames = (uint32_t)clampf(frames, 0.0f, ceiling);
            break;
        }
        case AUDIOIF_TANK_OPT_MOD_DEPTH_MS:
            config->mod_depth_ms = clampf(value, 0.0f, 100.0f);
            config->mod_depth_frames = config->mod_depth_ms *
                (float)config->sample_rate / 1000.0f;
            break;
        case AUDIOIF_TANK_OPT_MOD_RATE_HZ:
            config->mod_rate_hz = clampf(value, 0.0f, 100.0f);
            config->mod_step = config->mod_rate_hz <= 0.0f ? 0.0f :
                2.0f * sinf(AUDIOIF_TANK_PI * config->mod_rate_hz /
                    (float)config->sample_rate);
            break;
        case AUDIOIF_TANK_OPT_DRIVE:
            config->drive = clampf(value, 0.0f, 1.0f);
            break;
        case AUDIOIF_TANK_OPT_WIDTH:
            config->width = clampf(value, 0.0f, 2.0f);
            break;
        case AUDIOIF_TANK_OPT_TONE_DB:
            config->tone_db = clampf(value, -24.0f, 24.0f);
            // A tilt: half the asked-for change up at the top and the same
            // amount down at the bottom, pivoting at 1 kHz. One knob for
            // bright-to-dark rather than a shelf that only lifts one end; what
            // it does to the *level* depends on where the material's energy
            // sits, so it is a balance control and not a trim. Through expf
            // with ln(10)/40
            // folded in rather than powf, which is how audioif_dynamics.c:17
            // does its own dB conversion and keeps powf out of an mcu build.
            config->tone_low_gain =
                expf(-config->tone_db * AUDIOIF_TANK_DB_TO_LN);
            config->tone_high_gain =
                expf(config->tone_db * AUDIOIF_TANK_DB_TO_LN);
            config->tone_coef = one_pole_coefficient(
                AUDIOIF_TANK_TONE_PIVOT_HZ, config->sample_rate);
            break;
        case AUDIOIF_TANK_OPT_MIX:
            // 0..2, and the dry stays at unity until 1 -- `audiodelays.Echo`'s
            // convention, which `audioecho.FeedbackDelay` already adopted for
            // the same reason: a chain built out of several of these nodes
            // needs `mix` to mean one thing everywhere.
            config->mix = clampf(value, 0.0f, 2.0f);
            break;
    }
}

void audioif_tank_config_finish(audioif_tank_config_t *config) {
    audioif_tank_configure(config, AUDIOIF_TANK_OPT_DAMPING_HZ,
        config->damping_hz);
    audioif_tank_configure(config, AUDIOIF_TANK_OPT_BANDWIDTH_HZ,
        config->bandwidth_hz);
    audioif_tank_configure(config, AUDIOIF_TANK_OPT_LOW_CUT_HZ,
        config->low_cut_hz);
    audioif_tank_configure(config, AUDIOIF_TANK_OPT_MOD_RATE_HZ,
        config->mod_rate_hz);
    audioif_tank_configure(config, AUDIOIF_TANK_OPT_MOD_DEPTH_MS,
        config->mod_depth_ms);
    audioif_tank_configure(config, AUDIOIF_TANK_OPT_PREDELAY_MS,
        config->predelay_ms);
    audioif_tank_configure(config, AUDIOIF_TANK_OPT_TONE_DB, config->tone_db);
    // The modulated all-pass reads between `length - 1 - 2*depth` and
    // `length - 1`, so the excursion cannot be more than half a line.
    for (uint32_t half = 0; half < 2u; ++half) {
        const uint32_t line = 4u + 4u * half;
        const float ceiling = ((float)config->line_frames[line] - 2.0f) * 0.5f;
        if (config->mod_depth_frames > ceiling) {
            config->mod_depth_frames = ceiling < 0.0f ? 0.0f : ceiling;
        }
    }
}

uint32_t audioif_tank_buffer_samples(const audioif_tank_config_t *config) {
    uint32_t total = config->predelay_frames_max;
    for (uint32_t line = 0; line < AUDIOIF_TANK_LINES; ++line) {
        total += config->line_frames[line];
    }
    return total;
}

void audioif_tank_state_init(audioif_tank_state_t *state,
    const audioif_tank_config_t *config, int16_t *buffer) {
    int16_t *cursor = buffer;
    for (uint32_t line = 0; line < AUDIOIF_TANK_LINES; ++line) {
        state->lines[line] = cursor;
        state->write[line] = 0u;
        cursor += config->line_frames[line];
    }
    state->predelay = cursor;
    state->predelay_write = 0u;
    state->bandwidth_state = 0.0f;
    state->low_cut_state = 0.0f;
    state->damping_state[0] = state->damping_state[1] = 0.0f;
    state->tone_state[0] = state->tone_state[1] = 0.0f;
    state->mod_sine = 0.0f;
    state->mod_cosine = 1.0f;
}

void audioif_tank_reset(audioif_tank_state_t *state,
    const audioif_tank_config_t *config) {
    if (state->lines[0] != NULL) {
        memset(state->lines[0], 0,
            (size_t)audioif_tank_buffer_samples(config) * sizeof(int16_t));
    }
    int16_t *buffer = state->lines[0];
    audioif_tank_state_init(state, config, buffer);
}

// The one place the whole thing is not linear, and the same cubic
// audioif_feedback_delay.c soft-clips its loop with: odd-symmetric, so it
// makes third-harmonic thickening and no second, and two multiplies where a
// tanh would be a library call per sample.
static float tank_soft_clip(float value, float drive) {
    const float normalized = clampf(value * (1.0f / 32768.0f), -1.0f, 1.0f);
    return 32768.0f *
        (normalized - drive * normalized * normalized * normalized / 3.0f);
}

// The quantiser every *line* write goes through, and the reason the tail
// reaches exact zero rather than sitting at one LSB forever.
//
// Magnitude truncation, not rounding. A recirculating int16 network that
// rounds to nearest has limit cycles by construction: a line holding 1 with
// decay 0.5 computes 0.5, which rounds back to 1, and the tank hums at -90
// dBFS for as long as it is pulled. Truncating toward zero makes |Q(v)| <=
// |v| unconditionally, so a loop whose gain is under unity strictly loses
// magnitude every pass and lands on exact zero. It is the standard cure for
// fixed-point limit cycles and it costs nothing.
static int16_t tank_quantize(float value) {
    if (value >= 32767.0f) {
        return 32767;
    }
    if (value <= -32768.0f) {
        return -32768;
    }
    return (int16_t)value;
}

// The *output* quantiser, which is not in any loop, so it rounds -- the same
// one audioif_feedback_delay.c hands its blocks out with.
static int16_t tank_to_s16(float value) {
    if (value > 32767.0f) {
        return 32767;
    }
    if (value < -32768.0f) {
        return -32768;
    }
    return (int16_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

// A Schroeder all-pass over a whole line: the delay is the line's length, so
// the read index is the write cursor itself (which still holds what was
// written `length` frames ago) and the write lands on top of it.
//
//   d = line[w];  v = x + g*d;  line[w] = v;  y = d - g*v
//
// which is (z^-L - g) / (1 - g*z^-L): flat magnitude, dispersive phase. One
// sign convention for all six all-passes here, input diffusers and tank alike.
static float tank_allpass(int16_t *line, uint32_t write, float gain, float x) {
    const float delayed = (float)line[write];
    const float v = x + gain * delayed;
    line[write] = tank_quantize(v);
    return delayed - gain * v;
}

// A plain delay of the line's whole length.
static float tank_delay(int16_t *line, uint32_t write, float x) {
    const float delayed = (float)line[write];
    line[write] = tank_quantize(x);
    return delayed;
}

// The same all-pass with a fractional, moving read: the tank's first all-pass
// in each half wobbles by a fraction of a millisecond, which is what breaks
// the picket-fence ringing a static plate has. Nominal delay is one frame
// under the line so the interpolator's older neighbour always exists.
static float tank_mod_allpass(int16_t *line, uint32_t length, uint32_t write,
    float gain, float x, float depth, float lfo) {
    float offset = (float)length - 1.0f - depth + depth * lfo;
    offset = clampf(offset, 1.0f, (float)length - 1.0f);
    const uint32_t whole = (uint32_t)offset;
    const float fraction = offset - (float)whole;
    const uint32_t near_frame = (write + length - whole) % length;
    const uint32_t far_frame = (near_frame + length - 1u) % length;
    const float near_sample = (float)line[near_frame];
    const float delayed = near_sample +
        fraction * ((float)line[far_frame] - near_sample);
    const float v = x + gain * delayed;
    line[write] = tank_quantize(v);
    return delayed - gain * v;
}

void audioif_tank_process_s16(const audioif_tank_config_t *config,
    audioif_tank_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames) {
    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    const float dry = 2.0f - config->mix < 1.0f ? 2.0f - config->mix : 1.0f;
    const float wet = config->mix < 1.0f ? config->mix : 1.0f;
    const float decay = config->decay;
    const float g_input_1 = config->diffusion;
    const float g_input_2 = config->diffusion * AUDIOIF_TANK_INPUT_DIFFUSION_2;
    const float g_tank_1 = config->diffusion * AUDIOIF_TANK_DECAY_DIFFUSION_1;
    const float g_tank_2 = config->diffusion * AUDIOIF_TANK_DECAY_DIFFUSION_2;
    // Depth without a rate is a detune, not a modulation, and would leave the
    // two halves permanently different lengths. It takes both.
    const float depth =
        config->mod_step != 0.0f ? config->mod_depth_frames : 0.0f;
    const uint32_t predelay_length = config->predelay_frames_max;

    for (uint32_t frame = 0; frame < frames; ++frame) {
        // Rotate the modulation oscillator one step. Updating the sine first
        // and feeding the new value back into the cosine is what keeps this
        // stable indefinitely; the naive pair drifts in amplitude. The two
        // outputs are a quarter cycle apart, which is the quadrature pair the
        // two halves wobble against.
        state->mod_sine += config->mod_step * state->mod_cosine;
        state->mod_cosine -= config->mod_step * state->mod_sine;
        const float lfo[2] = { state->mod_sine, state->mod_cosine };

        // The tank is fed one signal. A stereo input is summed to it, and the
        // stereo comes back out of the tap table -- which is how a plate works
        // and why `width` is a control here rather than two tanks.
        float x = channels == 2u
            ? ((float)in[frame * 2u] + (float)in[frame * 2u + 1u]) * 0.5f
            : (float)in[frame];

        // Predelay. Written every frame even when it is switched off, so
        // turning it on mid-stream reads real audio rather than whatever was
        // in the line the last time anybody used it.
        {
            const uint32_t back = config->predelay_frames;
            float delayed = x;
            if (back >= 1u && back <= predelay_length) {
                const uint32_t index =
                    (state->predelay_write + predelay_length - back) %
                    predelay_length;
                delayed = (float)state->predelay[index];
            }
            state->predelay[state->predelay_write] = tank_to_s16(x);
            state->predelay_write =
                (state->predelay_write + 1u) % predelay_length;
            x = delayed;
        }

        if (config->bandwidth_coef > 0.0f) {
            state->bandwidth_state +=
                config->bandwidth_coef * (x - state->bandwidth_state);
            x = state->bandwidth_state;
        }
        if (config->low_cut_coef > 0.0f) {
            state->low_cut_state +=
                config->low_cut_coef * (x - state->low_cut_state);
            x -= state->low_cut_state;
        }
        if (config->drive > 0.0f) {
            x = tank_soft_clip(x, config->drive);
        }

        x = tank_allpass(state->lines[0], state->write[0], g_input_1, x);
        x = tank_allpass(state->lines[1], state->write[1], g_input_1, x);
        x = tank_allpass(state->lines[2], state->write[2], g_input_2, x);
        x = tank_allpass(state->lines[3], state->write[3], g_input_2, x);

        // Each half's contribution to the other one, read out of its last
        // delay line before anything this frame writes to it. That read is
        // what breaks the cycle: it is the value that entered the line a whole
        // line-length ago, so neither half waits on the other.
        float feedback[2];
        for (uint32_t half = 0; half < 2u; ++half) {
            const uint32_t last = 7u + 4u * half;
            feedback[half] =
                (float)state->lines[last][state->write[last]] * decay;
        }

        for (uint32_t half = 0; half < 2u; ++half) {
            const uint32_t base = 4u + 4u * half;
            float v = x + feedback[1u - half];
            v = tank_mod_allpass(state->lines[base], config->line_frames[base],
                state->write[base], g_tank_1, v, depth, lfo[half]);
            v = tank_delay(state->lines[base + 1u], state->write[base + 1u], v);
            if (config->damping_coef > 0.0f) {
                state->damping_state[half] +=
                    config->damping_coef * (v - state->damping_state[half]);
                v = state->damping_state[half];
            }
            v *= decay;
            v = tank_allpass(state->lines[base + 2u], state->write[base + 2u],
                g_tank_2, v);
            state->lines[base + 3u][state->write[base + 3u]] =
                tank_quantize(v);
        }

        // The taps, read after every line this frame writes has been written,
        // so an offset of 0 is the sample that just went in.
        float tapped[2] = { 0.0f, 0.0f };
        for (uint32_t tap = 0; tap < config->tap_count; ++tap) {
            const uint32_t line = config->tap_line[tap];
            const uint32_t length = config->line_frames[line];
            uint32_t back = config->tap_offset[tap];
            if (back >= length) {
                back = length - 1u;
            }
            const uint32_t index = (state->write[line] + length - back) %
                length;
            // A mono tank is the mono fold-down of the stereo one, not half
            // its tap table: both channels' taps land in the one lane and the
            // sum is halved below.
            const uint32_t lane =
                channels == 2u ? config->tap_channel[tap] : 0u;
            tapped[lane] += config->tap_gain[tap] * (float)state->lines[line][index];
        }
        if (channels == 1u) {
            tapped[0] *= 0.5f;
        }

        for (uint32_t line = 0; line < AUDIOIF_TANK_LINES; ++line) {
            state->write[line] =
                (state->write[line] + 1u) % config->line_frames[line];
        }

        if (channels == 2u && config->width != 1.0f) {
            const float mid = (tapped[0] + tapped[1]) * 0.5f;
            const float side = (tapped[0] - tapped[1]) * 0.5f * config->width;
            tapped[0] = mid + side;
            tapped[1] = mid - side;
        }

        for (uint32_t channel = 0; channel < channels; ++channel) {
            float value = tapped[channel];
            if (config->tone_db != 0.0f && config->tone_coef > 0.0f) {
                state->tone_state[channel] +=
                    config->tone_coef * (value - state->tone_state[channel]);
                value = state->tone_state[channel] * config->tone_low_gain +
                    (value - state->tone_state[channel]) *
                    config->tone_high_gain;
            }
            const float source = (float)in[frame * channels + channel];
            out[frame * channels + channel] =
                tank_to_s16(dry * source + wet * value);
        }
    }
}
