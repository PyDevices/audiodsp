// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// audiopump.DriftResampler -- plays a Ring filled on one clock at another
// clock's rate, steering its ratio to hold the Ring at a target fill.
//
// The DSP and the control loop are shared/audiodsp_drift.c, the same on every
// runtime. This file is the MicroPython half: it reads the Ring directly, at
// whatever pace the ratio asks for, through the Ring's own consumer calls
// (audiopump_ring.h), so it must be the Ring's only consumer.
//
// It lives in audiopump rather than beside audiospeed.Resampler because
// CircuitPython has its own audiospeed, and a module of ours may not shadow
// one of CircuitPython's; audiopump is ours on all three runtimes.

#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "audiocore/__init__.h"
#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/objproperty.h"
#include "shared/audiodsp_drift.h"
#include "shared/audiodsp_hot.h"
#include "shared/audiodsp_pump_lock.h"

#include "audiopump/audiopump_drift.h"
#include "audiopump/audiopump_ring.h"

typedef struct {
    audiosample_base_t base;
    mp_obj_t ring;
    audiodsp_drift_t drift;
    float *work;
    int16_t *scratch;       // the frames one block reads from the ring
    uint32_t scratch_frames;
    int16_t *out[2];
    uint8_t which;
} audiopump_drift_obj_t;

// --- the pull: this runs on the pump thread --------------------------------

static void audiopump_drift_reset_buffer(audiopump_drift_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    // Keeps the loop's state and what is queued, as the Ring does: play()
    // calls this, and a rate estimate is worth keeping across it.
    self->which = 0;
}

static audioio_get_buffer_result_t AUDIODSP_HOT audiopump_drift_get_buffer(
    audiopump_drift_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    void *ring = MP_OBJ_TO_PTR(self->ring);
    const uint32_t frame = self->drift.channels * 2u;
    const uint32_t block = self->drift.frames * frame;
    int16_t *dst = self->out[self->which];
    self->which ^= 1;

    const uint32_t underruns = self->drift.underruns;
    const uint32_t level = audiopump_ring_readable(ring) / frame;
    const uint32_t need = audiodsp_drift_plan(&self->drift, level);
    if (need == 0u) {
        memset(dst, 0, block);
        if (self->drift.underruns != underruns) {
            audiopump_ring_starve(ring, block);
        }
    } else {
        // In pieces the size of the scratch buffer: the block that starts
        // playback also drops what the ring holds beyond the target.
        uint32_t left = need;
        while (left != 0u) {
            const uint32_t n = left < self->scratch_frames ? left : self->scratch_frames;
            audiopump_ring_read(ring, (uint8_t *)self->scratch, n * frame);
            audiodsp_drift_push(&self->drift, self->scratch, n);
            left -= n;
        }
        audiodsp_drift_render(&self->drift, dst);
    }
    *buffer = (uint8_t *)dst;
    *buffer_length = block;
    return GET_BUFFER_MORE_DATA;
}

static mp_obj_t audiopump_drift_sources(mp_obj_t self_in, uint8_t index) {
    audiopump_drift_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return index == 0 ? self->ring : MP_OBJ_NULL;
}

// --- Python ----------------------------------------------------------------

static mp_obj_t audiopump_drift_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_ring, ARG_rate, ARG_target, ARG_max_ppm, ARG_bandwidth };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_ring,        MP_ARG_REQUIRED | MP_ARG_OBJ, {} },
        { MP_QSTR_sample_rate, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_target,      MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_max_ppm,     MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1000} },
        { MP_QSTR_bandwidth,   MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed),
        allowed, args);

    mp_obj_t ring_in = args[ARG_ring].u_obj;
    if (!mp_obj_is_type(ring_in, &audiopump_ring_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("ring must be an audiopump.Ring"));
    }
    audiosample_base_t *ring = MP_OBJ_TO_PTR(ring_in);
    audiosample_check_for_deinit(ring);
    const uint32_t channels = ring->channel_count;
    const uint32_t frame = channels * 2u;
    const uint32_t frames = ring->max_buffer_length / frame;
    const uint32_t capacity_frames = audiopump_ring_capacity(MP_OBJ_TO_PTR(ring_in)) / frame;
    const uint32_t in_rate = ring->sample_rate;
    const uint32_t out_rate = args[ARG_rate].u_obj == mp_const_none
        ? in_rate : (uint32_t)mp_obj_get_int(args[ARG_rate].u_obj);
    const mp_int_t max_ppm = args[ARG_max_ppm].u_int;
    const float bandwidth = args[ARG_bandwidth].u_obj == mp_const_none
        ? 0.01f : (float)mp_obj_get_float(args[ARG_bandwidth].u_obj);

    if (frames < 16u || frames > AUDIODSP_DRIFT_MAX_FRAMES) {
        mp_raise_ValueError(MP_ERROR_TEXT("the ring's frames must be 16 to 1024"));
    }
    if (!audiodsp_drift_rates_ok(in_rate, out_rate)) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "sample_rate must be from the ring's rate to twice it"));
    }
    if (max_ppm < 0 || max_ppm > (mp_int_t)AUDIODSP_DRIFT_MAX_PPM) {
        mp_raise_ValueError(MP_ERROR_TEXT("max_ppm must be from 0 to 10000"));
    }
    if (!(bandwidth >= AUDIODSP_DRIFT_MIN_BANDWIDTH && bandwidth <= AUDIODSP_DRIFT_MAX_BANDWIDTH)) {
        mp_raise_ValueError(MP_ERROR_TEXT("bandwidth must be from 0.001 to 0.1 Hz"));
    }
    const uint32_t least = audiodsp_drift_min_target(frames, in_rate, out_rate,
        (uint32_t)max_ppm);
    mp_int_t target = args[ARG_target].u_obj == mp_const_none
        ? (mp_int_t)audiodsp_drift_default_target(least, capacity_frames)
        : mp_obj_get_int(args[ARG_target].u_obj);
    if (target < (mp_int_t)least || target > (mp_int_t)audiodsp_drift_max_target(capacity_frames)) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "target must cover a block and fit in the ring"));
    }

    audiopump_drift_obj_t *self = mp_obj_malloc(audiopump_drift_obj_t,
        (const mp_obj_type_t *)&audiopump_drift_type);
    self->ring = ring_in;
    const uint32_t work_frames = audiodsp_drift_capacity(frames, in_rate,
        out_rate, (uint32_t)max_ppm);
    self->work = m_new(float, work_frames * channels);
    self->scratch_frames = least;
    self->scratch = m_new(int16_t, least * channels);
    self->out[0] = m_new(int16_t, frames * channels);
    self->out[1] = m_new(int16_t, frames * channels);
    memset(self->out[0], 0, frames * frame);
    memset(self->out[1], 0, frames * frame);
    self->which = 0;
    audiodsp_drift_init(&self->drift, self->work, channels, frames, in_rate,
        out_rate, (uint32_t)target, (uint32_t)max_ppm, bandwidth);

    self->base.sample_rate = out_rate;
    self->base.max_buffer_length = frames * frame;
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)channels;
    self->base.samples_signed = true;
    self->base.single_buffer = false;
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audiopump_drift_deinit(mp_obj_t self_in) {
    audiopump_drift_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_drift_deinit_obj,
    audiopump_drift_deinit);

