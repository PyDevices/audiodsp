// audioverb.Tank for CircuitPython. See audiodsp's src/audioverb/Tank.h for the
// MicroPython twin; the DSP is the same shared/audiodsp_tank.c in both, copied
// into this tree by audiodsp/apply_cp_patches.sh.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/audiocore/__init__.h"

#include "shared/audiodsp_tank.h"

typedef struct {
    audiosample_base_t base;
    mp_obj_t source;
    audiodsp_tank_config_t config;
    audiodsp_tank_state_t state;
    int16_t buffer[AUDIODSP_TANK_FRAMES * 2];
    // Source frames fetched but not yet consumed, carried across output
    // blocks.
    const int16_t *pending;
    uint32_t pending_frames;
} audioverb_tank_obj_t;

void audioverb_tank_reset_buffer(audioverb_tank_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audioverb_tank_get_buffer(
    audioverb_tank_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length);
