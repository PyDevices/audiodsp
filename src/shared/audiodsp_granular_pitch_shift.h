// Runtime-neutral granular pitch-shift processing.
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIODSP_GRANULAR_MAX_GRAINS (8)
#define AUDIODSP_GRANULAR_PITCH_READ_SHIFT (8)

typedef struct {
    bool active;
    uint32_t read_index;
    uint32_t read_rate;
    uint32_t phase;
    uint32_t length;
} audiodsp_grain_t;

typedef struct {
    audiodsp_grain_t grains[AUDIODSP_GRANULAR_MAX_GRAINS];
    uint32_t write_index;
    uint32_t until_next;
    uint32_t rng_state;
    uint32_t read_rate;
} audiodsp_granular_pitch_shift_state_t;

void audiodsp_granular_pitch_shift_fill_envelope(int16_t *envelope,
    uint32_t grain_size);

void audiodsp_granular_pitch_shift_reset_state(
    audiodsp_granular_pitch_shift_state_t *state);

void audiodsp_granular_pitch_shift_process_s16(int16_t *output,
    const int16_t *input, size_t sample_count, int16_t *capture,
    uint32_t capture_len, const int16_t *envelope, uint8_t channel_count,
    uint32_t grain_size, uint32_t density, double spread, uint32_t grain_gain,
    double mix, audiodsp_granular_pitch_shift_state_t *state);
