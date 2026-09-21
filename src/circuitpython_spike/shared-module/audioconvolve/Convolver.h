// audioconvolve.Convolver for CircuitPython. See audiodsp's
// src/audioconvolve/Convolver.h for the MicroPython twin; the DSP is the same
// shared/audiodsp_convolve.c in both, copied into this tree by
// audiodsp/apply_cp_patches.sh.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/audiocore/__init__.h"

#include "shared/audiodsp_convolve.h"

typedef struct {
    audiosample_base_t base;
    mp_obj_t source;
    audiodsp_convolve_config_t config;
    audiodsp_convolve_state_t state;
    // The base of the one allocation. Held as well as pointed into, so the
    // collector has the start of the block to trace from.
    float *storage;
    int16_t buffer[AUDIODSP_CONVOLVE_FRAMES * 2];
    const int16_t *pending;
    uint32_t pending_frames;
} audioconvolve_convolver_obj_t;

void audioconvolve_convolver_reset_buffer(
    audioconvolve_convolver_obj_t *self, bool single_channel_output,
    uint8_t channel);
audioio_get_buffer_result_t audioconvolve_convolver_get_buffer(
    audioconvolve_convolver_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length);
