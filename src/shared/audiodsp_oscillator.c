// Runtime-neutral band-limited oscillator. See audiodsp_oscillator.h.
// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"
#include "shared/audiodsp_hot.h"

#include "shared/audiodsp_oscillator.h"
#include "shared/audiodsp_exp2.h"

#include <stdbool.h>
#include <stddef.h>

enum { SAW = 0, PULSE = 1, TRIANGLE = 2, SINE = 3 };

#define TWO_PI 6.28318530717958647692f
#define PW_MIN 0.05f
#define PW_MAX 0.95f

void audiodsp_oscillator_config_init(audiodsp_oscillator_config_t *config,
    uint32_t sample_rate) {
    config->sample_rate = sample_rate < 1u ? 1u : sample_rate;
    config->channel_count = 1;
    config->frequency = 440.0f;
    config->pulse_width = 0.5f;
    config->level = 1.0f;
    config->pitch_octaves = 10.0f;
    config->fm_depth = 0.0f;
    config->pwm_depth = 0.5f;
    config->waveform[0] = AUDIODSP_OSCILLATOR_SAW;
    config->waveform[1] = AUDIODSP_OSCILLATOR_SAW;
    audiodsp_oscillator_config_finish(config);
}

void audiodsp_oscillator_set_channel_count(
    audiodsp_oscillator_config_t *config, uint32_t channel_count) {
    config->channel_count = channel_count == 2u ? 2u : 1u;
}

static uint8_t to_waveform(float value) {
    if (!(value > 0.0f)) {
        return 0;
    }
    if (value >= (float)AUDIODSP_OSCILLATOR_WAVES) {
        return AUDIODSP_OSCILLATOR_WAVES;
    }
    return (uint8_t)value;
}

void audiodsp_oscillator_configure(audiodsp_oscillator_config_t *config,
    audiodsp_oscillator_option_t option, float value) {
    switch (option) {
        case AUDIODSP_OSCILLATOR_OPT_FREQUENCY:
            config->frequency = value;
            break;
        case AUDIODSP_OSCILLATOR_OPT_WAVEFORM:
            config->waveform[0] = to_waveform(value);
            break;
        case AUDIODSP_OSCILLATOR_OPT_WAVEFORM_RIGHT:
            config->waveform[1] = to_waveform(value);
            break;
        case AUDIODSP_OSCILLATOR_OPT_PULSE_WIDTH:
            config->pulse_width = value;
            break;
        case AUDIODSP_OSCILLATOR_OPT_LEVEL:
            config->level = value;
            break;
        case AUDIODSP_OSCILLATOR_OPT_PITCH_OCTAVES:
            config->pitch_octaves = value;
            break;
        case AUDIODSP_OSCILLATOR_OPT_FM_DEPTH:
            config->fm_depth = value;
            break;
        case AUDIODSP_OSCILLATOR_OPT_PWM_DEPTH:
            config->pwm_depth = value;
            break;
        default:
            break;
    }
}

static float clampf(float value, float low, float high) {
    if (!(value >= low)) {
        return low;
    }
    return value > high ? high : value;
}

void audiodsp_oscillator_config_finish(audiodsp_oscillator_config_t *config) {
    const float rate = (float)config->sample_rate;
    const float inv_rate = 1.0f / rate;
    config->inv_rate = inv_rate;
    config->dt_min = 0.1f * inv_rate;
    config->dt_max = 0.49f;
    config->base_dt = clampf(config->frequency * inv_rate, config->dt_min,
        config->dt_max);
    config->pulse_width = clampf(config->pulse_width, PW_MIN, PW_MAX);
    config->pitch_scale = config->pitch_octaves * (1.0f / 32768.0f);
    config->fm_scale = config->fm_depth * inv_rate * (1.0f / 32768.0f);
    config->pwm_scale = config->pwm_depth * (1.0f / 32768.0f);
    config->gain = config->level * 32768.0f;
    config->waves = config->waveform[0];
    if (config->channel_count == 2u) {
        config->waves |= config->waveform[1];
    }
}

static void reset_pending(audiodsp_oscillator_state_t *state) {
    // Every waveform's value at phase 0: the saw and the triangle at the
    // bottom, the pulse high, the sine at zero.
    state->pending[SAW] = -1.0f;
    state->pending[PULSE] = 1.0f;
    state->pending[TRIANGLE] = -1.0f;
    state->pending[SINE] = 0.0f;
}

