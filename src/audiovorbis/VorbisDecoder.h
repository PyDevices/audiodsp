// audiovorbis.VorbisDecoder: an Ogg Vorbis file as an audio sample.
//
// Modelled on CircuitPython's audiomp3.MP3Decoder (this repo's
// src/audiomp3/MP3Decoder.h, from shared-bindings/audiomp3/MP3Decoder.h and
// shared-module/audiomp3/MP3Decoder.h, https://github.com/adafruit/circuitpython,
// MIT) so that the two are interchangeable. CircuitPython has no Vorbis
// module; this one is PyDevices'. The decoding is shared/audiodsp_vorbis.h.
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2019 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

#include "audiocore/__init__.h"
#include "shared/audiodsp_vorbis.h"

// Frames in each of the two output buffers the decoder rotates through.
#define AUDIOVORBIS_FRAMES (1024)

typedef struct {
    audiosample_base_t base;
    audiodsp_vorbis_t *decoder;
    mp_obj_t stream;

    int16_t *pcm_buffer[2];
    uint32_t buffer_frames;       // what each buffer holds, in frames
    uint32_t buffer_bytes[2];     // what each buffer holds right now
    uint8_t buffer_index;
    bool eof;
    bool started;                 // anything decoded since the last rewind

    int8_t other_channel;
    int8_t other_buffer_index;

    uint32_t samples_decoded;

    // An exception the stream raised inside a read, kept until the decoder
    // is back out of Tremor and the pump lock is not held.
    mp_obj_t stream_error;
    int stream_errcode;
} audiovorbis_vorbisdecoder_obj_t;

extern const mp_obj_type_t audiovorbis_vorbisdecoder_type;

void common_hal_audiovorbis_vorbisdecoder_construct(audiovorbis_vorbisdecoder_obj_t *self,
    mp_obj_t stream, uint8_t *buffer, size_t buffer_size);
void common_hal_audiovorbis_vorbisdecoder_set_file(audiovorbis_vorbisdecoder_obj_t *self,
    mp_obj_t stream);
void common_hal_audiovorbis_vorbisdecoder_deinit(audiovorbis_vorbisdecoder_obj_t *self);
mp_float_t common_hal_audiovorbis_vorbisdecoder_get_rms_level(audiovorbis_vorbisdecoder_obj_t *self);
uint32_t common_hal_audiovorbis_vorbisdecoder_get_samples_decoded(audiovorbis_vorbisdecoder_obj_t *self);

void audiovorbis_vorbisdecoder_reset_buffer(audiovorbis_vorbisdecoder_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audiovorbis_vorbisdecoder_get_buffer(audiovorbis_vorbisdecoder_obj_t *self,
    bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length);
