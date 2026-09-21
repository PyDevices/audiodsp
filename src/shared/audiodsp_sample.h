// Runtime-neutral audio sample protocol shared by the MicroPython and
// CPython bindings.
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AUDIODSP_BUFFER_DONE = 0,
    AUDIODSP_BUFFER_MORE_DATA = 1,
    AUDIODSP_BUFFER_ERROR = 2,
} audiodsp_buffer_result_t;

typedef enum {
    AUDIODSP_STATUS_OK = 0,
    AUDIODSP_STATUS_DEINITIALIZED,
    AUDIODSP_STATUS_INVALID_ARGUMENT,
} audiodsp_status_t;

typedef struct {
    uint32_t sample_rate;
    uint32_t max_buffer_length;
    uint8_t bits_per_sample;
    uint8_t channel_count;
    uint8_t samples_signed;
    bool single_buffer;
} audiodsp_sample_info_t;

typedef audiodsp_status_t (*audiodsp_sample_reset_fn)(void *context,
    bool single_channel_output, uint8_t audio_channel);
typedef audiodsp_status_t (*audiodsp_sample_get_fn)(void *context,
    bool single_channel_output, uint8_t audio_channel,
    const uint8_t **buffer, uint32_t *buffer_length,
    audiodsp_buffer_result_t *result);

typedef struct {
    audiodsp_sample_reset_fn reset_buffer;
    audiodsp_sample_get_fn get_buffer;
} audiodsp_sample_ops_t;

typedef struct {
    const audiodsp_sample_ops_t *ops;
    void *context;
    audiodsp_sample_info_t *info;
} audiodsp_sample_source_t;

audiodsp_status_t audiodsp_sample_reset(audiodsp_sample_source_t *source,
    bool single_channel_output, uint8_t audio_channel);
audiodsp_status_t audiodsp_sample_get(audiodsp_sample_source_t *source,
    bool single_channel_output, uint8_t audio_channel,
    const uint8_t **buffer, uint32_t *buffer_length,
    audiodsp_buffer_result_t *result);