void audiodsp_oscillator_state_init(audiodsp_oscillator_state_t *state) {
    state->pitch_last = INT32_MIN;
    state->pitch_scale = 0.0f;
    state->pitch_ratio = 1.0f;
    audiodsp_oscillator_reset(state);
}

void audiodsp_oscillator_reset(audiodsp_oscillator_state_t *state) {
    state->phase = 0.0f;
    state->sync_last = 0.0f;
    state->pulse_high = 1;
    reset_pending(state);
}

// sin(2 pi phase) for phase in [0, 1), deterministic for the same reason.
// Folded into a quarter either side of zero, then an odd series to the
// eleventh power.
AUDIODSP_GRAPH_HOT
static float sin_cycle(float phase) {
    float y;
    if (phase < 0.25f) {
        y = phase;
    } else if (phase < 0.75f) {
        y = 0.5f - phase;
    } else {
        y = phase - 1.0f;
    }
    const float x = y * TWO_PI;
    const float x2 = x * x;
    return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f
        + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f
        + x2 * (-1.0f / 39916800.0f))))));
}

static float triangle_at(float phase) {
    return phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase;
}

// A step of `height` that happened `t` samples before the sample being
// computed (0 <= t <= 1): half of the PolyBLEP residual goes on the sample
// before the step, half on the one after.
static void blep(float *before, float *after, float height, float t) {
    const float u = 1.0f - t;
    *before += height * 0.5f * t * t;
    *after -= height * 0.5f * u * u;
}

// A change of `slope` (in units per sample) at the same place: the integral
// of the residual above.
static void blamp(float *before, float *after, float slope, float t) {
    const float u = 1.0f - t;
    *before += slope * (1.0f / 6.0f) * t * t * t;
    *after += slope * (1.0f / 6.0f) * u * u * u;
}

