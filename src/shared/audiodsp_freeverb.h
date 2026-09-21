// Runtime-neutral Freeverb processing.
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

#define AUDIODSP_FREEVERB_COMB_SAMPLES 11024
#define AUDIODSP_FREEVERB_ALLPASS_SAMPLES 1563

void audiodsp_freeverb_process_s16(int16_t *output, const int16_t *input,
    size_t sample_count, int16_t *comb_buffers, uint32_t *comb_indices,
    int16_t *comb_filters, int16_t *allpass_buffers,
    uint32_t *allpass_indices, double roomsize, double damp, double mix);

void audiodsp_freeverb_process_s16_banks(int16_t *output, const int16_t *input,
    size_t sample_count, int16_t *const comb_buffers[8],
    const uint16_t comb_sizes[8], uint16_t *comb_indices,
    int16_t *comb_filters, int16_t *const allpass_buffers[4],
    const uint16_t allpass_sizes[4], uint16_t *allpass_indices,
    double roomsize, double damp, double mix);
