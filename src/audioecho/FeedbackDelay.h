// audioecho.FeedbackDelay -- MicroPython bindings over the runtime-neutral
// delay in shared/audiodsp_feedback_delay.c.
//
// A separate module rather than arguments on `audiodelays.Echo`, deliberately.
// `Echo` is CircuitPython's, and an argument added to audiodsp's copy of it
// would not exist on a stock board -- so a `TapeDelay` written against it
// would silently be a different effect there. A new module either installs
// whole, via apply_cp_patches.sh, or is absent and says so on import.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "audiocore/__init__.h"
#include "py/obj.h"
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

extern const mp_obj_type_t audioecho_feedback_delay_type;
