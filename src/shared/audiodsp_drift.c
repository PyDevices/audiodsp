// The drift resampler's kernel. See audiodsp_drift.h for what it is, and
// docs/how-it-works/drift-resampler.md for the numbers behind each choice.
//
// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_drift.h"
#include "shared/audiodsp_drift_table.h"

#include <string.h>

// Frames of history before the read position, and of look-ahead after it.
#define DRIFT_BEHIND (AUDIODSP_DRIFT_TAPS / 2u - 1u)
#define DRIFT_AHEAD (AUDIODSP_DRIFT_TAPS / 2u)

// The loop's damping, and the factor its starting bandwidth is above its
// final one. The smoothers sit at four times the loop's natural frequency,
// far enough above it to cost little phase and far enough below the block
// rate to take the ring's packet-sized sawtooth out of the measurement.
#define DRIFT_ZETA 0.8f
#define DRIFT_GEAR 7.0f
#define DRIFT_GEAR_SECONDS 15.0f
#define DRIFT_SMOOTH 4.0f
#define DRIFT_TWO_PI 6.28318531f

static uint64_t drift_nominal(uint32_t in_rate, uint32_t out_rate) {
    return (((uint64_t)in_rate << 32) + out_rate / 2u) / out_rate;
}

static uint64_t drift_max_step(uint64_t nominal, uint32_t max_ppm) {
    return nominal + (nominal / 1000000u) * max_ppm + 1u;
}

bool audiodsp_drift_rates_ok(uint32_t in_rate, uint32_t out_rate) {
    if (in_rate == 0u || out_rate == 0u) {
        return false;
    }
    // From 1/2 (doubling the rate) up to 1 (the same rate).
    return in_rate <= out_rate &&
           (uint64_t)in_rate * AUDIODSP_DRIFT_MIN_RATIO_DEN >=
           (uint64_t)out_rate * AUDIODSP_DRIFT_MIN_RATIO_NUM;
}

uint32_t audiodsp_drift_min_target(uint32_t frames, uint32_t in_rate,
    uint32_t out_rate, uint32_t max_ppm) {
    uint64_t step = drift_max_step(drift_nominal(in_rate, out_rate), max_ppm);
    return (uint32_t)((step * frames) >> 32) + 2u;
}

uint32_t audiodsp_drift_capacity(uint32_t frames, uint32_t in_rate,
    uint32_t out_rate, uint32_t max_ppm) {
    return AUDIODSP_DRIFT_TAPS +
           audiodsp_drift_min_target(frames, in_rate, out_rate, max_ppm) + 2u;
}

uint32_t audiodsp_drift_max_target(uint32_t ring_frames) {
    return ring_frames > DRIFT_AHEAD ? ring_frames - DRIFT_AHEAD : 0u;
}

uint32_t audiodsp_drift_default_target(uint32_t least, uint32_t ring_frames) {
    const uint32_t half = ring_frames / 2u;
    return half < least ? least : half;
}

void audiodsp_drift_reset(audiodsp_drift_t *d) {
    d->gear = DRIFT_GEAR;
    d->s1 = (float)d->target;
    d->s2 = (float)d->target;
    d->integ = 0.0f;
    d->dev = 0.0f;
    d->step = d->nominal;
    d->running = false;
    // Silent history behind the first frame, so the filter starts from
    // silence rather than from whatever the buffer held.
    memset(d->work, 0, sizeof(float) * d->capacity * d->channels);
    d->have = DRIFT_BEHIND;
    d->idx = DRIFT_BEHIND;
    d->frac = 0u;
    d->pending = 0u;
    d->skip = 0u;
}

void audiodsp_drift_init(audiodsp_drift_t *d, float *work, uint32_t channels,
    uint32_t frames, uint32_t in_rate, uint32_t out_rate, uint32_t target,
    uint32_t max_ppm, float bandwidth) {
    d->channels = channels;
    d->frames = frames;
    d->in_rate = in_rate;
    d->out_rate = out_rate;
    d->max_ppm = max_ppm;
    d->nominal = drift_nominal(in_rate, out_rate);
    d->max_dev = (float)max_ppm * 1e-6f;
    d->bandwidth = bandwidth;
    d->dt = (float)frames / (float)out_rate;
    // exp(-dt/T) to first order; dt/T is under 1e-3 for any block allowed.
    d->gear_decay = 1.0f - d->dt / DRIFT_GEAR_SECONDS;
    d->target = target;
    d->work = work;
    d->capacity = audiodsp_drift_capacity(frames, in_rate, out_rate, max_ppm);
    d->underruns = 0u;
    audiodsp_drift_reset(d);
}

static float drift_clamp(float v, float limit) {
    if (v > limit) {
        return limit;
    }
    if (v < -limit) {
        return -limit;
    }
    return v;
}

// Frames buffered ahead of the read position: the ring's, plus what the
// kernel has already taken from it beyond the filter's own look-ahead. The
// fractional part keeps the measure smooth as the position creeps.
static float drift_fill(const audiodsp_drift_t *d, uint32_t level) {
    return (float)level + (float)(d->have - d->idx) -
           (float)DRIFT_AHEAD - (float)(d->frac >> 8) * (1.0f / 16777216.0f);
}

