// audiomath.Multiply for CircuitPython. See audiodsp's src/audiomath/Multiply.h
// for the MicroPython twin; the DSP is the same shared/audiodsp_multiply.c in
// both, copied into this tree by audiodsp/apply_cp_patches.sh.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/audiocore/__init__.h"

#include "shared/audiodsp_multiply.h"

typedef struct {
    audiosample_base_t base;
    // The signal. When it runs dry the node hands out silence, the way every
    // other effect in the palette does.
    mp_obj_t source;
    // What to multiply it by. Pulled the same way, but with the opposite
    // failure: no modulator means the signal passes through untouched.
    mp_obj_t modulator;
    audiodsp_multiply_config_t config;
    int16_t buffer[AUDIODSP_MULTIPLY_FRAMES * 2];
    // Frames fetched from each input but not yet consumed, carried across
    // output blocks. The two inputs hand out different block lengths, so
    // neither cursor can be derived from the other.
    const int16_t *pending_source;
    uint32_t pending_source_frames;
    const int16_t *pending_modulator;
    uint32_t pending_modulator_frames;
} audiomath_multiply_obj_t;

void audiomath_multiply_reset_buffer(audiomath_multiply_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audiomath_multiply_get_buffer(
    audiomath_multiply_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length);
