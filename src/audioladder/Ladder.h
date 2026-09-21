// audioladder.Ladder -- MicroPython bindings over the runtime-neutral filter
// in shared/audiodsp_ladder.c.
//
// A separate module rather than arguments on `audiofilters.Filter`,
// deliberately. `Filter` is CircuitPython's, and an argument added to
// audiodsp's copy of it would not exist on a stock board -- so a `LadderFilter`
// written against it would silently be a different effect there. A new module
// either installs whole, via apply_cp_patches.sh, or is absent and says so on
// import.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "audiocore/__init__.h"
#include "py/obj.h"
#include "shared/audiodsp_ladder.h"

typedef struct {
    audiosample_base_t base;
    mp_obj_t source;
    audiodsp_ladder_config_t config;
    audiodsp_ladder_state_t state;
    int16_t buffer[AUDIODSP_LADDER_FRAMES * 2];
    // Source frames fetched but not yet consumed, carried across output
    // blocks.
    const int16_t *pending;
    uint32_t pending_frames;
} audioladder_ladder_obj_t;

extern const mp_obj_type_t audioladder_ladder_type;
