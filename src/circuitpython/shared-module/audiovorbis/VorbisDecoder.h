// audiovorbis.VorbisDecoder for CircuitPython. See audiodsp's
// src/audiovorbis/VorbisDecoder.h for the MicroPython twin; the decoding is
// the same shared/audiodsp_vorbis.c (Xiph's Tremor) in both, built into
// CircuitPython by audiodsp's circuitpython.mk. CircuitPython has no Vorbis
// module of its own; this one is PyDevices', shaped like audiomp3.MP3Decoder.
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2019 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/audiocore/__init__.h"

#include "shared/audiodsp_vorbis.h"

// Frames in each of the two output buffers the decoder rotates through.
#define AUDIOVORBIS_FRAMES (1024)

typedef struct {
    audiosample_base_t base;
    audiodsp_vorbis_t *decoder;
    mp_obj_t stream;

    int16_t *pcm_buffer[2];
    uint32_t buffer_frames;
    uint32_t buffer_bytes[2];
    uint8_t buffer_index;
    bool eof;
    bool started;

    int8_t other_channel;
    int8_t other_buffer_index;

    uint32_t samples_decoded;

    mp_obj_t stream_error;
    int stream_errcode;
} audiovorbis_vorbisdecoder_obj_t;

void audiovorbis_vorbisdecoder_reset_buffer(audiovorbis_vorbisdecoder_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audiovorbis_vorbisdecoder_get_buffer(audiovorbis_vorbisdecoder_obj_t *self,
    bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length);
