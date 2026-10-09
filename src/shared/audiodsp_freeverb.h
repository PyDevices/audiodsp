// Runtime-neutral Freeverb processing.
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

#define AUDIODSP_FREEVERB_COMB_SAMPLES 11024
#define AUDIODSP_FREEVERB_ALLPASS_SAMPLES 1563

// Interleaved samples, `channel_count` (1 or 2) per frame. Each channel has a
// bank of its own -- 8 combs and 4 all-passes -- and the state arrays hold
// `channel_count` banks back to back: `comb_buffers` is
// channel_count * AUDIODSP_FREEVERB_COMB_SAMPLES, the indices and filters
// 8 * channel_count, and so on. A call starts on the left bank, as
// CircuitPython's loop does for each run it processes.
void audiodsp_freeverb_process_s16(int16_t *output, const int16_t *input,
    size_t sample_count, uint32_t channel_count, int16_t *comb_buffers,
    uint32_t *comb_indices, int16_t *comb_filters, int16_t *allpass_buffers,
    uint32_t *allpass_indices, double roomsize, double damp, double mix);

void audiodsp_freeverb_process_s16_banks(int16_t *output, const int16_t *input,
    size_t sample_count, uint32_t channel_count,
    int16_t *const comb_buffers[16], const uint16_t comb_sizes[16],
    uint16_t *comb_indices, int16_t *comb_filters,
    int16_t *const allpass_buffers[8], const uint16_t allpass_sizes[8],
    uint16_t *allpass_indices, double roomsize, double damp, double mix);
