// audioverb.Tank -- MicroPython bindings over the runtime-neutral
// reverberation tank in shared/audiodsp_tank.c.
//
// A separate module rather than arguments on `audiofreeverb.Freeverb`,
// deliberately, and for the same reason `audioecho` is not `audiodelays`:
// `Freeverb` is CircuitPython's, and an argument added to audiodsp's copy of it
// would not exist on a stock board -- so a `Plate` written against it would
// silently be a different effect there. A new module either installs whole,
// via apply_cp_patches.sh, or is absent and says so on import.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "audiocore/__init__.h"
#include "py/obj.h"
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

extern const mp_obj_type_t audioverb_tank_type;
