// audiosampler.SamplerVoice -- one of a Sampler's voices. Made by the
// Sampler, never directly: `audiosampler.SamplerVoice()` has no constructor.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "audiosampler/Sampler.h"
#include "py/obj.h"

typedef struct {
    mp_obj_base_t base;
    audiosampler_sampler_obj_t *parent;
    uint32_t index;
    // The buffer object the kernel is reading, held here so the GC keeps it
    // for as long as the voice sounds. Cleared when the voice stops, whether
    // Python stopped it or the sample ran out.
    mp_obj_t sample;
    // The buffer the voice's tail is reading while it fades out a note that
    // stop(), or a play() over it, took off the voice. Cleared once the fade
    // ends.
    mp_obj_t tail_sample;
    // What Python last asked for, handed back by the getters: the kernel
    // keeps the clamped float32 and these keep the caller's number.
    mp_obj_t level;
    mp_obj_t pan;
    mp_obj_t pitch;
    mp_obj_t cutoff;
    mp_obj_t vibrato_depth;
    // The voice's and its tail's filter, envelope and LFO state, allocated
    // the first time a note or a live change asks for any of them, and held
    // here as well as in the voice table so the GC keeps it.
    audiodsp_sampler_mod_t *mod;
} audiosampler_samplervoice_obj_t;

extern const mp_obj_type_t audiosampler_samplervoice_type;

mp_obj_t audiosampler_samplervoice_new(audiosampler_sampler_obj_t *parent,
    uint32_t index);
