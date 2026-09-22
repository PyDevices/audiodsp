// audiospeed.Resampler. See Resampler.h for provenance.
// SPDX-License-Identifier: MIT

#include "audiospeed/Resampler.h"

#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/objproperty.h"
#include "cp_compat/util.h"
#include "py/runtime.h"
#include "shared/audiodsp_pump_lock.h"

void audiospeed_resampler_set_sample_rate(audiospeed_resampler_obj_t *self,
    uint32_t sample_rate) {
    self->destination_rate = sample_rate;
    // `speed.base.sample_rate` is the *source's* rate, taken at construction.
    // Playing 16 kHz material into an 8 kHz destination means consuming two
    // source frames per output frame, so the ratio is source over
    // destination -- upstream's `calculate_rate`, and the CPython twin's
    // `_bind_sample_rate`, both read it that way.
    if (self->speed.source != MP_OBJ_NULL && sample_rate != 0) {
        // Rounded, not truncated, for the same reason `rate_to_fp` rounds:
        // 48000/44100 is 71331.9 in Q16 and upstream's cast hands back
        // 71331. docs/upstream-diff.md, audiodsp#92.
        self->speed.rate_fp = (uint32_t)(
            (mp_float_t)self->speed.base.sample_rate / sample_rate *
            (1 << SPEED_SHIFT) + (mp_float_t)0.5);
    } else {
        self->speed.rate_fp = 1 << SPEED_SHIFT;
    }
}

static mp_obj_t audiospeed_resampler_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_source };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_source, MP_ARG_REQUIRED | MP_ARG_OBJ, {} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_obj_t source = args[ARG_source].u_obj;
    audiosample_check(source);

    audiospeed_resampler_obj_t *self = mp_obj_malloc(
        audiospeed_resampler_obj_t, &audiospeed_resampler_type);
    // Ratio 1.0 until something plays it, which is upstream's "default rate
    // 1.0" comment in `common_hal_audiospeed_resampler_construct`.
    common_hal_audiospeed_speedchanger_construct(&self->speed, source,
        1 << SPEED_SHIFT);
    self->destination_rate = 0;
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audiospeed_resampler_deinit(mp_obj_t self_in) {
    // The whole body. mark_deinit is not the damage; the pointer
    // nulling AFTER it is -- the funnel's guard has already let a
    // pull in by then, and the pull writes into a buffer that has
    // just become NULL. Detach under the lock, free afterwards.
    audiodsp_pump_lock_acquire();
    audiospeed_resampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiospeed_speedchanger_deinit(&self->speed);
    self->destination_rate = 0;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiospeed_resampler_deinit_obj,
    audiospeed_resampler_deinit);

// Read-only, unlike SpeedChanger's: the ratio is not the caller's to set, it
// is whatever the destination asked for.
static mp_obj_t audiospeed_resampler_obj_get_rate(mp_obj_t self_in) {
    audiospeed_resampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->speed.base);
    return mp_obj_new_float(
        (mp_float_t)self->speed.rate_fp / (1 << SPEED_SHIFT));
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiospeed_resampler_get_rate_obj,
    audiospeed_resampler_obj_get_rate);
static MP_PROPERTY_GETTER(audiospeed_resampler_rate_obj,
    (mp_obj_t)&audiospeed_resampler_get_rate_obj);

static const mp_rom_map_elem_t audiospeed_resampler_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit),
      MP_ROM_PTR(&audiospeed_resampler_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_rate), MP_ROM_PTR(&audiospeed_resampler_rate_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiospeed_resampler_locals_dict,
    audiospeed_resampler_locals_dict_table);

// The same two functions SpeedChanger uses, reached through the embedded
// SpeedChanger. Nothing about the audio path differs.
static const audiosample_p_t audiospeed_resampler_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)
        audiospeed_speedchanger_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)
        audiospeed_speedchanger_get_buffer,
    // A Resampler's first member IS a SpeedChanger, which is what lets it
    // share the three protocol functions. audiodsp#112.
    .sources = audiospeed_speedchanger_sources,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiospeed_resampler_type,
    MP_QSTR_Resampler,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiospeed_resampler_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiospeed_resampler_locals_dict,
    protocol, &audiospeed_resampler_proto
    );
