// Runtime-neutral pitch-shift processing.
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t window_index;
    uint32_t overlap_index;
    uint32_t read_index;
} audiodsp_pitchshift_positions_t;

void audiodsp_pitchshift_process_s16(int16_t *output, const int16_t *input,
    size_t sample_count, int16_t *window, uint32_t window_samples,
    int16_t *overlap, uint32_t overlap_samples, uint8_t channel_count,
    uint32_t read_rate, double mix, audiodsp_pitchshift_positions_t *positions);
