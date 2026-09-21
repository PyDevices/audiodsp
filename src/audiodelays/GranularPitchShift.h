// Ported from CircuitPython's shared-bindings+shared-module/audiodelays/
// GranularPitchShift.{h,c} (upstream repo: https://github.com/adafruit/circuitpython,
// MIT). Merged into one header/source pair -- see audiomixer/Mixer.h for
// why this port doesn't keep the shared-bindings/shared-module split.
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Tim Cocks for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

#include "audiocore/__init__.h"
#include "shared/audiodsp_granular_pitch_shift.h"
#include "synthio/block.h"

extern const mp_obj_type_t audiodelays_granular_pitch_shift_type;

typedef struct {
    audiosample_base_t base;
    synthio_block_slot_t semitones;
    mp_float_t current_semitones;
    synthio_block_slot_t mix;

    int8_t *buffer[2];
    uint8_t last_buf_idx;
    uint32_t buffer_len;

    uint8_t *sample_remaining_buffer;
    uint32_t sample_buffer_length;

    bool loop;
    bool more_data;

    int16_t *capture_buffer;
    uint32_t capture_len;
    int16_t *envelope_table;
    uint32_t grain_size;
    uint32_t density;
    mp_float_t spread;
    uint32_t grain_gain;
    audiodsp_granular_pitch_shift_state_t granular;

    mp_obj_t sample;
} audiodelays_granular_pitch_shift_obj_t;

void common_hal_audiodelays_granular_pitch_shift_construct(
    audiodelays_granular_pitch_shift_obj_t *self,
    mp_obj_t semitones, mp_obj_t mix, uint32_t grain_size, uint32_t density,
    mp_float_t spread, uint32_t buffer_size, uint8_t bits_per_sample,
    bool samples_signed, uint8_t channel_count, uint32_t sample_rate);

void common_hal_audiodelays_granular_pitch_shift_deinit(
    audiodelays_granular_pitch_shift_obj_t *self);

mp_obj_t common_hal_audiodelays_granular_pitch_shift_get_semitones(
    audiodelays_granular_pitch_shift_obj_t *self);
void common_hal_audiodelays_granular_pitch_shift_set_semitones(
    audiodelays_granular_pitch_shift_obj_t *self, mp_obj_t semitones);

mp_obj_t common_hal_audiodelays_granular_pitch_shift_get_mix(
    audiodelays_granular_pitch_shift_obj_t *self);
void common_hal_audiodelays_granular_pitch_shift_set_mix(
    audiodelays_granular_pitch_shift_obj_t *self, mp_obj_t arg);

mp_float_t common_hal_audiodelays_granular_pitch_shift_get_spread(
    audiodelays_granular_pitch_shift_obj_t *self);
void common_hal_audiodelays_granular_pitch_shift_set_spread(
    audiodelays_granular_pitch_shift_obj_t *self, mp_float_t spread);

bool common_hal_audiodelays_granular_pitch_shift_get_playing(
    audiodelays_granular_pitch_shift_obj_t *self);
void common_hal_audiodelays_granular_pitch_shift_play(
    audiodelays_granular_pitch_shift_obj_t *self, mp_obj_t sample, bool loop);
void common_hal_audiodelays_granular_pitch_shift_stop(
    audiodelays_granular_pitch_shift_obj_t *self);

void granular_pitch_shift_recalculate_rate(
    audiodelays_granular_pitch_shift_obj_t *self, mp_float_t semitones);

void audiodelays_granular_pitch_shift_reset_buffer(
    audiodelays_granular_pitch_shift_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audiodelays_granular_pitch_shift_get_buffer(
    audiodelays_granular_pitch_shift_obj_t *self,
    bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length);
