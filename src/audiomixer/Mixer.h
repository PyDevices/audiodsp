// Ported from CircuitPython's shared-bindings/audiomixer/Mixer.h and
// shared-module/audiomixer/Mixer.h (upstream repo:
// https://github.com/adafruit/circuitpython, MIT), merged into one file
// (this port doesn't keep CP's shared-bindings/shared-module split).
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"
#include "py/objtuple.h"

#include "audiocore/__init__.h"

typedef struct {
    audiosample_base_t base;
    uint32_t *first_buffer;
    uint32_t *second_buffer;
    uint32_t len; // in words
    bool use_first_buffer;

    uint32_t read_count;
    uint32_t left_read_count;
    uint32_t right_read_count;

    uint8_t voice_count;
    // True while this mixer is inside its own get_buffer. A graph that leads
    // back into the mixer (Mixer -> Port -> Mixer) is refused at the second
    // entry rather than mixing a voice the outer pull is still using
    // (audiodsp#178).
    bool in_pull;
    mp_obj_tuple_t *voice_tuple;
    mp_obj_t voice[];
} audiomixer_mixer_obj_t;

extern const mp_obj_type_t audiomixer_mixer_type;

void common_hal_audiomixer_mixer_construct(audiomixer_mixer_obj_t *self,
    uint8_t voice_count,
    uint32_t buffer_size,
    uint8_t bits_per_sample,
    bool samples_signed,
    uint8_t channel_count,
    uint32_t sample_rate);

void common_hal_audiomixer_mixer_deinit(audiomixer_mixer_obj_t *self);

bool common_hal_audiomixer_mixer_get_playing(audiomixer_mixer_obj_t *self);

// These are not available from Python because it may be called in an interrupt.
void audiomixer_mixer_reset_buffer(audiomixer_mixer_obj_t *self,
    bool single_channel_output,
    uint8_t channel);
audioio_get_buffer_result_t audiomixer_mixer_get_buffer(audiomixer_mixer_obj_t *self,
    bool single_channel_output,
    uint8_t channel,
    uint8_t **buffer,
    uint32_t *buffer_length);                                                      // length in bytes
