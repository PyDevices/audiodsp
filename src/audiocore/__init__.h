// Ported from CircuitPython's shared-module/audiocore/__init__.h (upstream
// repo: https://github.com/adafruit/circuitpython, MIT), unchanged except
// the proto.h include path (cp_compat/proto.h -- see that file for why the
// named-protocol system needed porting too). Also folds in the handful of
// declarations CP keeps in the separate shared-bindings/audiocore/__init__.h
// (AUDIOSAMPLE_FIELDS and the common property accessors) -- CP's
// shared-bindings/shared-module split doesn't have an equivalent here since
// this port has one C source tree, not one per Python-implementation
// binding target.
//
// The audiosample "pull" protocol: every audio source (RawSample, WaveFile,
// synthio.Synthesizer, Mixer, every effect) implements this, and everything
// downstream (Mixer, effects, outputs) composes purely through it. See
// docs/porting-plan.md, "The audiosample pull protocol is the spine."
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Dan Halbert for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cp_compat/objproperty.h"
#include "cp_compat/proto.h"
#include "py/obj.h"

typedef enum {
    GET_BUFFER_DONE,            // No more data to read
    GET_BUFFER_MORE_DATA,       // More data to read.
    GET_BUFFER_ERROR,           // Error while reading data.
} audioio_get_buffer_result_t;

typedef struct audiosample_base {
    mp_obj_base_t self;
    uint32_t sample_rate;
    uint32_t max_buffer_length;
    uint8_t bits_per_sample;
    uint8_t channel_count;
    uint8_t samples_signed;
    bool single_buffer;
} audiosample_base_t;

typedef void (*audiosample_reset_buffer_fun)(mp_obj_t,
    bool single_channel_output, uint8_t audio_channel);
typedef audioio_get_buffer_result_t (*audiosample_get_buffer_fun)(mp_obj_t,
    bool single_channel_output, uint8_t channel, uint8_t **buffer,
    uint32_t *buffer_length);

//: One of the sources a node pulls from, counting from 0. `mp_const_none`
//: is a slot that exists and is empty; MP_OBJ_NULL is past the last slot,
//: and is the only thing that ends the walk. Deviation from upstream,
//: additive: CircuitPython's protocol has no way to ask a node what is
//: behind it, so a graph can only be inspected from the outside by pulling
//: it. audiodsp#112.
typedef mp_obj_t (*audiosample_sources_fun)(mp_obj_t self, uint8_t index);

typedef struct _audiosample_p_t {
    MP_PROTOCOL_HEAD // MP_QSTR_protocol_audiosample
    audiosample_reset_buffer_fun reset_buffer;
    audiosample_get_buffer_fun get_buffer;
    //: Optional, and NULL means "I cannot tell you" rather than "I have
    //: none" -- a walk stops at a node that does not answer instead of
    //: concluding the graph ends there. Every node in this repository that
    //: holds a source answers; a node from somewhere else need not.
    audiosample_sources_fun sources;
} audiosample_p_t;

//: Walk a graph from `sample` and return the first node whose type name is
//: in `names`, or MP_OBJ_NULL. Depth-first, bounded, and it stops at any
//: node that does not implement `sources` -- so a false answer is always
//: "did not find one", never "found one that is not there".
mp_obj_t audiosample_find_type(mp_obj_t sample, const qstr *names,
    size_t name_count);

static inline uint32_t audiosample_get_bits_per_sample(audiosample_base_t *self) {
    return self->bits_per_sample;
}

static inline uint32_t audiosample_get_sample_rate(audiosample_base_t *self) {
    return self->sample_rate;
}

static inline void audiosample_set_sample_rate(audiosample_base_t *self, uint32_t sample_rate) {
    self->sample_rate = sample_rate;
}

static inline uint8_t audiosample_get_channel_count(audiosample_base_t *self) {
    return self->channel_count;
}

void audiosample_reset_buffer(mp_obj_t sample_obj, bool single_channel_output, uint8_t audio_channel);
audioio_get_buffer_result_t audiosample_get_buffer(mp_obj_t sample_obj,
    bool single_channel_output,
    uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length);

static inline void audiosample_get_buffer_structure(audiosample_base_t *self, bool single_channel_output,
    bool *single_buffer, bool *samples_signed,
    uint32_t *max_buffer_length, uint8_t *spacing) {

    *single_buffer = self->single_buffer;
    *samples_signed = self->samples_signed;
    *max_buffer_length = self->max_buffer_length;

    if (single_channel_output) {
        *spacing = self->channel_count;
    } else {
        *spacing = 1;
    }
}

