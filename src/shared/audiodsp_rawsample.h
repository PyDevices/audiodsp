// Runtime-neutral RawSample state and pull implementation.
// SPDX-License-Identifier: MIT

#pragma once

#include "shared/audiodsp_sample.h"

typedef struct {
    audiodsp_sample_info_t *info;
    uint8_t *buffer;
    uint32_t buffer_length;
    uint8_t buffer_index;
    bool deinited;
} audiodsp_rawsample_state_t;

void audiodsp_rawsample_construct(audiodsp_rawsample_state_t *state,
    audiodsp_sample_info_t *info, uint8_t *buffer, uint32_t len,
    uint8_t bytes_per_sample, bool samples_signed, uint8_t channel_count,
    uint32_t sample_rate, bool single_buffer);
void audiodsp_rawsample_deinit(audiodsp_rawsample_state_t *state);
audiodsp_sample_source_t audiodsp_rawsample_source(audiodsp_rawsample_state_t *state);

