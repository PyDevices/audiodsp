// audiosampler.Sampler for CircuitPython. See audiodsp's
// src/audiosampler/Sampler.h for the MicroPython twin; the DSP is the same
// shared/audiodsp_sampler.c in both, built into CircuitPython by audiodsp's
// circuitpython.mk.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "py/objtuple.h"
#include "shared-module/audiocore/__init__.h"

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
} audiosampler_samplervoice_obj_t;

void audiosampler_sampler_reset_buffer(audiosampler_sampler_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audiosampler_sampler_get_buffer(
    audiosampler_sampler_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length);
