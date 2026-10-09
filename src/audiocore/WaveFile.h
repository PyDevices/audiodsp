// Adapted from CircuitPython's shared-bindings/audiocore/WaveFile.h and
// shared-module/audiocore/WaveFile.h (upstream repo:
// https://github.com/adafruit/circuitpython, MIT). The file is any object
// with the stream protocol, as upstream has it since CircuitPython 11.
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"
#include "py/stream.h"

#include "audiocore/__init__.h"

typedef struct {
    audiosample_base_t base;
    uint8_t *buffer;
    uint32_t buffer_length;
    uint8_t *second_buffer;
    uint32_t second_buffer_length;
    uint32_t file_length; // In bytes
    uint32_t data_start; // Where the data values start (stream offset)
    uint16_t buffer_index;
    uint32_t bytes_remaining;

    uint32_t len;
    mp_obj_t file;

    uint32_t read_count;
    uint32_t left_read_count;
    uint32_t right_read_count;
} audioio_wavefile_obj_t;

extern const mp_obj_type_t audioio_wavefile_type;

void common_hal_audioio_wavefile_construct(audioio_wavefile_obj_t *self,
    mp_obj_t file,
    uint8_t *buffer, size_t buffer_size);

void common_hal_audioio_wavefile_deinit(audioio_wavefile_obj_t *self);

// These are not available from Python because it may be called in an interrupt.
void audioio_wavefile_reset_buffer(audioio_wavefile_obj_t *self,
    bool single_channel_output,
    uint8_t channel);
audioio_get_buffer_result_t audioio_wavefile_get_buffer(audioio_wavefile_obj_t *self,
    bool single_channel_output,
    uint8_t channel,
    uint8_t **buffer,
    uint32_t *buffer_length);                                                     // length in bytes
