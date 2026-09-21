// Ported from CircuitPython's shared-bindings+shared-module/audiodelays/
// MultiTapDelay.{h,c} (upstream repo: https://github.com/adafruit/circuitpython,
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

extern const mp_obj_type_t audiodelays_multi_tap_delay_type;

typedef struct {
    audiosample_base_t base;
    uint32_t max_delay_ms;
    mp_float_t delay_ms;
    mp_float_t sample_ms;
    synthio_block_slot_t decay;
    synthio_block_slot_t mix;

    mp_float_t *tap_positions;
    // Always double, not mp_float_t: this is what audiodsp_multitap_process_s16
    // (a runtime-neutral shared/ signature) takes, and mp_float_t is float on
    // an MCU build. Converted once here, when taps are set, so the audio
    // callback in *_get_buffer never pays for a per-block marshal.
    double *tap_levels;
    uint32_t *tap_offsets;
    size_t tap_len;

    int8_t *buffer[2];
    uint8_t last_buf_idx;
    uint32_t buffer_len; // max buffer in bytes

    uint8_t *sample_remaining_buffer;
    uint32_t sample_buffer_length;

    bool loop;
    bool more_data;

    int8_t *delay_buffer;
    uint32_t delay_buffer_len; // bytes
    uint32_t max_delay_buffer_len; // bytes
    uint32_t delay_buffer_pos;
    uint32_t delay_buffer_right_pos;

    mp_obj_t sample;
} audiodelays_multi_tap_delay_obj_t;

void common_hal_audiodelays_multi_tap_delay_construct(audiodelays_multi_tap_delay_obj_t *self, uint32_t max_delay_ms,
    mp_obj_t delay_ms, mp_obj_t decay, mp_obj_t mix, mp_obj_t taps,
    uint32_t buffer_size, uint8_t bits_per_sample, bool samples_signed,
    uint8_t channel_count, uint32_t sample_rate);

void common_hal_audiodelays_multi_tap_delay_deinit(audiodelays_multi_tap_delay_obj_t *self);

mp_float_t common_hal_audiodelays_multi_tap_delay_get_delay_ms(audiodelays_multi_tap_delay_obj_t *self);
void common_hal_audiodelays_multi_tap_delay_set_delay_ms(audiodelays_multi_tap_delay_obj_t *self, mp_obj_t delay_ms);

mp_obj_t common_hal_audiodelays_multi_tap_delay_get_decay(audiodelays_multi_tap_delay_obj_t *self);
void common_hal_audiodelays_multi_tap_delay_set_decay(audiodelays_multi_tap_delay_obj_t *self, mp_obj_t decay);

mp_obj_t common_hal_audiodelays_multi_tap_delay_get_mix(audiodelays_multi_tap_delay_obj_t *self);
void common_hal_audiodelays_multi_tap_delay_set_mix(audiodelays_multi_tap_delay_obj_t *self, mp_obj_t mix);

mp_obj_t common_hal_audiodelays_multi_tap_delay_get_taps(audiodelays_multi_tap_delay_obj_t *self);
void common_hal_audiodelays_multi_tap_delay_set_taps(audiodelays_multi_tap_delay_obj_t *self, mp_obj_t taps);

bool common_hal_audiodelays_multi_tap_delay_get_playing(audiodelays_multi_tap_delay_obj_t *self);
void common_hal_audiodelays_multi_tap_delay_play(audiodelays_multi_tap_delay_obj_t *self, mp_obj_t sample, bool loop);
void common_hal_audiodelays_multi_tap_delay_stop(audiodelays_multi_tap_delay_obj_t *self);

void validate_tap_value(mp_obj_t item, qstr arg_name);
double get_tap_value(mp_obj_t item);
void recalculate_tap_offsets(audiodelays_multi_tap_delay_obj_t *self);

void audiodelays_multi_tap_delay_reset_buffer(audiodelays_multi_tap_delay_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audiodelays_multi_tap_delay_get_buffer(audiodelays_multi_tap_delay_obj_t *self,
    bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length);
