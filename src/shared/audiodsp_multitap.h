// Runtime-neutral multi-tap delay processing.
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

uint32_t audiodsp_multitap_process_s16(int16_t *output,
    const int16_t *input, size_t sample_count, int16_t *delay_buffer,
    uint32_t position, uint32_t delay_samples_per_channel,
    uint8_t channel_count, const uint32_t *tap_offsets,
    const double *tap_levels, size_t tap_count, double decay, double mix);
