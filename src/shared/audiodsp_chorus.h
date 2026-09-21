// Runtime-neutral chorus sample processing.
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

uint32_t audiodsp_chorus_process_s16(int16_t *output,
    const int16_t *input, size_t sample_count, int16_t *delay_buffer,
    uint32_t position, uint32_t delay_samples, uint32_t maximum_samples,
    int32_t voices, double mix);
