// SPDX-License-Identifier: MIT

#include "shared/audioif_fp_contract.h"

#include "shared/audioif_convolve.h"

#include <math.h>

#include "shared/audioif_trig.h"

#define FRAMES AUDIOIF_CONVOLVE_FRAMES
#define FFTN AUDIOIF_CONVOLVE_FFT
#define BINS AUDIOIF_CONVOLVE_BINS
#define SPECTRUM (2u * BINS)

static float clampf(float value, float low, float high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

// exp() for the small negative arguments this file asks for: an envelope step
// per sample and a one-pole coefficient. Both are within a few thousandths of
// zero, where nine terms are exact to float and then some.
//
// Not libm's, for the reason in audioif_trig.h -- these values multiply
// themselves tens of thousands of times down an impulse response, so a
// last-place difference between two libms would not stay in the last place.
static double exp_small(double x) {
    if (x < -1.0) x = -1.0;
    if (x > 0.0) x = 0.0;
    double term = 1.0, sum = 1.0;
    for (int k = 1; k <= 9; k++) {
        term *= x / (double)k;
        sum += term;
    }
    return sum;
}

// xorshift32. The point is not statistical quality -- a reverb tail is
// forgiving -- but that the same seed gives the same room on CPython,
// MicroPython and CircuitPython.
static float noise_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    // Top 24 bits into [-1, 1), which keeps the mantissa exact.
    return (float)((int32_t)(x >> 8) - 8388608) * (1.0f / 8388608.0f);
}

uint32_t audioif_convolve_float_count(const audioif_convolve_config_t *config) {
    uint32_t parts = config->partitions;
    return audioif_rfft_table_floats(FFTN)
        + 2u * FFTN                        // window
        + 2u * FRAMES                      // pending
        + 2u * FRAMES                      // emitted
        + 2u * parts * SPECTRUM            // frequency-delay line
        + config->ir_channels * parts * SPECTRUM
        + SPECTRUM                         // accumulator
        + FFTN                             // scratch
        + FFTN;                            // block
}

void audioif_convolve_config_init(audioif_convolve_config_t *config,
    uint32_t sample_rate, uint32_t partitions, uint32_t ir_channels) {
    config->sample_rate = sample_rate ? sample_rate : 48000u;
    config->channel_count = 2u;
    if (partitions < 1u) partitions = 1u;
    if (partitions > AUDIOIF_CONVOLVE_MAX_PARTITIONS) {
        partitions = AUDIOIF_CONVOLVE_MAX_PARTITIONS;
    }
    config->partitions = partitions;
    config->ir_channels = ir_channels >= 2u ? 2u : 1u;
    config->mix = 1.0f;   // 0.5 asked for, doubled -- full wet beside full dry
}

void audioif_convolve_set_channel_count(audioif_convolve_config_t *config,
    uint32_t channel_count) {
    config->channel_count = channel_count == 1u ? 1u : 2u;
}

void audioif_convolve_configure(audioif_convolve_config_t *config,
    audioif_convolve_option_t option, float value) {
    switch (option) {
        case AUDIOIF_CONVOLVE_OPT_MIX:
            config->mix = clampf(value, 0.0f, 1.0f) * 2.0f;
            break;
        default:
            break;
    }
}

void audioif_convolve_state_init(audioif_convolve_state_t *state,
    const audioif_convolve_config_t *config, float *storage) {
    uint32_t parts = config->partitions;
    float *cursor = storage;

    audioif_rfft_init(&state->rfft, FFTN, cursor);
    cursor += audioif_rfft_table_floats(FFTN);

    state->window = cursor;      cursor += 2u * FFTN;
    state->pending = cursor;     cursor += 2u * FRAMES;
    state->emitted = cursor;     cursor += 2u * FRAMES;
    state->fdl = cursor;         cursor += 2u * parts * SPECTRUM;
    state->impulse = cursor;     cursor += config->ir_channels * parts * SPECTRUM;
    state->accumulator = cursor; cursor += SPECTRUM;
    state->scratch = cursor;     cursor += FFTN;
    state->block = cursor;

    for (uint32_t i = 0; i < config->ir_channels * parts * SPECTRUM; i++) {
        state->impulse[i] = 0.0f;
    }
    state->loaded = 0;
    audioif_convolve_reset(state, config);
}

void audioif_convolve_reset(audioif_convolve_state_t *state,
    const audioif_convolve_config_t *config) {
    for (uint32_t i = 0; i < 2u * FFTN; i++) state->window[i] = 0.0f;
    for (uint32_t i = 0; i < 2u * FRAMES; i++) state->pending[i] = 0.0f;
    for (uint32_t i = 0; i < 2u * FRAMES; i++) state->emitted[i] = 0.0f;
    for (uint32_t i = 0; i < 2u * config->partitions * SPECTRUM; i++) {
        state->fdl[i] = 0.0f;
    }
    state->phase = 0;
    state->cursor = 0;
}

