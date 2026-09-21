// Runtime-neutral 16-bit echo delay line.
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t left_position;
    uint32_t right_position;
} audiodsp_echo_positions_t;

void audiodsp_echo_process_s16(int16_t *output, const int16_t *input,
    size_t sample_count, int16_t *delay_buffer,
    uint32_t delay_samples_per_channel, uint32_t maximum_samples_per_channel,
    uint32_t rate, double decay, double mix, bool frequency_shift,
    uint8_t channel_count, audiodsp_echo_positions_t *positions);

