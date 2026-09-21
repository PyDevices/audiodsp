// Runtime-neutral flanger sample processing.
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t write_pos[2];
    uint32_t lfo_phase[2];
} audiodsp_flanger_state_t;

uint32_t audiodsp_flanger_ms_to_frames_q16(double ms, uint32_t sample_rate,
    uint32_t frames);

uint32_t audiodsp_flanger_triangle(uint32_t phase);

void audiodsp_flanger_process_s16(int16_t *output, const int16_t *input,
    size_t sample_count, int16_t *delay_line, uint32_t frames,
    uint8_t channel_count, audiodsp_flanger_state_t *state,
    uint32_t delay_min, uint32_t delay_span, uint32_t phase_inc,
    int32_t feedback, int32_t mix, bool invert);