// Transforms one partition's worth of taps sitting in state->block[0..FRAMES)
// into slot `part` of impulse channel `channel`. The upper half of the
// transform's input is the zero padding overlap-save needs.
static void store_partition(audioif_convolve_state_t *state,
    const audioif_convolve_config_t *config, uint32_t channel, uint32_t part) {
    for (uint32_t i = FRAMES; i < FFTN; i++) state->block[i] = 0.0f;
    audioif_rfft_forward(&state->rfft, state->block,
        state->impulse + (channel * config->partitions + part) * SPECTRUM,
        state->scratch);
}

void audioif_convolve_load_s16(audioif_convolve_state_t *state,
    const audioif_convolve_config_t *config, const int16_t *taps,
    uint32_t tap_frames, uint32_t channels, float gain) {
    uint32_t parts = config->partitions;
    uint32_t capacity = parts * FRAMES;
    if (tap_frames > capacity) tap_frames = capacity;
    if (channels < 1u) channels = 1u;
    if (channels > 2u) channels = 2u;

    float scale = gain * (1.0f / 32768.0f);
    for (uint32_t ch = 0; ch < config->ir_channels; ch++) {
        // A mono impulse into a stereo configuration is the same impulse
        // twice; a stereo impulse into a mono configuration keeps the left.
        uint32_t source = channels == 1u ? 0u : (ch < channels ? ch : channels - 1u);
        for (uint32_t part = 0; part < parts; part++) {
            for (uint32_t i = 0; i < FRAMES; i++) {
                uint32_t frame = part * FRAMES + i;
                state->block[i] = frame < tap_frames
                    ? (float)taps[frame * channels + source] * scale : 0.0f;
            }
            store_partition(state, config, ch, part);
        }
    }
    state->loaded = (tap_frames + FRAMES - 1u) / FRAMES;
    if (state->loaded > parts) state->loaded = parts;
    audioif_convolve_reset(state, config);
}

void audioif_convolve_synthesize(audioif_convolve_state_t *state,
    const audioif_convolve_config_t *config, float decay_seconds,
    float damping_hz, float predelay_ms, float diffusion_ms, uint32_t seed) {
    uint32_t parts = config->partitions;
    uint32_t capacity = parts * FRAMES;
    double rate = (double)config->sample_rate;

    decay_seconds = clampf(decay_seconds, 0.05f, 30.0f);
    predelay_ms = clampf(predelay_ms, 0.0f, 200.0f);
    diffusion_ms = clampf(diffusion_ms, 0.0f, 500.0f);

    // -60 dB over decay_seconds, as a per-sample multiplier.
    double envelope_step = exp_small(-3.0 * 2.302585092994046
        / ((double)decay_seconds * rate));
    // A one-pole rolling the tail's top off: a real tail loses its top faster
    // than its level, and a flat-spectrum decay is the giveaway that an
    // impulse was generated rather than measured. Above a quarter of the
    // sample rate a one-pole is not a filter any more, so anything up there
    // means "no damping" rather than a coefficient nobody can use.
    double damping = 1.0;
    if (damping_hz > 0.0f && (double)damping_hz < rate * 0.25) {
        damping = 1.0 - exp_small(-2.0 * AUDIOIF_PI * (double)damping_hz / rate);
    }

    uint32_t predelay = (uint32_t)((double)predelay_ms * 0.001 * rate);
    uint32_t diffusion = (uint32_t)((double)diffusion_ms * 0.001 * rate);
    if (predelay > capacity) predelay = capacity;
    double rise = diffusion ? 1.0 / (double)diffusion : 0.0;

    // Two passes over the same deterministic sequence: the first measures the
    // impulse's energy, the second writes it scaled so that energy is one.
    //
    // Normalizing matters more here than it looks. A tail of unit-amplitude
    // noise convolved with anything is enormous -- 48000 taps near full scale
    // sum to tens of thousands of times the input -- so an unnormalized
    // synthetic room is not a quiet room, it is a clipped one. Unit energy
    // means noise-like material comes back at the level it went in, which is
    // the convention a measured impulse is normalized to as well.
    //
    // Two passes rather than one and a rescale because the partitions are
    // transformed as they are generated and there is nowhere to keep the taps.
    // The second pass costs only the noise, not the transforms.
    double scale = 1.0;
    for (int pass = 0; pass < 2; pass++) {
        double energy = 0.0;
        for (uint32_t ch = 0; ch < config->ir_channels; ch++) {
            // Same envelope, different noise: two channels of one room, not
            // two rooms. A shared seed would make the impulse mono and
            // collapse the stereo image the whole exercise is for.
            uint32_t rng = seed + ch * 0x9e3779b9u;
            if (rng == 0) rng = 1;
            double envelope = 1.0;
            double lowpass = 0.0;
            for (uint32_t part = 0; part < parts; part++) {
                for (uint32_t i = 0; i < FRAMES; i++) {
                    uint32_t frame = part * FRAMES + i;
                    if (frame < predelay) { state->block[i] = 0.0f; continue; }
                    double sample = (double)noise_next(&rng) * envelope;
                    lowpass += damping * (sample - lowpass);
                    double shaped = lowpass;
                    uint32_t since = frame - predelay;
                    if (since < diffusion) {
                        // Linear fade-in. Anything smoother is inaudible
                        // against noise, and the reciprocal is hoisted.
                        shaped *= (double)since * rise;
                    }
                    envelope *= envelope_step;
                    if (pass == 0) {
                        energy += shaped * shaped;
                    } else {
                        state->block[i] = (float)(shaped * scale);
                    }
                }
                if (pass) store_partition(state, config, ch, part);
            }
        }
        if (pass == 0) {
            // Per channel, so a stereo room is not half the level of a mono
            // one -- each side has to stand on its own.
            energy /= (double)config->ir_channels;
            scale = energy > 0.0 ? 1.0 / sqrt(energy) : 0.0;
        }
    }
    state->loaded = parts;
    audioif_convolve_reset(state, config);
}

