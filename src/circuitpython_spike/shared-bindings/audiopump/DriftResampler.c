// audiopump.DriftResampler bindings for CircuitPython.
//
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices

#include <stdint.h>

#include "shared-bindings/audiopump/DriftResampler.h"
#include "shared-bindings/audiopump/Ring.h"
#include "shared-bindings/audiocore/__init__.h"

#include "py/objproperty.h"
#include "py/runtime.h"
#include "shared/runtime/context_manager_helpers.h"

//| class DriftResampler:
//|     """Plays a `Ring` that is filled on another clock, holding it at a
//|     target fill.
//|
//|     Two devices that both run at 48 kHz differ by tens of parts per
//|     million, so a Ring filled from one and played by the other slowly
//|     empties or overflows. A DriftResampler reads the Ring at a ratio it
//|     steers, in steps far finer than a part per million, so the Ring stays
//|     at ``target`` frames and the output never hears a gap or a skip.
//|
//|     Play the DriftResampler, not the Ring: it is the Ring's only reader::
//|
//|         ring = Ring(sample_rate=48000, channel_count=2, frames=256, capacity=8)
//|         drift = DriftResampler(ring)
//|         out.play(drift)
//|         while True:
//|             ring.write(read_from_the_other_clock())
//|
//|     It plays silence until the Ring first reaches ``target``. If the Ring
//|     runs dry it plays a block of silence, counts an underrun on itself and
//|     on the Ring, and waits for ``target`` again, keeping its rate."""
//|
//|     def __init__(
//|         self,
//|         ring: Ring,
//|         *,
//|         sample_rate: Optional[int] = None,
//|         target: Optional[int] = None,
//|         max_ppm: int = 1000,
//|         bandwidth: float = 0.01,
//|     ) -> None:
//|         """Read ``ring`` and play it at ``sample_rate``.
//|
//|         ``sample_rate`` is the output's rate: the Ring's own (the default)
//|         up to twice it. ``target`` is the fill to hold, in frames, half the
//|         Ring's capacity by default. ``max_ppm`` bounds how far the ratio
//|         moves from nominal, up to 10000. ``bandwidth`` is how fast the loop
//|         follows the clocks once settled, in hertz, from 0.001 to 0.1; it
//|         starts eight times faster and narrows over about a minute.
//|         The Ring's block must be 16 to 1024 frames."""
//|         ...
static mp_obj_t audiopump_driftresampler_make_new(const mp_obj_type_t *type,
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
    audiopump_ring_obj_t *ring = MP_OBJ_TO_PTR(ring_in);
    audiosample_check_for_deinit(&ring->base);
    const uint32_t frame = ring->frame;
    const uint32_t frames = ring->block / frame;
    const uint32_t capacity_frames = ring->cap / frame;
    const uint32_t in_rate = ring->base.sample_rate;
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
    const mp_int_t target = args[ARG_target].u_obj == mp_const_none
        ? (mp_int_t)audiodsp_drift_default_target(least, capacity_frames)
        : mp_obj_get_int(args[ARG_target].u_obj);
    if (target < (mp_int_t)least || target > (mp_int_t)audiodsp_drift_max_target(capacity_frames)) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "target must cover a block and fit in the ring"));
    }

    audiopump_driftresampler_obj_t *self = mp_obj_malloc(
        audiopump_driftresampler_obj_t, &audiopump_driftresampler_type);
    audiopump_driftresampler_construct(self, ring, out_rate, (uint32_t)target,
        (uint32_t)max_ppm, bandwidth);
    return MP_OBJ_FROM_PTR(self);
}

//|     def deinit(self) -> None:
//|         """Mark it deinitialised. An output still playing it stops at its
//|         next pull. The Ring is left as it is."""
//|         ...
static mp_obj_t audiopump_driftresampler_obj_deinit(mp_obj_t self_in) {
    audiopump_driftresampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiopump_driftresampler_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_driftresampler_deinit_obj,
    audiopump_driftresampler_obj_deinit);

//|     def reset(self) -> None:
//|         """Start over: silence until the Ring reaches ``target``, and the
//|         rate estimate forgotten. For when the clock feeding the Ring
//|         changes."""
//|         ...
static mp_obj_t audiopump_driftresampler_obj_reset(mp_obj_t self_in) {
    audiopump_driftresampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    audiodsp_drift_reset(&self->drift);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_driftresampler_reset_obj,
    audiopump_driftresampler_obj_reset);

