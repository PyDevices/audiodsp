// Runtime-neutral fixed-point synthesis primitives.
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int16_t audiodsp_sat16(int32_t value, int right_shift);
int16_t audiodsp_mix_down_sample(int32_t sample, int32_t scale,
    int32_t range_low, int32_t range_high);
bool audiodsp_oscillator_fill(int32_t *output, const int16_t *waveform,
    uint32_t waveform_start, uint32_t waveform_end, uint32_t dds_rate,
    uint32_t *accumulator, uint16_t duration, uint8_t frequency_shift);
void audiodsp_assign_loudness(int32_t word, int32_t *last_word,
    int16_t active_loudness[2], const int16_t pending_loudness[2]);
void audiodsp_assign_packed_level(uint32_t word, uint32_t *last_word,
    int32_t *active_lo, int32_t *active_hi,
    int32_t pending_lo, int32_t pending_hi);
void audiodsp_sum_with_loudness(int32_t *output, const int32_t *voice,
    int16_t active_loudness[2], const int16_t pending_loudness[2],
    size_t duration, uint8_t channel_count);
uint32_t audiodsp_pitch_bend(uint32_t frequency_scaled, int32_t bend_value);
