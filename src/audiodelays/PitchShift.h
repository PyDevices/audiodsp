// Ported from CircuitPython's shared-bindings+shared-module/audiodelays/
// PitchShift.{h,c} (upstream repo: https://github.com/adafruit/circuitpython,
// MIT). Merged into one header/source pair -- see audiomixer/Mixer.h for
// why this port doesn't keep the shared-bindings/shared-module split.
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Cooper Dalrymple
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

#include "audiocore/__init__.h"
#include "synthio/__init__.h"
#include "synthio/block.h"

#define PITCH_READ_SHIFT (8)

extern const mp_obj_type_t audiodelays_pitch_shift_type;

typedef struct {
    audiosample_base_t base;
    synthio_block_slot_t semitones;
    mp_float_t current_semitones;
    synthio_block_slot_t mix;
    bool freeze;
    uint32_t window_len;
    uint32_t overlap_len;

    int8_t *buffer[2];
    uint8_t last_buf_idx;
    uint32_t buffer_len; // max buffer in bytes

    uint8_t *sample_remaining_buffer;
    uint32_t sample_buffer_length;

    bool loop;
    bool more_data;

    int8_t *window_buffer;
    uint32_t window_index; // words

    int8_t *overlap_buffer;
    uint32_t overlap_index; // words

    uint32_t read_index; // words << PITCH_READ_SHIFT
    uint32_t read_rate; // words << PITCH_READ_SHIFT

    mp_obj_t sample;
} audiodelays_pitch_shift_obj_t;

void common_hal_audiodelays_pitch_shift_construct(audiodelays_pitch_shift_obj_t *self,
    mp_obj_t semitones, mp_obj_t mix, uint32_t window, uint32_t overlap,
    uint32_t buffer_size, uint8_t bits_per_sample, bool samples_signed,
    uint8_t channel_count, uint32_t sample_rate);

void common_hal_audiodelays_pitch_shift_deinit(audiodelays_pitch_shift_obj_t *self);

mp_obj_t common_hal_audiodelays_pitch_shift_get_semitones(audiodelays_pitch_shift_obj_t *self);
void common_hal_audiodelays_pitch_shift_set_semitones(audiodelays_pitch_shift_obj_t *self, mp_obj_t semitones);

mp_obj_t common_hal_audiodelays_pitch_shift_get_mix(audiodelays_pitch_shift_obj_t *self);
void common_hal_audiodelays_pitch_shift_set_mix(audiodelays_pitch_shift_obj_t *self, mp_obj_t arg);

bool common_hal_audiodelays_pitch_shift_get_freeze(audiodelays_pitch_shift_obj_t *self);
void common_hal_audiodelays_pitch_shift_set_freeze(audiodelays_pitch_shift_obj_t *self, bool freeze);

bool common_hal_audiodelays_pitch_shift_get_playing(audiodelays_pitch_shift_obj_t *self);
void common_hal_audiodelays_pitch_shift_play(audiodelays_pitch_shift_obj_t *self, mp_obj_t sample, bool loop);
void common_hal_audiodelays_pitch_shift_stop(audiodelays_pitch_shift_obj_t *self);

void recalculate_rate(audiodelays_pitch_shift_obj_t *self, mp_float_t semitones);

void audiodelays_pitch_shift_reset_buffer(audiodelays_pitch_shift_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audiodelays_pitch_shift_get_buffer(audiodelays_pitch_shift_obj_t *self,
    bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length);
