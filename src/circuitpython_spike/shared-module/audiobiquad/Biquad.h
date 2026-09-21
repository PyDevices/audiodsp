// audiobiquad.Biquad for CircuitPython. See audiodsp's
// src/audiobiquad/Biquad.h for the MicroPython twin; the DSP is the same
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
    synthio_block_slot_t Q;
    synthio_block_slot_t gain_db;
    synthio_block_slot_t mix;
    audiodsp_biquad_f32_config_t config;
    audiodsp_biquad_f32_state_t state;
    int16_t buffer[AUDIODSP_FILTER_F32_FRAMES * 2];
    // Source frames fetched but not yet consumed, carried across output
    // blocks.
    const int16_t *pending;
    uint32_t pending_frames;
} audiobiquad_biquad_obj_t;

// Reads every block input and hands the values to the kernel, which clamps
// them. Shared with the bindings so `coefficients` reports what the next
// block would use without advancing an LFO to find out.
void audiobiquad_biquad_refresh(audiobiquad_biquad_obj_t *self);
void audiobiquad_biquad_reset_buffer(audiobiquad_biquad_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audiobiquad_biquad_get_buffer(
    audiobiquad_biquad_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length);
