// audiodynamics.Dynamics for CircuitPython. See audiodsp's
// src/audiodynamics/Dynamics.h for the MicroPython twin; the DSP is the same
// shared/audiodsp_dynamics.c in both, copied into this tree by
// audiodsp/apply_cp_patches.sh.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/audiocore/__init__.h"

#include "shared/audiodsp_dynamics.h"

typedef struct {
    audiosample_base_t base;
    mp_obj_t source;
    audiodsp_dynamics_config_t config;
    audiodsp_dynamics_state_t state;
    int16_t buffer[AUDIODSP_DYNAMICS_FRAMES * 2];
    // Source frames fetched but not yet consumed, carried across output blocks.
    const int16_t *pending;
    uint32_t pending_frames;
    // The external key, if `key()` was given one: a second stream the
    // detector reads while the gain still lands on `source`. Pulled in
    // lockstep with the audio, so a block is only ever as long as the shorter
    // of the two has frames for.
    mp_obj_t key_source;
    const int16_t *key_pending;
    uint32_t key_pending_frames;
} audiodynamics_dynamics_obj_t;

void audiodynamics_dynamics_reset_buffer(audiodynamics_dynamics_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audiodynamics_dynamics_get_buffer(
    audiodynamics_dynamics_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length);
