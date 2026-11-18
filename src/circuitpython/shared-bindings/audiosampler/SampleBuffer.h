// audiosampler.SampleBuffer -- bytes for a sampler's voices to read, kept
// outside the collected heap.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "py/obj.h"

typedef struct {
    mp_obj_base_t base;
    uint8_t *data;   // NULL once deinitialised, or for a zero-length buffer
    size_t len;
    bool hooked;     // taken from the port's mem hooks rather than malloc
} audiosampler_samplebuffer_obj_t;

extern const mp_obj_type_t audiosampler_samplebuffer_type;
