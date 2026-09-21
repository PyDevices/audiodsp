// audiomath.Multiply -- MicroPython bindings over the runtime-neutral
// multiply in shared/audiodsp_multiply.c.
//
// Not a CircuitPython port and not from vstaudio: audiodsp adds this one. See
// shared/audiodsp_multiply.h for why nothing already in the palette can do it,
// and docs/upstream-diff.md for where it sits among the rest.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "audiocore/__init__.h"
#include "py/obj.h"
#include "shared/audiodsp_multiply.h"

typedef struct {
    audiosample_base_t base;
    // The signal. When it runs dry the node hands out silence, the way every
    // other effect in the palette does.
    mp_obj_t source;
    // What to multiply it by. Pulled the same way, but with the opposite
    // failure: no modulator means the signal passes through untouched.
    mp_obj_t modulator;
    audiodsp_multiply_config_t config;
    int16_t buffer[AUDIODSP_MULTIPLY_FRAMES * 2];
    // Frames fetched from each input but not yet consumed, carried across
    // output blocks. The two inputs hand out different block lengths, so
    // neither cursor can be derived from the other.
    const int16_t *pending_source;
    uint32_t pending_source_frames;
    const int16_t *pending_modulator;
    uint32_t pending_modulator_frames;
} audiomath_multiply_obj_t;

extern const mp_obj_type_t audiomath_multiply_type;
