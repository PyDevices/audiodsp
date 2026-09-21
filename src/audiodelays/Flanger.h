// Ported from CircuitPython's shared-bindings+shared-module/audiodelays/
// Flanger.{h,c} (upstream repo: https://github.com/adafruit/circuitpython,
// MIT). Merged into one header/source pair -- see audiomixer/Mixer.h for
// why this port doesn't keep the shared-bindings/shared-module split.
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Tim Cocks for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

#include "audiocore/__init__.h"
#include "shared/audiodsp_flanger.h"
#include "synthio/block.h"

extern const mp_obj_type_t audiodelays_flanger_type;

typedef struct {
    audiosample_base_t base;
    synthio_block_slot_t min_delay_ms;
    synthio_block_slot_t rate;
    synthio_block_slot_t depth;
    synthio_block_slot_t feedback;
    synthio_block_slot_t mix;

    uint32_t max_delay_ms;
    mp_float_t sample_ms;
    bool invert;

    int8_t *buffer[2];
    uint8_t last_buf_idx;
    uint32_t buffer_len;

    uint8_t *sample_remaining_buffer;
    uint32_t sample_buffer_length;

    bool loop;
    bool more_data;

    int16_t *delay_buffer;
    uint32_t delay_buffer_frames;
    audiodsp_flanger_state_t flanger;

    mp_obj_t sample;
} audiodelays_flanger_obj_t;

void common_hal_audiodelays_flanger_construct(audiodelays_flanger_obj_t *self, uint32_t max_delay_ms,
    mp_obj_t min_delay_ms, mp_obj_t rate, mp_obj_t depth, mp_obj_t feedback, mp_obj_t mix, bool invert,
    uint32_t buffer_size, uint8_t bits_per_sample,
    bool samples_signed, uint8_t channel_count, uint32_t sample_rate);

void common_hal_audiodelays_flanger_deinit(audiodelays_flanger_obj_t *self);
bool common_hal_audiodelays_flanger_deinited(audiodelays_flanger_obj_t *self);

mp_obj_t common_hal_audiodelays_flanger_get_min_delay_ms(audiodelays_flanger_obj_t *self);
void common_hal_audiodelays_flanger_set_min_delay_ms(audiodelays_flanger_obj_t *self, mp_obj_t min_delay_ms);

mp_obj_t common_hal_audiodelays_flanger_get_rate(audiodelays_flanger_obj_t *self);
void common_hal_audiodelays_flanger_set_rate(audiodelays_flanger_obj_t *self, mp_obj_t rate);

mp_obj_t common_hal_audiodelays_flanger_get_depth(audiodelays_flanger_obj_t *self);
void common_hal_audiodelays_flanger_set_depth(audiodelays_flanger_obj_t *self, mp_obj_t depth);

mp_obj_t common_hal_audiodelays_flanger_get_feedback(audiodelays_flanger_obj_t *self);
void common_hal_audiodelays_flanger_set_feedback(audiodelays_flanger_obj_t *self, mp_obj_t feedback);

mp_obj_t common_hal_audiodelays_flanger_get_mix(audiodelays_flanger_obj_t *self);
void common_hal_audiodelays_flanger_set_mix(audiodelays_flanger_obj_t *self, mp_obj_t arg);

mp_float_t common_hal_audiodelays_flanger_get_lfo_value(audiodelays_flanger_obj_t *self);

bool common_hal_audiodelays_flanger_get_invert(audiodelays_flanger_obj_t *self);
void common_hal_audiodelays_flanger_set_invert(audiodelays_flanger_obj_t *self, bool invert);

bool common_hal_audiodelays_flanger_get_playing(audiodelays_flanger_obj_t *self);
void common_hal_audiodelays_flanger_play(audiodelays_flanger_obj_t *self, mp_obj_t sample, bool loop);
void common_hal_audiodelays_flanger_stop(audiodelays_flanger_obj_t *self);

void audiodelays_flanger_reset_buffer(audiodelays_flanger_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audiodelays_flanger_get_buffer(audiodelays_flanger_obj_t *self,
    bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length);
