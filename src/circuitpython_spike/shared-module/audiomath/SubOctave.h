// audiomath.SubOctave for CircuitPython. See audiodsp's
// src/audiomath/SubOctave.h for the MicroPython twin; the DSP is the same
// shared/audiodsp_suboctave.c in both, copied into this tree by
// audiodsp/apply_cp_patches.sh.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/audiocore/__init__.h"

#include "shared/audiodsp_suboctave.h"

typedef struct {
    audiosample_base_t base;
    // The signal to divide. When it runs dry the node hands out silence, the
    // way every other effect in the palette does. There is no second input:
    // a divider's clock comes from the signal itself, which is the whole
    // difference between it and audiomath.Multiply.
    mp_obj_t source;
    audiodsp_suboctave_config_t config;
    audiodsp_suboctave_state_t state;
    int16_t buffer[AUDIODSP_SUBOCTAVE_FRAMES * 2];
    // Frames fetched from the source but not yet consumed, carried across
    // output blocks: the source hands out its own block length, not ours.
    const int16_t *pending;
    uint32_t pending_frames;
} audiomath_suboctave_obj_t;

void audiomath_suboctave_reset_buffer(audiomath_suboctave_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audiomath_suboctave_get_buffer(
    audiomath_suboctave_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length);