static float clamp_t(float t) {
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

// 1 / dt, worked out the first time an event needs it. `*inv_dt` is 0 until
// then: a sample with no event in it, which is most of them, never divides.
static float seg_inv_dt(float dt, float *inv_dt) {
    if (*inv_dt == 0.0f) {
        *inv_dt = 1.0f / dt;
    }
    return *inv_dt;
}

// Advances `*phase` to `end` (unwrapped, so at most one reset), placing the
// events crossed on the way. `offset` is how many samples the segment ends
// before the sample being computed.
AUDIODSP_GRAPH_HOT
static void run_segment(audiodsp_oscillator_state_t *state, float *next,
    uint8_t waves, float *phase, float end, float dt, float *inv_dt, float pw,
    float offset) {
    float *cur = state->pending;
    float start = *phase;
    for (;;) {
        if (state->pulse_high && end >= pw) {
            const float at = start > pw ? start : pw;
            if (waves & AUDIODSP_OSCILLATOR_PULSE) {
                blep(&cur[PULSE], &next[PULSE], -2.0f,
                    clamp_t(offset + (end - at) * seg_inv_dt(dt, inv_dt)));
            }
            state->pulse_high = 0;
        }
        if ((waves & AUDIODSP_OSCILLATOR_TRIANGLE) && start < 0.5f &&
            end >= 0.5f) {
            blamp(&cur[TRIANGLE], &next[TRIANGLE], -8.0f * dt,
                clamp_t(offset + (end - 0.5f) * seg_inv_dt(dt, inv_dt)));
        }
        if (end < 1.0f) {
            break;
        }
        end -= 1.0f;
        // Only the saw, the pulse and the triangle place anything at the
        // wrap.
        const bool edges = (waves & (AUDIODSP_OSCILLATOR_SAW |
            AUDIODSP_OSCILLATOR_TRIANGLE)) ||
            (!state->pulse_high && (waves & AUDIODSP_OSCILLATOR_PULSE));
        const float t = edges
            ? clamp_t(offset + end * seg_inv_dt(dt, inv_dt)) : 0.0f;
        if (waves & AUDIODSP_OSCILLATOR_SAW) {
            blep(&cur[SAW], &next[SAW], -2.0f, t);
        }
        if (!state->pulse_high) {
            if (waves & AUDIODSP_OSCILLATOR_PULSE) {
                blep(&cur[PULSE], &next[PULSE], 2.0f, t);
            }
            state->pulse_high = 1;
        }
        if (waves & AUDIODSP_OSCILLATOR_TRIANGLE) {
            blamp(&cur[TRIANGLE], &next[TRIANGLE], 8.0f * dt, t);
        }
        start = 0.0f;
    }
    *phase = end;
}

// A hard sync `t` samples before the sample being computed: every waveform
// jumps from where it was to its value at phase 0, and the triangle and sine
// also turn a corner.
static void sync_reset(audiodsp_oscillator_state_t *state, float *next,
    uint8_t waves, float at, float dt, float t) {
    float *cur = state->pending;
    if (waves & AUDIODSP_OSCILLATOR_SAW) {
        blep(&cur[SAW], &next[SAW], -2.0f * at, t);
    }
    if (!state->pulse_high) {
        if (waves & AUDIODSP_OSCILLATOR_PULSE) {
            blep(&cur[PULSE], &next[PULSE], 2.0f, t);
        }
        state->pulse_high = 1;
    }
    if (waves & AUDIODSP_OSCILLATOR_TRIANGLE) {
        blep(&cur[TRIANGLE], &next[TRIANGLE], -1.0f - triangle_at(at), t);
        if (at >= 0.5f) {
            blamp(&cur[TRIANGLE], &next[TRIANGLE], 8.0f * dt, t);
        }
    }
    if (waves & AUDIODSP_OSCILLATOR_SINE) {
        float quarter = at + 0.25f;
        if (quarter >= 1.0f) {
            quarter -= 1.0f;
        }
        blep(&cur[SINE], &next[SINE], -sin_cycle(at), t);
        blamp(&cur[SINE], &next[SINE],
            TWO_PI * dt * (1.0f - sin_cycle(quarter)), t);
    }
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

static float mix_waves(const float *values, uint8_t waves) {
    float sum = 0.0f;
    if (waves & AUDIODSP_OSCILLATOR_SAW) {
        sum += values[SAW];
    }
    if (waves & AUDIODSP_OSCILLATOR_PULSE) {
        sum += values[PULSE];
    }
    if (waves & AUDIODSP_OSCILLATOR_TRIANGLE) {
        sum += values[TRIANGLE];
    }
    if (waves & AUDIODSP_OSCILLATOR_SINE) {
        sum += values[SINE];
    }
    return sum;
}

// One sample of a free-running oscillator: the loop below with no inputs.
AUDIODSP_GRAPH_HOT
static void free_step(const audiodsp_oscillator_config_t *config,
    audiodsp_oscillator_state_t *state, int16_t *out, uint32_t channels,
    float *phase, float dt, float inv_dt, float pw) {
    const uint8_t waves = config->waves;
    float next[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    run_segment(state, next, waves, phase, *phase + dt, dt, &inv_dt, pw, 0.0f);
    if (waves & AUDIODSP_OSCILLATOR_SAW) {
        next[SAW] += 2.0f * *phase - 1.0f;
    }
    if (waves & AUDIODSP_OSCILLATOR_PULSE) {
        next[PULSE] += state->pulse_high ? 1.0f : -1.0f;
    }
    if (waves & AUDIODSP_OSCILLATOR_TRIANGLE) {
        next[TRIANGLE] += triangle_at(*phase);
    }
    if (waves & AUDIODSP_OSCILLATOR_SINE) {
        next[SINE] += sin_cycle(*phase);
    }
    float *cur = state->pending;
    out[0] = to_s16(mix_waves(cur, config->waveform[0]) * config->gain);
    if (channels == 2u) {
        out[1] = to_s16(mix_waves(cur, config->waveform[1]) * config->gain);
    }
    cur[SAW] = next[SAW];
    cur[PULSE] = next[PULSE];
    cur[TRIANGLE] = next[TRIANGLE];
    cur[SINE] = next[SINE];
}

// With nothing patched into it, an oscillator's step, its reciprocal and its
// pulse width hold for the whole block, so they are worked out once. The
// clamps the loop below applies each sample change nothing here:
// config_finish has already clamped the step and the pulse width to the same
// bounds. A mono pulse, as a metal bank's six are, writes the same sample
// every frame between two edges, so those frames only advance the phase; a
// frame with an edge, or the one just after it, takes the full step. Every
// sample comes out as the loop below would make it.
AUDIODSP_GRAPH_HOT
static void free_run(const audiodsp_oscillator_config_t *config,
    audiodsp_oscillator_state_t *state, int16_t *out, uint32_t frames,
    uint32_t channels) {
    const float dt = config->base_dt;
    const float inv_dt = 1.0f / dt;
    const float pw = config->pulse_width;
    float phase = state->phase;
    uint32_t i = 0;
    if (channels == 1u && config->waves == AUDIODSP_OSCILLATOR_PULSE &&
        config->waveform[0] == AUDIODSP_OSCILLATOR_PULSE && frames > 0u) {
        const float zero = 0.0f;
        const int16_t high = to_s16((zero + 1.0f) * config->gain);
        const int16_t low = to_s16((zero + -1.0f) * config->gain);
        while (i < frames) {
            const bool up = state->pulse_high != 0;
            if (state->pending[PULSE] == (up ? 1.0f : -1.0f)) {
                // The next edge: the fall at the pulse width while high, the
                // rise at the wrap while low.
                const float edge = up ? pw : 1.0f;
                const int16_t v = up ? high : low;
                while (i < frames) {
                    const float end = phase + dt;
                    if (end >= edge) {
                        break;
                    }
                    phase = end;
                    out[i++] = v;
                }
                if (i == frames) {
                    break;
                }
            }
            free_step(config, state, &out[i], 1u, &phase, dt, inv_dt, pw);
            ++i;
        }
        // The loop below leaves the waveforms not playing at zero.
        state->pending[SAW] = 0.0f;
        state->pending[TRIANGLE] = 0.0f;
        state->pending[SINE] = 0.0f;
    } else {
        for (; i < frames; ++i) {
            free_step(config, state, &out[i * channels], channels, &phase,
                dt, inv_dt, pw);
        }
    }
    state->phase = phase;
}

// A sine and nothing else, with only its pitch patched in: a drum machine's
// swept kick and toms. The loop below, sample for sample, without the events a
// sine never has. The phase still wraps where that loop wraps it, and the
// pulse's high/low state, which a sine does not read, still moves as it would,
// so a waveform switched on later starts from the same place.
AUDIODSP_GRAPH_HOT
static void pitched_sine(
    const audiodsp_oscillator_config_t *config,
    audiodsp_oscillator_state_t *state, int16_t *out, uint32_t frames,
    uint32_t channels, const int16_t *pitch, uint32_t stride) {
    const float gain = config->gain;
    const float pw = config->pulse_width;
    float phase = state->phase;
    uint8_t high = state->pulse_high;
    float *cur = state->pending;
    float sine = cur[SINE];
    for (uint32_t frame = 0; frame < frames; ++frame) {
        float dt = config->base_dt;
        if (pitch != NULL) {
            const int32_t value = pitch[frame * stride];
            if (value != state->pitch_last ||
                config->pitch_scale != state->pitch_scale) {
                state->pitch_last = value;
                state->pitch_scale = config->pitch_scale;
                state->pitch_ratio = audiodsp_exp2_poly((float)value
                    * config->pitch_scale);
            }
            dt = config->frequency * config->inv_rate * state->pitch_ratio;
        }
        dt = clampf(dt, config->dt_min, config->dt_max);
        float end = phase + dt;
        if (high && end >= pw) {
            high = 0;
        }
        if (!(end < 1.0f)) {
            end -= 1.0f;
            high = end >= pw ? 0 : 1;
        }
        phase = end;
        const int16_t v = to_s16((0.0f + sine) * gain);
        out[frame * channels] = v;
        if (channels == 2u) {
            out[frame * channels + 1u] = v;
        }
        sine = 0.0f + sin_cycle(phase);
    }
    cur[SAW] = 0.0f;
    cur[PULSE] = 0.0f;
    cur[TRIANGLE] = 0.0f;
    cur[SINE] = sine;
    state->pulse_high = high;
    state->phase = phase;
}

AUDIODSP_GRAPH_HOT
void audiodsp_oscillator_process_s16(
    const audiodsp_oscillator_config_t *config,
    audiodsp_oscillator_state_t *state, int16_t *out, uint32_t frames,
    const int16_t *const inputs[AUDIODSP_OSCILLATOR_INPUTS],
    const uint32_t strides[AUDIODSP_OSCILLATOR_INPUTS]) {
    const int16_t *pitch = inputs[AUDIODSP_OSCILLATOR_IN_PITCH];
    const int16_t *fm = inputs[AUDIODSP_OSCILLATOR_IN_FM];
    const int16_t *pwm = inputs[AUDIODSP_OSCILLATOR_IN_PWM];
    const int16_t *sync = inputs[AUDIODSP_OSCILLATOR_IN_SYNC];
    if (pitch == NULL && fm == NULL && pwm == NULL && sync == NULL) {
        free_run(config, state, out, frames,
            config->channel_count == 2u ? 2u : 1u);
        return;
    }
    const uint32_t channels = config->channel_count == 2u ? 2u : 1u;
    const uint8_t waves = config->waves;
    const float gain = config->gain;
    float phase = state->phase;
    if (fm == NULL && pwm == NULL && sync == NULL &&
        config->waveform[0] == AUDIODSP_OSCILLATOR_SINE &&
        (channels == 1u || config->waveform[1] == AUDIODSP_OSCILLATOR_SINE) &&
        frames > 0u) {
        pitched_sine(config, state, out, frames, channels, pitch, strides[0]);
        return;
    }
    for (uint32_t frame = 0; frame < frames; ++frame) {
        float dt = config->base_dt;
        if (pitch != NULL) {
            const int32_t value = pitch[frame * strides[0]];
            if (value != state->pitch_last ||
                config->pitch_scale != state->pitch_scale) {
                state->pitch_last = value;
                state->pitch_scale = config->pitch_scale;
                state->pitch_ratio = audiodsp_exp2_poly((float)value
                    * config->pitch_scale);
            }
            dt = config->frequency * config->inv_rate * state->pitch_ratio;
        }
        if (fm != NULL) {
            dt += (float)fm[frame * strides[1]] * config->fm_scale;
        }
        dt = clampf(dt, config->dt_min, config->dt_max);
        float inv_dt = 0.0f;
        float pw = config->pulse_width;
        if (pwm != NULL) {
            pw = clampf(pw + (float)pwm[frame * strides[2]]
                * config->pwm_scale, PW_MIN, PW_MAX);
        }

        float next[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        bool synced = false;
        float since = 0.0f;
        if (sync != NULL) {
            const float value = (float)sync[frame * strides[3]];
            const float last = state->sync_last;
            if (last <= 0.0f && value > 0.0f) {
                // Where between the two samples the input crossed zero, as a
                // fraction of the way from the last one.
                const float crossed = last / (last - value);
                since = 1.0f - crossed;
                synced = true;
            }
            state->sync_last = value;
        }
        if (synced) {
            run_segment(state, next, waves, &phase,
                phase + (1.0f - since) * dt, dt, &inv_dt, pw, since);
            sync_reset(state, next, waves, phase, dt, since);
            phase = 0.0f;
            run_segment(state, next, waves, &phase, since * dt, dt, &inv_dt,
                pw, 0.0f);
        } else {
            run_segment(state, next, waves, &phase, phase + dt, dt, &inv_dt,
                pw, 0.0f);
        }

        // The new sample's own value goes on top of the half-kernels its
        // events left on it; the finished sample before it goes out.
        if (waves & AUDIODSP_OSCILLATOR_SAW) {
            next[SAW] += 2.0f * phase - 1.0f;
        }
        if (waves & AUDIODSP_OSCILLATOR_PULSE) {
            next[PULSE] += state->pulse_high ? 1.0f : -1.0f;
        }
        if (waves & AUDIODSP_OSCILLATOR_TRIANGLE) {
            next[TRIANGLE] += triangle_at(phase);
        }
        if (waves & AUDIODSP_OSCILLATOR_SINE) {
            next[SINE] += sin_cycle(phase);
        }
        float *cur = state->pending;
        out[frame * channels] = to_s16(mix_waves(cur, config->waveform[0])
            * gain);
        if (channels == 2u) {
            out[frame * channels + 1u] = to_s16(
                mix_waves(cur, config->waveform[1]) * gain);
        }
        cur[SAW] = next[SAW];
        cur[PULSE] = next[PULSE];
        cur[TRIANGLE] = next[TRIANGLE];
        cur[SINE] = next[SINE];
    }
    state->phase = phase;
}
