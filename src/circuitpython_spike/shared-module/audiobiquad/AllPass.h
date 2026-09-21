// audiobiquad.AllPass for CircuitPython. See audiodsp's
// src/audiobiquad/AllPass.h for the MicroPython twin; the DSP is the same
// shared/audiodsp_filter_f32.c in both, copied into this tree by
// audiodsp/apply_cp_patches.sh.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/audiocore/__init__.h"
#include "shared-module/synthio/__init__.h"
#include "shared-module/synthio/block.h"

#include "shared/audiodsp_filter_f32.h"

typedef struct {
    audiosample_base_t base;
    mp_obj_t source;
    synthio_block_slot_t frequency;
    synthio_block_slot_t feedback;
    synthio_block_slot_t mix;
    audiodsp_allpass_f32_config_t config;
    audiodsp_allpass_f32_state_t state;
    int16_t buffer[AUDIODSP_FILTER_F32_FRAMES * 2];
    const int16_t *pending;
    uint32_t pending_frames;
} audiobiquad_allpass_obj_t;

// See Biquad.h: reads the block inputs without advancing them.
void audiobiquad_allpass_refresh(audiobiquad_allpass_obj_t *self);
void audiobiquad_allpass_reset_buffer(audiobiquad_allpass_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audiobiquad_allpass_get_buffer(
    audiobiquad_allpass_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length);
