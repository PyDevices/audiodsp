// audioroute.MidSide for CircuitPython. See audiodsp's src/audioroute/MidSide.h
// for the MicroPython twin; the DSP is the same shared/audiodsp_midside.c in
// both, copied into this tree by audiodsp/apply_cp_patches.sh.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/audiocore/__init__.h"

#include "shared/audiodsp_midside.h"

typedef struct {
    audiosample_base_t base;
    mp_obj_t source;
    audiodsp_midside_config_t config;
    int16_t buffer[AUDIODSP_MIDSIDE_FRAMES * 2];
    // Source frames fetched but not yet consumed, carried across output
    // blocks.
    const int16_t *pending;
    uint32_t pending_frames;
} audioroute_midside_obj_t;

void audioroute_midside_reset_buffer(audioroute_midside_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audioroute_midside_get_buffer(
    audioroute_midside_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length);
