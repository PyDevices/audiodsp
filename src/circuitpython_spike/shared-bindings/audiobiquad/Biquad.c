// audiobiquad.Biquad bindings for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "shared-bindings/audiobiquad/Biquad.h"
#include "shared-bindings/audiocore/__init__.h"

#include "py/objproperty.h"
#include "shared/runtime/context_manager_helpers.h"
#include "py/runtime.h"

//| class Biquad:
//|     """One RBJ biquad section with float state, so its tail reaches exact
//|     zero.
//|
//|     `audiofilters.Filter` over `synthio.Biquad` is the integer version of
//|     the same filter, and its recursion has fixed points it can settle on:
//|     fed silence after a burst it can hold a constant of one to a few LSB
//|     for ever. This one cannot, because any state word below 1e-20 is
//|     written as exact zero.
//|
//|     Processes signed 16-bit mono or stereo and hands out 256 frames at a
//|     time. It sits in an audiosample chain like any other effect and never
//|     reports itself finished - a starved chain gets silence."""
//|
//|     def __init__(
//|         self,
//|         *,
//|         mode: int = 0,
//|         frequency: synthio.BlockInput = 1000.0,
//|         Q: synthio.BlockInput = 0.7071067811865475,
//|         gain_db: synthio.BlockInput = 0.0,
//|         mix: synthio.BlockInput = 1.0,
//|         sample_rate: int = 48000,
//|         channel_count: int = 2,
//|     ) -> None:
//|         """Create a biquad. ``mode`` is one of the module's ``LOW_PASS``
//|         .. ``HIGH_SHELF`` constants, numbered exactly as
//|         `synthio.FilterMode` numbers them. ``gain_db`` applies to the
//|         peaking and shelf modes only. ``mix`` crossfades between the
//|         untouched signal and the filtered one, the way
//|         `audiofilters.Filter`'s does."""
//|         ...

static void biquad_set_mode(audiobiquad_biquad_obj_t *self, mp_int_t mode) {
    if (mode < AUDIODSP_BIQUAD_F32_LOW_PASS ||
        mode > AUDIODSP_BIQUAD_F32_HIGH_SHELF) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "mode must be one of audiobiquad's LOW_PASS..HIGH_SHELF"));
    }
    audiodsp_biquad_f32_configure(&self->config,
        AUDIODSP_BIQUAD_F32_OPT_MODE, (float)mode);
}

static mp_obj_t audiobiquad_biquad_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_mode, ARG_frequency, ARG_Q, ARG_gain_db, ARG_mix,
           ARG_sample_rate, ARG_channel_count };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_mode, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_frequency, MP_ARG_OBJ | MP_ARG_KW_ONLY,
          {.u_obj = MP_ROM_INT(1000)} },
        { MP_QSTR_Q, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_NONE} },
        { MP_QSTR_gain_db, MP_ARG_OBJ | MP_ARG_KW_ONLY,
          {.u_obj = MP_ROM_INT(0)} },
        { MP_QSTR_mix, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(1)} },
        { MP_QSTR_sample_rate, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 48000} },
        { MP_QSTR_channel_count, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 2} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t channel_count = mp_arg_validate_int_range(
        args[ARG_channel_count].u_int, 1, 2, MP_QSTR_channel_count);
    mp_arg_validate_int_min(args[ARG_sample_rate].u_int, 1,
        MP_QSTR_sample_rate);

    audiobiquad_biquad_obj_t *self =
        mp_obj_malloc(audiobiquad_biquad_obj_t, &audiobiquad_biquad_type);
    self->base.sample_rate = (uint32_t)args[ARG_sample_rate].u_int;
    self->base.max_buffer_length = sizeof(self->buffer);
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)channel_count;
    self->base.samples_signed = 1;
    self->base.single_buffer = false;
    self->source = MP_OBJ_NULL;
    self->pending = NULL;
    self->pending_frames = 0;

    audiodsp_biquad_f32_config_init(&self->config, self->base.sample_rate,
        (uint32_t)channel_count);
    audiodsp_biquad_f32_state_init(&self->state);
    biquad_set_mode(self, args[ARG_mode].u_int);

    mp_obj_t q = args[ARG_Q].u_obj;
    if (q == mp_const_none) {
        q = mp_obj_new_float(MICROPY_FLOAT_CONST(0.7071067811865475));
    }
    synthio_block_assign_slot(args[ARG_frequency].u_obj, &self->frequency,
        MP_QSTR_frequency);
    synthio_block_assign_slot(q, &self->Q, MP_QSTR_Q);
    synthio_block_assign_slot(args[ARG_gain_db].u_obj, &self->gain_db,
        MP_QSTR_gain_db);
    synthio_block_assign_slot(args[ARG_mix].u_obj, &self->mix, MP_QSTR_mix);
    audiodsp_biquad_f32_config_finish(&self->config);
    return MP_OBJ_FROM_PTR(self);
}

