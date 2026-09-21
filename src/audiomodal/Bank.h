// audiomodal.Bank -- MicroPython bindings over the runtime-neutral resonator
// bank in shared/audiodsp_modal.c.
//
// A separate module rather than a mode on `audiobiquad.Biquad`, deliberately,
// and the argument is not the usual one. `audiobiquad` is already audiodsp's
// own, so "an argument here would not exist on a stock board" does not apply.
// The reason is that a bank is not a filter with more coefficients: it sums N
// recursions before the quantiser, it is parameterised by decay time rather
// than Q because a modal decay is a time and because `audiobiquad`'s Q cap of
// 60 is sixty-eight times too low for a cymbal partial, and it skips modes
// that have finished ringing. None of that is a shape a `Biquad` can take
// without becoming a different node with the same name. The name is the room.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "audiocore/__init__.h"
#include "py/obj.h"
#include "shared/audiodsp_modal.h"

typedef struct {
    audiosample_base_t base;
    mp_obj_t source;
    audiodsp_modal_config_t config;
    audiodsp_modal_state_t state;
    // Mode table, coefficients and recursion memory, all allocated by this
    // binding and borrowed by the config and state. The kernel never
    // allocates.
    audiodsp_modal_mode_t *modes;
    audiodsp_modal_coeff_t *coeffs;
    float *s1;
    float *s2;
    int16_t buffer[AUDIODSP_MODAL_FRAMES * 2];
    // Source frames fetched but not yet consumed, carried across output
    // blocks.
    const int16_t *pending;
    uint32_t pending_frames;
} audiomodal_bank_obj_t;

extern const mp_obj_type_t audiomodal_bank_type;
