// audiosampler.Sampler -- MicroPython bindings over the runtime-neutral
// sample player in shared/audiodsp_sampler.c.
//
// A module of its own rather than a mode on `audiomixer.Mixer`, because a
// Mixer's voice plays a *source* at the source's own rate -- it pulls blocks
// through the audiosample protocol and adds them up -- and a sampler's voice
// reads a *buffer* at a pitch, through an interpolator, under an envelope.
// Nothing a Mixer voice does survives a pitch of 1.5: the block it pulled
// would have to be stretched, and the next one would start where the last
// one ended rather than where the sample should be. The engine is the part
// that has to be fast, so it is one node with one C call per block.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "audiocore/__init__.h"
#include "py/obj.h"
#include "py/objtuple.h"
#include "shared/audiodsp_sampler.h"

typedef struct {
    audiosample_base_t base;
    audiodsp_sampler_t sampler;
    // Voice table, mix accumulator and output block, all allocated by this
    // binding and borrowed by the kernel. The kernel never allocates.
    audiodsp_sampler_voice_t *voices;
    float *mix;
    int16_t *buffer;
    mp_obj_tuple_t *voice_tuple;
    mp_obj_t *voice;
} audiosampler_sampler_obj_t;

extern const mp_obj_type_t audiosampler_sampler_type;
