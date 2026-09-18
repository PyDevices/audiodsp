// audioshaper.SampleHold for CircuitPython. See audioif's
// src/audioshaper/SampleHold.h for the MicroPython twin and for why an exact
// rational hold is a node of audioif's own rather than a rate form added to
// `audiospeed.SpeedChanger`; the arithmetic is the same
// shared/audioif_samplehold.c in both, copied into this tree by
// audioif/apply_cp_patches.sh.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/audiocore/__init__.h"

#include "shared/audioif_samplehold.h"

typedef struct {
    audiosample_base_t base;
    mp_obj_t source;
    audioif_samplehold_config_t config;
    audioif_samplehold_state_t state;
    // Bytes in one frame of the source's format, fixed at construction.
    uint8_t frame_bytes;
    // Source frames fetched but not yet consumed, carried across output
    // blocks, and the source's own end-of-stream flags.
    const uint8_t *pending;
    uint32_t pending_frames;
    bool source_done;
    bool source_exhausted;
    uint8_t buffer[AUDIOIF_SAMPLEHOLD_FRAMES *
        AUDIOIF_SAMPLEHOLD_MAX_FRAME_BYTES];
} audioshaper_samplehold_obj_t;

void audioshaper_samplehold_reset_buffer(audioshaper_samplehold_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audioshaper_samplehold_get_buffer(
    audioshaper_samplehold_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length);
