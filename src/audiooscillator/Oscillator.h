// audiooscillator.Oscillator -- MicroPython bindings over the runtime-neutral
// oscillator in shared/audiodsp_oscillator.c.
//
// Not a CircuitPython port: audiodsp adds this one. See
// shared/audiodsp_oscillator.h for what it does that synthio cannot.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "audiocore/__init__.h"
#include "py/obj.h"
#include "shared/audiodsp_oscillator.h"

typedef struct {
    audiosample_base_t base;
    // pitch, fm, pwm and sync, in that order. MP_OBJ_NULL is not connected.
    mp_obj_t input[AUDIODSP_OSCILLATOR_INPUTS];
    // Frames fetched from each input but not yet consumed, carried across
    // output blocks: the inputs hand out different block lengths, so no
    // cursor can be derived from another.
    const int16_t *pending[AUDIODSP_OSCILLATOR_INPUTS];
    uint32_t pending_frames[AUDIODSP_OSCILLATOR_INPUTS];
    uint8_t input_channels[AUDIODSP_OSCILLATOR_INPUTS];
    // Frames per output block: buffer_size's, at most
    // AUDIODSP_OSCILLATOR_FRAMES.
    uint32_t frames;
    audiodsp_oscillator_config_t config;
    audiodsp_oscillator_state_t state;
    int16_t buffer[AUDIODSP_OSCILLATOR_FRAMES * 2];
} audiooscillator_oscillator_obj_t;

extern const mp_obj_type_t audiooscillator_oscillator_type;