//|     ppm: float
//|     """How far the ratio is from nominal, in parts per million: positive
//|     when the Ring's clock runs fast against the output's. Read-only."""
static mp_obj_t audiopump_driftresampler_get_ppm(mp_obj_t self_in) {
    audiopump_driftresampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return mp_obj_new_float((mp_float_t)audiodsp_drift_ppm(&self->drift));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiopump_driftresampler_get_ppm_obj,
    audiopump_driftresampler_get_ppm);
MP_PROPERTY_GETTER(audiopump_driftresampler_ppm_obj,
    (mp_obj_t)&audiopump_driftresampler_get_ppm_obj);

//|     fill: float
//|     """The smoothed fill the loop is holding at ``target``, in frames.
//|     Read-only."""
static mp_obj_t audiopump_driftresampler_get_fill(mp_obj_t self_in) {
    audiopump_driftresampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return mp_obj_new_float((mp_float_t)audiodsp_drift_fill(&self->drift));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiopump_driftresampler_get_fill_obj,
    audiopump_driftresampler_get_fill);
MP_PROPERTY_GETTER(audiopump_driftresampler_fill_obj,
    (mp_obj_t)&audiopump_driftresampler_get_fill_obj);

//|     rate: float
//|     """The ratio in use: Ring frames read per frame played. Read-only."""
static mp_obj_t audiopump_driftresampler_get_rate(mp_obj_t self_in) {
    audiopump_driftresampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return mp_obj_new_float((mp_float_t)audiodsp_drift_rate(&self->drift));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiopump_driftresampler_get_rate_obj,
    audiopump_driftresampler_get_rate);
MP_PROPERTY_GETTER(audiopump_driftresampler_rate_obj,
    (mp_obj_t)&audiopump_driftresampler_get_rate_obj);

//|     underruns: int
//|     """Blocks of silence played because the Ring ran dry. Read-only."""
static mp_obj_t audiopump_driftresampler_get_underruns(mp_obj_t self_in) {
    audiopump_driftresampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return mp_obj_new_int_from_uint(self->drift.underruns);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiopump_driftresampler_get_underruns_obj,
    audiopump_driftresampler_get_underruns);
MP_PROPERTY_GETTER(audiopump_driftresampler_underruns_obj,
    (mp_obj_t)&audiopump_driftresampler_get_underruns_obj);

//|     target: int
//|     """The fill to hold, in Ring frames. Setting it moves the fill there
//|     slowly, at the loop's pace, without a jump."""
//|
static mp_obj_t audiopump_driftresampler_get_target(mp_obj_t self_in) {
    audiopump_driftresampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return mp_obj_new_int_from_uint(self->drift.target);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiopump_driftresampler_get_target_obj,
    audiopump_driftresampler_get_target);

static mp_obj_t audiopump_driftresampler_set_target(mp_obj_t self_in,
    mp_obj_t value) {
    audiopump_driftresampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    const mp_int_t target = mp_obj_get_int(value);
    const uint32_t least = audiodsp_drift_min_target(self->drift.frames,
        self->drift.in_rate, self->drift.out_rate, self->drift.max_ppm);
    const uint32_t most = self->ring->cap / self->ring->frame;
    if (target < (mp_int_t)least || target > (mp_int_t)audiodsp_drift_max_target(most)) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "target must cover a block and fit in the ring"));
    }
    self->drift.target = (uint32_t)target;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiopump_driftresampler_set_target_obj,
    audiopump_driftresampler_set_target);
MP_PROPERTY_GETSET(audiopump_driftresampler_target_obj,
    (mp_obj_t)&audiopump_driftresampler_get_target_obj,
    (mp_obj_t)&audiopump_driftresampler_set_target_obj);

static const mp_rom_map_elem_t audiopump_driftresampler_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiopump_driftresampler_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&audiopump_driftresampler_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_ppm), MP_ROM_PTR(&audiopump_driftresampler_ppm_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill), MP_ROM_PTR(&audiopump_driftresampler_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR_rate), MP_ROM_PTR(&audiopump_driftresampler_rate_obj) },
    { MP_ROM_QSTR(MP_QSTR_underruns), MP_ROM_PTR(&audiopump_driftresampler_underruns_obj) },
    { MP_ROM_QSTR(MP_QSTR_target), MP_ROM_PTR(&audiopump_driftresampler_target_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiopump_driftresampler_locals_dict,
    audiopump_driftresampler_locals_dict_table);

static const audiosample_p_t audiopump_driftresampler_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audiopump_driftresampler_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audiopump_driftresampler_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiopump_driftresampler_type,
    MP_QSTR_DriftResampler,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiopump_driftresampler_make_new,
    locals_dict, &audiopump_driftresampler_locals_dict,
    protocol, &audiopump_driftresampler_proto
    );