//|     def play(self, sample: circuitpython_typing.AudioSample) -> None:
//|         """Set the source the filter reads from."""
//|         ...
static mp_obj_t audiobiquad_biquad_play(mp_obj_t self_in, mp_obj_t sample) {
    audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in);
    (void)audiosample_check(sample);
    self->source = sample;
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiobiquad_biquad_play_obj,
    audiobiquad_biquad_play);

//|     def stop(self) -> None:
//|         """Let go of the source. The node keeps handing out silence."""
//|         ...
static mp_obj_t audiobiquad_biquad_stop(mp_obj_t self_in) {
    audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->source = MP_OBJ_NULL;
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiobiquad_biquad_stop_obj,
    audiobiquad_biquad_stop);

//|     def clear(self) -> None:
//|         """Zero the recursion."""
//|         ...
static mp_obj_t audiobiquad_biquad_clear(mp_obj_t self_in) {
    audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_biquad_f32_reset(&self->state);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiobiquad_biquad_clear_obj,
    audiobiquad_biquad_clear);

//|     playing: bool
//|     """True while a source is attached."""
static mp_obj_t audiobiquad_biquad_obj_get_playing(mp_obj_t self_in) {
    audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(self->source != MP_OBJ_NULL);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiobiquad_biquad_get_playing_obj,
    audiobiquad_biquad_obj_get_playing);
MP_PROPERTY_GETTER(audiobiquad_biquad_playing_obj,
    (mp_obj_t)&audiobiquad_biquad_get_playing_obj);

//|     mode: int
//|     """The filter shape, ``LOW_PASS`` .. ``HIGH_SHELF``."""
static mp_obj_t audiobiquad_biquad_obj_get_mode(mp_obj_t self_in) {
    audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->config.mode);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiobiquad_biquad_get_mode_obj,
    audiobiquad_biquad_obj_get_mode);

static mp_obj_t audiobiquad_biquad_obj_set_mode(mp_obj_t self_in,
    mp_obj_t value) {
    audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in);
    biquad_set_mode(self, mp_obj_get_int(value));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiobiquad_biquad_set_mode_obj,
    audiobiquad_biquad_obj_set_mode);
MP_PROPERTY_GETSET(audiobiquad_biquad_mode_obj,
    (mp_obj_t)&audiobiquad_biquad_get_mode_obj,
    (mp_obj_t)&audiobiquad_biquad_set_mode_obj);

