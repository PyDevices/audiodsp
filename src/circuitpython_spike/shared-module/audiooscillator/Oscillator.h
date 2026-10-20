// audiooscillator.Oscillator for CircuitPython. See audiodsp's
// src/audiooscillator/Oscillator.h for the MicroPython twin; the DSP is the
// same shared/audiodsp_oscillator.c in both, built into CircuitPython by
// audiodsp's circuitpython.mk.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/audiocore/__init__.h"

#include "shared/audiodsp_oscillator.h"

typedef struct {
    audiosample_base_t base;
    // pitch, fm, pwm and sync, in that order. MP_OBJ_NULL is not connected.
    mp_obj_t input[AUDIODSP_OSCILLATOR_INPUTS];
    const int16_t *pending[AUDIODSP_OSCILLATOR_INPUTS];
    uint32_t pending_frames[AUDIODSP_OSCILLATOR_INPUTS];
    uint8_t input_channels[AUDIODSP_OSCILLATOR_INPUTS];
    uint32_t frames;
    audiodsp_oscillator_config_t config;
    audiodsp_oscillator_state_t state;
    int16_t buffer[AUDIODSP_OSCILLATOR_FRAMES * 2];
} audiooscillator_oscillator_obj_t;

void audiooscillator_oscillator_reset_buffer(
    audiooscillator_oscillator_obj_t *self, bool single_channel_output,
    uint8_t channel);
audioio_get_buffer_result_t audiooscillator_oscillator_get_buffer(
    audiooscillator_oscillator_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length);