static void drift_control(audiodsp_drift_t *d, float fill) {
    const float w = DRIFT_TWO_PI * d->bandwidth * (1.0f + d->gear);
    d->gear *= d->gear_decay;
    float a = DRIFT_SMOOTH * w * d->dt;
    if (a > 1.0f) {
        a = 1.0f;
    }
    d->s1 += a * (fill - d->s1);
    d->s2 += a * (d->s1 - d->s2);
    const float e = d->s2 - (float)d->target;
    // A fill error of e frames changes the fill at -e * kp * out_rate frames
    // a second through the proportional path; with ki the integrator's rate,
    // the loop is e'' + 2 zeta w e' + w^2 e = 0.
    const float rate = (float)d->out_rate;
    const float kp = 2.0f * DRIFT_ZETA * w / rate;
    const float ki = w * w / rate;
    d->integ = drift_clamp(d->integ + ki * e * d->dt, d->max_dev);
    d->dev = drift_clamp(kp * e + d->integ, d->max_dev);
    d->step = d->nominal + (uint64_t)(int64_t)((double)d->dev * (double)d->nominal);
}

static uint32_t drift_need(const audiodsp_drift_t *d) {
    const uint64_t last = (uint64_t)d->frac + (uint64_t)(d->frames - 1u) * d->step;
    const uint32_t idx_last = d->idx + (uint32_t)(last >> 32);
    return idx_last + DRIFT_AHEAD + 1u - d->have;
}

uint32_t audiodsp_drift_plan(audiodsp_drift_t *d, uint32_t level) {
    d->pending = 0u;
    d->skip = 0u;
    if (!d->running) {
        // The fill counts what is beyond the filter's look-ahead, so the
        // ring holds that much more than the target when the fill is there.
        const uint32_t start = d->target + DRIFT_AHEAD;
        if (level < start) {
            return 0u;
        }
        // Start at the target: drop what is beyond it, and start the
        // smoothers there so they do not see a step.
        d->running = true;
        d->skip = level - start;
        level = start;
        d->s1 = d->s2 = drift_fill(d, level);
    }
    drift_control(d, drift_fill(d, level));
    const uint32_t need = drift_need(d);
    if (need > level) {
        // A whole block of silence, and back to waiting for the target: the
        // frames in the ring stay there, and the rate estimate is kept.
        d->underruns++;
        d->running = false;
        d->skip = 0u;
        return 0u;
    }
    d->pending = need + d->skip;
    return d->pending;
}

void audiodsp_drift_push(audiodsp_drift_t *d, const int16_t *in,
    uint32_t frames) {
    if (frames > d->pending) {
        frames = d->pending;
    }
    d->pending -= frames;
    if (d->skip != 0u) {
        const uint32_t dropped = frames < d->skip ? frames : d->skip;
        d->skip -= dropped;
        frames -= dropped;
        in += (size_t)dropped * d->channels;
    }
    float *dst = d->work + (size_t)d->have * d->channels;
    const uint32_t n = frames * d->channels;
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = (float)in[i];
    }
    d->have += frames;
}

static int16_t drift_to_int16(float v) {
    v += v >= 0.0f ? 0.5f : -0.5f;
    if (v >= 32767.0f) {
        return 32767;
    }
    if (v <= -32768.0f) {
        return -32768;
    }
    return (int16_t)(int32_t)v;
}

void audiodsp_drift_render(audiodsp_drift_t *d, int16_t *out) {
    const uint32_t channels = d->channels;
    const uint64_t step = d->step;
    uint32_t idx = d->idx;
    uint32_t frac = d->frac;
    float h[AUDIODSP_DRIFT_TAPS];

    for (uint32_t o = 0; o < d->frames; o++) {
        const uint32_t p = frac >> (32u - AUDIODSP_DRIFT_PHASE_BITS);
        const float a = (float)((frac << AUDIODSP_DRIFT_PHASE_BITS) >> 8) *
            (1.0f / 16777216.0f);
        const float *h0 = audiodsp_drift_table[p];
        const float *h1 = audiodsp_drift_table[p + 1u];
        for (uint32_t k = 0; k < AUDIODSP_DRIFT_TAPS; k++) {
            h[k] = h0[k] + a * (h1[k] - h0[k]);
        }
        const float *x = d->work + (size_t)(idx - DRIFT_BEHIND) * channels;
        if (channels == 2u) {
            float left = 0.0f;
            float right = 0.0f;
            for (uint32_t k = 0; k < AUDIODSP_DRIFT_TAPS; k++) {
                left += h[k] * x[2u * k];
                right += h[k] * x[2u * k + 1u];
            }
            out[2u * o] = drift_to_int16(left);
            out[2u * o + 1u] = drift_to_int16(right);
        } else {
            float acc = 0.0f;
            for (uint32_t k = 0; k < AUDIODSP_DRIFT_TAPS; k++) {
                acc += h[k] * x[k];
            }
            out[o] = drift_to_int16(acc);
        }
        const uint64_t next = (uint64_t)frac + step;
        idx += (uint32_t)(next >> 32);
        frac = (uint32_t)next;
    }

    // Keep the history the next block's first frame needs, at the front.
    const uint32_t drop = idx - DRIFT_BEHIND;
    const uint32_t keep = d->have - drop;
    memmove(d->work, d->work + (size_t)drop * channels,
        sizeof(float) * keep * channels);
    d->have = keep;
    d->idx = idx - drop;
    d->frac = frac;
}

float audiodsp_drift_ppm(const audiodsp_drift_t *d) {
    return d->dev * 1e6f;
}

float audiodsp_drift_fill(const audiodsp_drift_t *d) {
    return d->s2;
}

float audiodsp_drift_rate(const audiodsp_drift_t *d) {
    return (float)((double)d->step * (1.0 / 4294967296.0));
}