// One partition's worth of gathered input, per channel, through the
// convolution and out into state->emitted.
static void process_block(const audioif_convolve_config_t *config,
    audioif_convolve_state_t *state) {
    uint32_t parts = config->partitions;
    float mix = config->mix;
    float dry = 2.0f - mix < 1.0f ? 2.0f - mix : 1.0f;
    float wet = mix < 1.0f ? mix : 1.0f;

    state->cursor = state->cursor + 1u < parts ? state->cursor + 1u : 0u;

    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    for (uint32_t ch = 0; ch < channels; ch++) {
        float *window = state->window + ch * FFTN;
        float *pending = state->pending + ch * FRAMES;

        // Overlap-save: the transform sees the previous block and this one,
        // and only the second half of the result is free of the wrap-around.
        for (uint32_t i = 0; i < FRAMES; i++) window[i] = window[i + FRAMES];
        for (uint32_t i = 0; i < FRAMES; i++) window[FRAMES + i] = pending[i];

        float *slot = state->fdl + (ch * parts + state->cursor) * SPECTRUM;
        audioif_rfft_forward(&state->rfft, window, slot, state->scratch);

        for (uint32_t i = 0; i < SPECTRUM; i++) state->accumulator[i] = 0.0f;
        uint32_t ir_channel = config->ir_channels == 2u ? ch : 0u;
        for (uint32_t part = 0; part < state->loaded; part++) {
            uint32_t age = state->cursor >= part
                ? state->cursor - part : state->cursor + parts - part;
            const float *x = state->fdl + (ch * parts + age) * SPECTRUM;
            const float *h = state->impulse
                + (ir_channel * parts + part) * SPECTRUM;
            for (uint32_t bin = 0; bin < BINS; bin++) {
                float xr = x[2 * bin], xi = x[2 * bin + 1];
                float hr = h[2 * bin], hi = h[2 * bin + 1];
                state->accumulator[2 * bin] += xr * hr - xi * hi;
                state->accumulator[2 * bin + 1] += xr * hi + xi * hr;
            }
        }
        audioif_rfft_inverse(&state->rfft, state->accumulator, state->block,
            state->scratch);

        float *emitted = state->emitted + ch * FRAMES;
        for (uint32_t i = 0; i < FRAMES; i++) {
            emitted[i] = dry * pending[i] + wet * state->block[FRAMES + i];
        }
    }
}

void audioif_convolve_process_s16(const audioif_convolve_config_t *config,
    audioif_convolve_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames) {
    if (state->loaded == 0) {
        // Nothing loaded is a bypass, not a room of zero size -- and it has
        // to bypass without the block latency, or a chain built before its
        // impulse arrives would drift against its neighbours.
        const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
        for (uint32_t i = 0; i < frames * channels; i++) out[i] = in[i];
        return;
    }
    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    for (uint32_t frame = 0; frame < frames; frame++) {
        uint32_t phase = state->phase;
        // Read before write: `emitted` holds the previous block's result, so
        // the output trails the input by exactly one partition however the
        // caller chops up its calls.
        for (uint32_t ch = 0; ch < channels; ch++) {
            float value = state->emitted[ch * FRAMES + phase];
            int32_t rounded = (int32_t)(value < 0.0f ? value - 0.5f : value + 0.5f);
            if (rounded > 32767) rounded = 32767;
            else if (rounded < -32768) rounded = -32768;
            state->pending[ch * FRAMES + phase] =
                (float)in[frame * channels + ch];
            out[frame * channels + ch] = (int16_t)rounded;
        }
        state->phase = phase + 1u;
        if (state->phase >= FRAMES) {
            process_block(config, state);
            state->phase = 0;
        }
    }
}
