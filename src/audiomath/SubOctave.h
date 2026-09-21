// audiomath.SubOctave -- MicroPython bindings over the runtime-neutral octave
// divider in shared/audiodsp_suboctave.c.
//
// Not a CircuitPython port and not from vstaudio: audiodsp adds this one. See
// shared/audiodsp_suboctave.h for why nothing already in the palette can do it,
// and docs/upstream-diff.md for where it sits among the rest.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "audiocore/__init__.h"
#include "py/obj.h"
#include "shared/audiodsp_suboctave.h"

typedef struct {
    audiosample_base_t base;
    // The signal to divide. When it runs dry the node hands out silence, the
    // way every other effect in the palette does. There is no second input:
    // a divider's clock comes from the signal itself, which is the whole
    // difference between it and audiomath.Multiply.
    mp_obj_t source;
    audiodsp_suboctave_config_t config;
    audiodsp_suboctave_state_t state;
    int16_t buffer[AUDIODSP_SUBOCTAVE_FRAMES * 2];
    // Frames fetched from the source but not yet consumed, carried across
    // output blocks: the source hands out its own block length, not ours.
    const int16_t *pending;
    uint32_t pending_frames;
} audiomath_suboctave_obj_t;

extern const mp_obj_type_t audiomath_suboctave_type;