static inline audiosample_base_t *audiosample_check(mp_obj_t self_in) {
    // called for side effect
    (void)mp_proto_get_or_throw(MP_QSTR_protocol_audiosample, self_in);
    return MP_OBJ_TO_PTR(self_in);
}

static inline void audiosample_get_buffer_structure_checked(mp_obj_t self_in, bool single_channel_output,
    bool *single_buffer, bool *samples_signed,
    uint32_t *max_buffer_length, uint8_t *spacing) {
    audiosample_get_buffer_structure(audiosample_check(self_in), single_channel_output, single_buffer, samples_signed, max_buffer_length, spacing);
}

void audiosample_must_match(audiosample_base_t *self, mp_obj_t other, bool allow_mono_to_stereo);

void audiosample_convert_u8m_s16s(int16_t *buffer_out, const uint8_t *buffer_in, size_t nframes);
void audiosample_convert_u8s_s16s(int16_t *buffer_out, const uint8_t *buffer_in, size_t nframes);
void audiosample_convert_s8m_s16s(int16_t *buffer_out, const int8_t *buffer_in, size_t nframes);
void audiosample_convert_s8s_s16s(int16_t *buffer_out, const int8_t *buffer_in, size_t nframes);
void audiosample_convert_u16m_s16s(int16_t *buffer_out, const uint16_t *buffer_in, size_t nframes);
void audiosample_convert_u16s_s16s(int16_t *buffer_out, const uint16_t *buffer_in, size_t nframes);
void audiosample_convert_s16m_s16s(int16_t *buffer_out, const int16_t *buffer_in, size_t nframes);

void audiosample_convert_u8s_u8m(uint8_t *buffer_out, const uint8_t *buffer_in, size_t nframes);
void audiosample_convert_s8m_u8m(uint8_t *buffer_out, const int8_t *buffer_in, size_t nframes);
void audiosample_convert_s8s_u8m(uint8_t *buffer_out, const int8_t *buffer_in, size_t nframes);
void audiosample_convert_u16m_u8m(uint8_t *buffer_out, const uint16_t *buffer_in, size_t nframes);
void audiosample_convert_u16s_u8m(uint8_t *buffer_out, const uint16_t *buffer_in, size_t nframes);
void audiosample_convert_s16m_u8m(uint8_t *buffer_out, const int16_t *buffer_in, size_t nframes);
void audiosample_convert_s16s_u8m(uint8_t *buffer_out, const int16_t *buffer_in, size_t nframes);

void audiosample_convert_u8m_u8s(uint8_t *buffer_out, const uint8_t *buffer_in, size_t nframes);
void audiosample_convert_s8m_u8s(uint8_t *buffer_out, const int8_t *buffer_in, size_t nframes);
void audiosample_convert_s8s_u8s(uint8_t *buffer_out, const int8_t *buffer_in, size_t nframes);
void audiosample_convert_u16m_u8s(uint8_t *buffer_out, const uint16_t *buffer_in, size_t nframes);
void audiosample_convert_u16s_u8s(uint8_t *buffer_out, const uint16_t *buffer_in, size_t nframes);
void audiosample_convert_s16m_u8s(uint8_t *buffer_out, const int16_t *buffer_in, size_t nframes);
void audiosample_convert_s16s_u8s(uint8_t *buffer_out, const int16_t *buffer_in, size_t nframes);

// --- from CircuitPython's shared-bindings/audiocore/__init__.h ---------

#define AUDIOSAMPLE_FIELDS \
    { MP_ROM_QSTR(MP_QSTR_sample_rate), MP_ROM_PTR(&audiosample_sample_rate_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_bits_per_sample), MP_ROM_PTR(&audiosample_bits_per_sample_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_channel_count), MP_ROM_PTR(&audiosample_channel_count_obj) }

extern const cp_compat_property_t audiosample_sample_rate_obj;
extern const cp_compat_property_t audiosample_bits_per_sample_obj;
extern const cp_compat_property_t audiosample_channel_count_obj;
void audiosample_check_for_deinit(const audiosample_base_t *self);
bool audiosample_deinited(const audiosample_base_t *self);
void audiosample_mark_deinit(audiosample_base_t *self);
