// audioshaper.Waveshaper -- MicroPython bindings over the runtime-neutral
// waveshaper in shared/audiodsp_shaper.c.
//
// A separate module rather than arguments on `audiofilters.Distortion`,
// deliberately, and for the same reason `audioecho` is not `audiodelays`:
// `Distortion` is CircuitPython's, and an argument added to audiodsp's copy of
// it would not exist on a stock board -- so an `Overdrive` written against it
// would silently be a different effect there. A new module either installs
// whole, via apply_cp_patches.sh, or is absent and says so on import.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "audiocore/__init__.h"
#include "py/obj.h"
#include "shared/audiodsp_shaper.h"

typedef struct {
    audiosample_base_t base;
    mp_obj_t source;
    audiodsp_shaper_config_t config;
    audiodsp_shaper_state_t state;
    int16_t buffer[AUDIODSP_SHAPER_FRAMES * 2];
    // The curve, copied at construction so the caller may drop the array it
    // built. The config borrows this pointer; the GC keeps it because this
    // object holds it.
    int16_t *curve;
    // Source frames fetched but not yet consumed, carried across output
    // blocks.
    const int16_t *pending;
    uint32_t pending_frames;
} audioshaper_waveshaper_obj_t;

extern const mp_obj_type_t audioshaper_waveshaper_type;
