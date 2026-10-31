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

static float AUDIODSP_GRAPH_HOT clampf(float value, float low, float high) {
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
static float AUDIODSP_GRAPH_HOT sin_cycle(float phase) {
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

static float AUDIODSP_GRAPH_HOT triangle_at(float phase) {
    return phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase;
}

// A step of `height` that happened `t` samples before the sample being
// computed (0 <= t <= 1): half of the PolyBLEP residual goes on the sample
// before the step, half on the one after.
static void AUDIODSP_GRAPH_HOT blep(float *before, float *after, float height, float t) {
    const float u = 1.0f - t;
    *before += height * 0.5f * t * t;
    *after -= height * 0.5f * u * u;
}

// A change of `slope` (in units per sample) at the same place: the integral
// of the residual above.
static void AUDIODSP_GRAPH_HOT blamp(float *before, float *after, float slope, float t) {
    const float u = 1.0f - t;
    *before += slope * (1.0f / 6.0f) * t * t * t;
    *after += slope * (1.0f / 6.0f) * u * u * u;
}

static float AUDIODSP_GRAPH_HOT clamp_t(float t) {
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

// Advances `*phase` to `end` (unwrapped, so at most one reset), placing the
// events crossed on the way. `offset` is how many samples the segment ends
// before the sample being computed.
static void AUDIODSP_GRAPH_HOT run_segment(audiodsp_oscillator_state_t *state, float *next,
    uint8_t waves, float *phase, float end, float dt, float inv_dt, float pw,
    float offset) {
    float *cur = state->pending;
    float start = *phase;
    for (;;) {
        if (state->pulse_high && end >= pw) {
            const float at = start > pw ? start : pw;
            if (waves & AUDIODSP_OSCILLATOR_PULSE) {
                blep(&cur[PULSE], &next[PULSE], -2.0f,
                    clamp_t(offset + (end - at) * inv_dt));
            }
            state->pulse_high = 0;
        }
        if ((waves & AUDIODSP_OSCILLATOR_TRIANGLE) && start < 0.5f &&
            end >= 0.5f) {
            blamp(&cur[TRIANGLE], &next[TRIANGLE], -8.0f * dt,
                clamp_t(offset + (end - 0.5f) * inv_dt));
        }
        if (end < 1.0f) {
            break;
        }
        end -= 1.0f;
        const float t = clamp_t(offset + end * inv_dt);
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
static void AUDIODSP_GRAPH_HOT sync_reset(audiodsp_oscillator_state_t *state, float *next,
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

static int16_t AUDIODSP_GRAPH_HOT to_s16(float value) {
    if (value > 32767.0f) {
        return 32767;
    }
    if (value < -32768.0f) {
        return -32768;
    }
    return (int16_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

static float AUDIODSP_GRAPH_HOT mix_waves(const float *values, uint8_t waves) {
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

void AUDIODSP_GRAPH_HOT audiodsp_oscillator_process_s16(
    const audiodsp_oscillator_config_t *config,
    audiodsp_oscillator_state_t *state, int16_t *out, uint32_t frames,
    const int16_t *const inputs[AUDIODSP_OSCILLATOR_INPUTS],
    const uint32_t strides[AUDIODSP_OSCILLATOR_INPUTS]) {
    const int16_t *pitch = inputs[AUDIODSP_OSCILLATOR_IN_PITCH];
    const int16_t *fm = inputs[AUDIODSP_OSCILLATOR_IN_FM];
    const int16_t *pwm = inputs[AUDIODSP_OSCILLATOR_IN_PWM];
    const int16_t *sync = inputs[AUDIODSP_OSCILLATOR_IN_SYNC];
    const uint32_t channels = config->channel_count == 2u ? 2u : 1u;
    const uint8_t waves = config->waves;
    const float gain = config->gain;
    float phase = state->phase;
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
        const float inv_dt = 1.0f / dt;
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
                phase + (1.0f - since) * dt, dt, inv_dt, pw, since);
            sync_reset(state, next, waves, phase, dt, since);
            phase = 0.0f;
            run_segment(state, next, waves, &phase, since * dt, dt, inv_dt,
                pw, 0.0f);
        } else {
            run_segment(state, next, waves, &phase, phase + dt, dt, inv_dt,
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
