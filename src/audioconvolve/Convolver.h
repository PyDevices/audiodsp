// audioconvolve.Convolver -- MicroPython bindings over the runtime-neutral
// partitioned convolution in shared/audiodsp_convolve.c.
//
// Its own module for the same reason audioecho is: a class that only exists
// where audiodsp's build has been applied has to be absent and say so on a
// stock CircuitPython board, rather than quietly being a different effect.
//
// The storage is one block, allocated here and carved up by the DSP layer,
// because how much a convolver needs depends on the impulse it is going to be
// given and the DSP has no allocator. See audiodsp_convolve_float_count().
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "audiocore/__init__.h"
#include "py/obj.h"
#include "shared/audiodsp_convolve.h"

typedef struct {
    audiosample_base_t base;
    mp_obj_t source;
    audiodsp_convolve_config_t config;
    audiodsp_convolve_state_t state;
    // The base of the one allocation. Held as well as pointed into, so the
    // collector has the start of the block to trace from.
    float *storage;
    int16_t buffer[AUDIODSP_CONVOLVE_FRAMES * 2];
    const int16_t *pending;
    uint32_t pending_frames;
} audioconvolve_convolver_obj_t;

extern const mp_obj_type_t audioconvolve_convolver_type;
