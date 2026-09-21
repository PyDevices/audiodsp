// audioecho.FeedbackDelay for CircuitPython. See audiodsp's
// src/audioecho/FeedbackDelay.h for the MicroPython twin; the DSP is the same
// shared/audiodsp_feedback_delay.c in both, copied into this tree by
// audiodsp/apply_cp_patches.sh.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/audiocore/__init__.h"

#include "shared/audiodsp_feedback_delay.h"

typedef struct {
    audiosample_base_t base;
    mp_obj_t source;
    // The `wow_shape` table, kept here so the collector can see it: the
    // config borrows the samples and nothing else references the object.
    mp_obj_t wow_shape;
    audiodsp_feedback_delay_config_t config;
    audiodsp_feedback_delay_state_t state;
    int16_t buffer[AUDIODSP_FEEDBACK_DELAY_FRAMES * 2];
    // Source frames fetched but not yet consumed, carried across output
    // blocks.
    const int16_t *pending;
    uint32_t pending_frames;
} audioecho_feedback_delay_obj_t;

void audioecho_feedback_delay_reset_buffer(
    audioecho_feedback_delay_obj_t *self, bool single_channel_output,
    uint8_t channel);
audioio_get_buffer_result_t audioecho_feedback_delay_get_buffer(
    audioecho_feedback_delay_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length);
