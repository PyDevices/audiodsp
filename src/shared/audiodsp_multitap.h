// Runtime-neutral multi-tap delay processing.
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

// `first_channel` is the lane of `input[0]`: 0, unless an earlier run in the
// same block ended part-way through a frame. Upstream CircuitPython starts
// every run on the left lane, so a stereo source that hands an odd number of
// samples swaps the lanes from there on; this port carries the lane instead
// (a recorded deviation, docs/upstream-diff.md).
uint32_t audiodsp_multitap_process_s16(int16_t *output,
    const int16_t *input, size_t sample_count, int16_t *delay_buffer,
    uint32_t position, uint32_t delay_samples_per_channel,
    uint8_t channel_count, uint8_t first_channel,
    const uint32_t *tap_offsets, const double *tap_levels, size_t tap_count,
    double decay, double mix);