static mp_obj_t audiopump_drift_reset(mp_obj_t self_in) {
    audiopump_drift_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    audiodsp_pump_lock_acquire();
    audiodsp_drift_reset(&self->drift);
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_drift_reset_obj,
    audiopump_drift_reset);

static mp_obj_t audiopump_drift_get_ppm(mp_obj_t self_in) {
    audiopump_drift_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return mp_obj_new_float((mp_float_t)audiodsp_drift_ppm(&self->drift));
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_drift_get_ppm_obj,
    audiopump_drift_get_ppm);
static MP_PROPERTY_GETTER(audiopump_drift_ppm_obj,
    (mp_obj_t)&audiopump_drift_get_ppm_obj);

static mp_obj_t audiopump_drift_get_fill(mp_obj_t self_in) {
    audiopump_drift_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return mp_obj_new_float((mp_float_t)audiodsp_drift_fill(&self->drift));
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_drift_get_fill_obj,
    audiopump_drift_get_fill);
static MP_PROPERTY_GETTER(audiopump_drift_fill_obj,
    (mp_obj_t)&audiopump_drift_get_fill_obj);

static mp_obj_t audiopump_drift_get_rate(mp_obj_t self_in) {
    audiopump_drift_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return mp_obj_new_float((mp_float_t)audiodsp_drift_rate(&self->drift));
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_drift_get_rate_obj,
    audiopump_drift_get_rate);
static MP_PROPERTY_GETTER(audiopump_drift_rate_obj,
    (mp_obj_t)&audiopump_drift_get_rate_obj);

static mp_obj_t audiopump_drift_get_underruns(mp_obj_t self_in) {
    audiopump_drift_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return mp_obj_new_int_from_uint(self->drift.underruns);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_drift_get_underruns_obj,
    audiopump_drift_get_underruns);
static MP_PROPERTY_GETTER(audiopump_drift_underruns_obj,
    (mp_obj_t)&audiopump_drift_get_underruns_obj);

static mp_obj_t audiopump_drift_get_target(mp_obj_t self_in) {
    audiopump_drift_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return mp_obj_new_int_from_uint(self->drift.target);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_drift_get_target_obj,
    audiopump_drift_get_target);

static mp_obj_t audiopump_drift_set_target(mp_obj_t self_in, mp_obj_t value) {
    audiopump_drift_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    const uint32_t frame = self->drift.channels * 2u;
    const mp_int_t target = mp_obj_get_int(value);
    const uint32_t least = audiodsp_drift_min_target(self->drift.frames,
        self->drift.in_rate, self->drift.out_rate, self->drift.max_ppm);
    const uint32_t most = audiopump_ring_capacity(MP_OBJ_TO_PTR(self->ring)) / frame;
    if (target < (mp_int_t)least || target > (mp_int_t)audiodsp_drift_max_target(most)) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "target must cover a block and fit in the ring"));
    }
    self->drift.target = (uint32_t)target;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiopump_drift_set_target_obj,
    audiopump_drift_set_target);
static MP_PROPERTY_GETSET(audiopump_drift_target_obj,
    (mp_obj_t)&audiopump_drift_get_target_obj,
    (mp_obj_t)&audiopump_drift_set_target_obj);

static const mp_rom_map_elem_t audiopump_driftresampler_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiopump_drift_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&audiopump_drift_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_ppm), MP_ROM_PTR(&audiopump_drift_ppm_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill), MP_ROM_PTR(&audiopump_drift_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR_rate), MP_ROM_PTR(&audiopump_drift_rate_obj) },
    { MP_ROM_QSTR(MP_QSTR_underruns), MP_ROM_PTR(&audiopump_drift_underruns_obj) },
    { MP_ROM_QSTR(MP_QSTR_target), MP_ROM_PTR(&audiopump_drift_target_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiopump_drift_locals,
    audiopump_driftresampler_locals_table);

static const audiosample_p_t audiopump_drift_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audiopump_drift_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audiopump_drift_get_buffer,
    .sources = audiopump_drift_sources,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiopump_drift_type,
    MP_QSTR_DriftResampler,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiopump_drift_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiopump_drift_locals,
    protocol, &audiopump_drift_proto
    );
