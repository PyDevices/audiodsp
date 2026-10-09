// Ported from CircuitPython's shared-bindings/audiomp3/MP3Decoder.h and
// shared-module/audiomp3/MP3Decoder.h (upstream repo:
// https://github.com/adafruit/circuitpython, MIT), merged into one file --
// this port doesn't keep CP's shared-bindings/shared-module split.
// `supervisor/background_callback.h` -> `cp_compat/background_callback.h`
// (see that header for why it's a stub, not a port); `extmod/vfs_fat.h`
// dropped (only used by CP for a doc-comment type hint, not actually
// referenced by this header's declarations).
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft
// SPDX-FileCopyrightText: Copyright (c) 2019 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

#include "audiocore/__init__.h"
#include "cp_compat/background_callback.h"

typedef struct {
    uint8_t *buf;
    mp_int_t size;
    mp_int_t read_off;
    mp_int_t write_off;
} mp3_input_buffer_t;

typedef struct {
    audiosample_base_t base;
    struct _MP3DecInfo *decoder;
    background_callback_t inbuf_fill_cb;
    mp3_input_buffer_t inbuf;
    int16_t *pcm_buffer[2];
    uint32_t len;

    mp_obj_t stream;

    uint8_t buffer_index;
    bool eof;
    bool block_ok;
    mp_obj_t settimeout_args[3];

    int8_t other_channel;
    int8_t other_buffer_index;

    uint32_t samples_decoded;
} audiomp3_mp3file_obj_t;

extern const mp_obj_type_t audiomp3_mp3file_type;

void common_hal_audiomp3_mp3file_construct(audiomp3_mp3file_obj_t *self,
    mp_obj_t stream, uint8_t *buffer, size_t buffer_size);

void common_hal_audiomp3_mp3file_set_file(audiomp3_mp3file_obj_t *self, mp_obj_t stream);
void common_hal_audiomp3_mp3file_deinit(audiomp3_mp3file_obj_t *self);
mp_float_t common_hal_audiomp3_mp3file_get_rms_level(audiomp3_mp3file_obj_t *self);
uint32_t common_hal_audiomp3_mp3file_get_samples_decoded(audiomp3_mp3file_obj_t *self);

// These are not available from Python because it may be called in an interrupt.
void audiomp3_mp3file_reset_buffer(audiomp3_mp3file_obj_t *self,
    bool single_channel_output,
    uint8_t channel);
audioio_get_buffer_result_t audiomp3_mp3file_get_buffer(audiomp3_mp3file_obj_t *self,
    bool single_channel_output,
    uint8_t channel,
    uint8_t **buffer,
    uint32_t *buffer_length);                                                     // length in bytes
