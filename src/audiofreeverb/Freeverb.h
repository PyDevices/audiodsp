// Ported from CircuitPython's shared-bindings+shared-module/audiofreeverb/
// Freeverb.{h,c} (upstream repo: https://github.com/adafruit/circuitpython,
// MIT). Merged into one header/source pair -- see audiomixer/Mixer.h for
// why this port doesn't keep the shared-bindings/shared-module split.
// roomsize/damp/mix are synthio_block_slot_t exactly as upstream (this
// module was never guarded behind CIRCUITPY_SYNTHIO upstream, unlike
// audiomixer -- see docs/how-it-works/upstream-diff.md).
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Mark Komus
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#pragma once

#include "audiocore/__init__.h"
#include "audiofilters/__init__.h"
#include "synthio/__init__.h"
#include "synthio/block.h"

#include "py/obj.h"

extern const mp_obj_type_t audiofreeverb_freeverb_type;

typedef struct {
    audiosample_base_t base;
    synthio_block_slot_t roomsize;
    synthio_block_slot_t damp;
    audiofilters_filter_chain_t pre_filter;
    audiofilters_filter_chain_t post_filter;
    synthio_block_slot_t mix;

    int8_t *buffer[2];
    uint8_t last_buf_idx;
    uint32_t buffer_len; // max buffer in bytes

    uint8_t *sample_remaining_buffer;
    uint32_t sample_buffer_length;

    bool loop;
    bool more_data;

    uint16_t combbuffersizes[16];
    int16_t *combbuffers[16];
    uint16_t combbufferindex[16];
    int16_t combfitlers[16];

    uint16_t allpassbuffersizes[8];
    int16_t *allpassbuffers[8];
    uint16_t allpassbufferindex[8];

    mp_obj_t sample;
} audiofreeverb_freeverb_obj_t;

void common_hal_audiofreeverb_freeverb_construct(audiofreeverb_freeverb_obj_t *self,
    mp_obj_t roomsize, mp_obj_t damp, mp_obj_t pre_filter, mp_obj_t post_filter, mp_obj_t mix,
    uint32_t buffer_size, uint8_t bits_per_sample, bool samples_signed,
    uint8_t channel_count, uint32_t sample_rate);

void common_hal_audiofreeverb_freeverb_deinit(audiofreeverb_freeverb_obj_t *self);
bool common_hal_audiofreeverb_freeverb_deinited(audiofreeverb_freeverb_obj_t *self);

mp_obj_t common_hal_audiofreeverb_freeverb_get_roomsize(audiofreeverb_freeverb_obj_t *self);
void common_hal_audiofreeverb_freeverb_set_roomsize(audiofreeverb_freeverb_obj_t *self, mp_obj_t roomsize);

mp_obj_t common_hal_audiofreeverb_freeverb_get_damp(audiofreeverb_freeverb_obj_t *self);
void common_hal_audiofreeverb_freeverb_set_damp(audiofreeverb_freeverb_obj_t *self, mp_obj_t damp);

mp_obj_t common_hal_audiofreeverb_freeverb_get_pre_filter(audiofreeverb_freeverb_obj_t *self);
void common_hal_audiofreeverb_freeverb_set_pre_filter(audiofreeverb_freeverb_obj_t *self, mp_obj_t arg);

mp_obj_t common_hal_audiofreeverb_freeverb_get_post_filter(audiofreeverb_freeverb_obj_t *self);
void common_hal_audiofreeverb_freeverb_set_post_filter(audiofreeverb_freeverb_obj_t *self, mp_obj_t arg);

mp_obj_t common_hal_audiofreeverb_freeverb_get_mix(audiofreeverb_freeverb_obj_t *self);
void common_hal_audiofreeverb_freeverb_set_mix(audiofreeverb_freeverb_obj_t *self, mp_obj_t mix);

bool common_hal_audiofreeverb_freeverb_get_playing(audiofreeverb_freeverb_obj_t *self);
void common_hal_audiofreeverb_freeverb_play(audiofreeverb_freeverb_obj_t *self, mp_obj_t sample, bool loop);
void common_hal_audiofreeverb_freeverb_stop(audiofreeverb_freeverb_obj_t *self);

void audiofreeverb_freeverb_reset_buffer(audiofreeverb_freeverb_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audiofreeverb_freeverb_get_buffer(audiofreeverb_freeverb_obj_t *self,
    bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length);

int16_t audiofreeverb_freeverb_get_roomsize_fixedpoint(mp_float_t n);
void audiofreeverb_freeverb_get_damp_fixedpoint(mp_float_t n, int16_t *damp1, int16_t *damp2);
void audiofreeverb_freeverb_get_mix_fixedpoint(mp_float_t mix, int16_t *mix_sample, int16_t *mix_effect);