//|     frequency: synthio.BlockInput
//|     """The corner, centre or shelf frequency."""
//|
//|     Q: synthio.BlockInput
//|     """The section's Q, bounded to 0.05..60 - a pole on the unit circle is
//|     a filter that never stops ringing, which is the thing this class
//|     exists to avoid."""
//|
//|     gain_db: synthio.BlockInput
//|     """Peaking and shelf gain, -60..60 dB. Ignored by the other modes."""
//|
//|     mix: synthio.BlockInput
//|     """0 is a wire, 1 is the filtered signal alone."""
#define BIQUAD_SLOT_PROPERTY(name) \
    static mp_obj_t audiobiquad_biquad_obj_get_##name(mp_obj_t self_in) { \
        audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in); \
        return self->name.obj; \
    } \
    MP_DEFINE_CONST_FUN_OBJ_1(audiobiquad_biquad_get_##name##_obj, \
        audiobiquad_biquad_obj_get_##name); \
    static mp_obj_t audiobiquad_biquad_obj_set_##name(mp_obj_t self_in, \
        mp_obj_t value) { \
        audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in); \
        synthio_block_assign_slot(value, &self->name, MP_QSTR_##name); \
        return mp_const_none; \
    } \
    MP_DEFINE_CONST_FUN_OBJ_2(audiobiquad_biquad_set_##name##_obj, \
        audiobiquad_biquad_obj_set_##name); \
    MP_PROPERTY_GETSET(audiobiquad_biquad_##name##_obj, \
        (mp_obj_t)&audiobiquad_biquad_get_##name##_obj, \
        (mp_obj_t)&audiobiquad_biquad_set_##name##_obj)

BIQUAD_SLOT_PROPERTY(frequency);
BIQUAD_SLOT_PROPERTY(Q);
BIQUAD_SLOT_PROPERTY(gain_db);
BIQUAD_SLOT_PROPERTY(mix);

//|     coefficients: Tuple[float, float, float, float, float]
//|     """``(b0, b1, b2, a1, a2)``, normalized, at the settings in force.
//|
//|     Reads the block inputs but does not advance them, so looking at this
//|     does not move an LFO along."""
//|
//|
static mp_obj_t audiobiquad_biquad_obj_get_coefficients(mp_obj_t self_in) {
    audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiobiquad_biquad_refresh(self);
    mp_obj_t items[5] = {
        mp_obj_new_float((mp_float_t)self->config.b0),
        mp_obj_new_float((mp_float_t)self->config.b1),
        mp_obj_new_float((mp_float_t)self->config.b2),
        mp_obj_new_float((mp_float_t)self->config.a1),
        mp_obj_new_float((mp_float_t)self->config.a2),
    };
    return mp_obj_new_tuple(5, items);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiobiquad_biquad_get_coefficients_obj,
    audiobiquad_biquad_obj_get_coefficients);
MP_PROPERTY_GETTER(audiobiquad_biquad_coefficients_obj,
    (mp_obj_t)&audiobiquad_biquad_get_coefficients_obj);

// `deinit()` releases what this binding holds and marks the node
// deinitialised, so the guarded getters raise afterwards. The MicroPython
// binding of this same type gained it on 2026-09-09 (audiodsp#58, #60, #63) and
// this copy did not, which is audiodsp#75: the two bindings are hand-written and
// nothing held them to each other. Same fields, same order, deliberately.
static mp_obj_t audiobiquad_biquad_deinit(mp_obj_t self_in) {
    audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    self->source = mp_const_none;
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiobiquad_biquad_deinit_obj, audiobiquad_biquad_deinit);

static const mp_rom_map_elem_t audiobiquad_biquad_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiobiquad_biquad_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiobiquad_biquad_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audiobiquad_biquad_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&audiobiquad_biquad_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_playing),
      MP_ROM_PTR(&audiobiquad_biquad_playing_obj) },
    { MP_ROM_QSTR(MP_QSTR_mode), MP_ROM_PTR(&audiobiquad_biquad_mode_obj) },
    { MP_ROM_QSTR(MP_QSTR_frequency),
      MP_ROM_PTR(&audiobiquad_biquad_frequency_obj) },
    { MP_ROM_QSTR(MP_QSTR_Q), MP_ROM_PTR(&audiobiquad_biquad_Q_obj) },
    { MP_ROM_QSTR(MP_QSTR_gain_db),
      MP_ROM_PTR(&audiobiquad_biquad_gain_db_obj) },
    { MP_ROM_QSTR(MP_QSTR_mix), MP_ROM_PTR(&audiobiquad_biquad_mix_obj) },
    { MP_ROM_QSTR(MP_QSTR_coefficients),
      MP_ROM_PTR(&audiobiquad_biquad_coefficients_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiobiquad_biquad_locals_dict,
    audiobiquad_biquad_locals_dict_table);

static const audiosample_p_t audiobiquad_biquad_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)
        audiobiquad_biquad_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)
        audiobiquad_biquad_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiobiquad_biquad_type,
    MP_QSTR_Biquad,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiobiquad_biquad_make_new,
    locals_dict, &audiobiquad_biquad_locals_dict,
    protocol, &audiobiquad_biquad_proto
    );
