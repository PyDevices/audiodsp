// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_granular_pitch_shift.h"
#include "shared/audiodsp_synth_dsp.h"

#include <math.h>
#include <string.h>

#define AUDIODSP_GRANULAR_MIX_SCALE (0xfffffff / (32768 * 2 - 28000))

void audiodsp_granular_pitch_shift_fill_envelope(int16_t *envelope,
    uint32_t grain_size) {
    double denom = (grain_size > 1) ? (double)(grain_size - 1) : 1.0;
    for (uint32_t n = 0; n < grain_size; n++) {
        double w = 0.5 * (1.0 - cos(2.0 * 3.14159265358979323846 *
            (double)n / denom));
        envelope[n] = (int16_t)(w * 32767.0);
    }
}

void audiodsp_granular_pitch_shift_reset_state(
    audiodsp_granular_pitch_shift_state_t *state) {
    memset(state->grains, 0, sizeof(state->grains));
    state->write_index = 0;
    state->until_next = 0;
}

static uint32_t granular_rand(audiodsp_granular_pitch_shift_state_t *state) {
    uint32_t x = state->rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state->rng_state = x;
    return x;
}

static void granular_launch(int16_t *capture, uint32_t capture_len,
    uint32_t grain_size, double spread,
    audiodsp_granular_pitch_shift_state_t *state) {
    (void)capture;
    for (uint32_t g = 0; g < AUDIODSP_GRANULAR_MAX_GRAINS; g++) {
        if (state->grains[g].active) {
            continue;
        }
        uint32_t jitter = 0;
        if (spread > 0.0) {
            uint32_t max_jitter = (uint32_t)(spread * (double)grain_size);
            if (max_jitter > 0) {
                jitter = granular_rand(state) % (max_jitter + 1);
            }
        }
        uint32_t start = (state->write_index + capture_len - grain_size -
            jitter) % capture_len;
        state->grains[g].active = true;
        state->grains[g].read_index = start << AUDIODSP_GRANULAR_PITCH_READ_SHIFT;
        state->grains[g].read_rate = state->read_rate;
        state->grains[g].phase = 0;
        state->grains[g].length = grain_size;
        return;
    }
}

void audiodsp_granular_pitch_shift_process_s16(int16_t *output,
    const int16_t *input, size_t sample_count, int16_t *capture,
    uint32_t capture_len, const int16_t *envelope, uint8_t channel_count,
    uint32_t grain_size, uint32_t density, double spread, uint32_t grain_gain,
    double mix, audiodsp_granular_pitch_shift_state_t *state) {
    uint32_t spacing = grain_size / density;
    if (spacing == 0) {
        spacing = 1;
    }
    double dry = ((2.0 - mix) < 1.0) ? (2.0 - mix) : 1.0;
    double wet = (mix < 1.0) ? mix : 1.0;
    for (size_t index = 0; index < sample_count; index++) {
        int32_t sample = input[index];
        uint8_t buf_offset = (channel_count == 2 && (index % 2) == 1) ? 1 : 0;
        capture[state->write_index + capture_len * buf_offset] = (int16_t)sample;
        int32_t word = 0;
        for (uint32_t g = 0; g < AUDIODSP_GRANULAR_MAX_GRAINS; g++) {
            if (!state->grains[g].active) {
                continue;
            }
            uint32_t ipart = state->grains[g].read_index >>
                AUDIODSP_GRANULAR_PITCH_READ_SHIFT;
            uint32_t frac = state->grains[g].read_index &
                ((1u << AUDIODSP_GRANULAR_PITCH_READ_SHIFT) - 1);
            uint32_t index_lo = ipart % capture_len;
            uint32_t index_hi = (ipart + 1) % capture_len;
            int32_t sample_lo = capture[index_lo + capture_len * buf_offset];
            int32_t sample_hi = capture[index_hi + capture_len * buf_offset];
            int32_t grain_out = sample_lo +
                (((sample_hi - sample_lo) * (int32_t)frac) >>
                    AUDIODSP_GRANULAR_PITCH_READ_SHIFT);
            word += (grain_out * envelope[state->grains[g].phase]) >> 15;
        }
        word = (int32_t)(((int64_t)word * (int64_t)grain_gain) >> 15);
        word = (int32_t)((double)sample * dry + (double)word * wet);
        output[index] = audiodsp_mix_down_sample(word, AUDIODSP_GRANULAR_MIX_SCALE,
            -28000, 28000);
        if (channel_count == 1 || buf_offset) {
            for (uint32_t g = 0; g < AUDIODSP_GRANULAR_MAX_GRAINS; g++) {
                if (!state->grains[g].active) {
                    continue;
                }
                state->grains[g].read_index += state->grains[g].read_rate;
                state->grains[g].phase += 1;
                if (state->grains[g].phase >= state->grains[g].length) {
                    state->grains[g].active = false;
                }
            }
            if (state->until_next == 0) {
                granular_launch(capture, capture_len, grain_size, spread, state);
                state->until_next = spacing;
            } else {
                state->until_next -= 1;
            }
            state->write_index += 1;
            if (state->write_index >= capture_len) {
                state->write_index = 0;
            }
        }
    }
}
