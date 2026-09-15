// audiomodal.Bank for CircuitPython. See audioif's src/audiomodal/Bank.h for
// the MicroPython twin; the DSP is the same shared/audioif_modal.c in both,
// copied into this tree by audioif/apply_cp_patches.sh.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/audiocore/__init__.h"

#include "shared/audioif_modal.h"

typedef struct {
    audiosample_base_t base;
    mp_obj_t source;
    audioif_modal_config_t config;
    audioif_modal_state_t state;
    // Mode table, coefficients and recursion memory, all allocated by this
    // binding and borrowed by the config and state. The kernel never
    // allocates.
    audioif_modal_mode_t *modes;
    audioif_modal_coeff_t *coeffs;
    float *s1;
    float *s2;
    int16_t buffer[AUDIOIF_MODAL_FRAMES * 2];
    // Source frames fetched but not yet consumed, carried across output
    // blocks.
    const int16_t *pending;
    uint32_t pending_frames;
} audiomodal_bank_obj_t;

void audiomodal_bank_reset_buffer(audiomodal_bank_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audiomodal_bank_get_buffer(
    audiomodal_bank_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length);
