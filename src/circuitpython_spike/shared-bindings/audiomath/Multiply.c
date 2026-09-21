// audiomath.Multiply bindings for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "shared-bindings/audiomath/Multiply.h"
#include "shared-bindings/audiocore/__init__.h"

#include "py/objproperty.h"
#include "shared/runtime/context_manager_helpers.h"
#include "py/runtime.h"

//| class Multiply:
//|     """Two audio streams multiplied together: ring modulation, and with a
//|     modulator that does not cross zero, amplitude modulation.
//|
//|     Processes signed 16-bit stereo, and hands out 256 frames at a time. It
//|     sits in an audiosample chain like any other effect, and never reports
//|     itself finished - a starved chain gets silence.
//|
//|     The two inputs fail in opposite directions, deliberately. A source that
//|     runs dry gives silence; a modulator that is absent or has stopped lets
//|     the signal through untouched, because multiplying by nothing and
//|     multiplying by zero must not mean the same thing."""
//|
//|     def __init__(
//|         self,
//|         source: Optional[circuitpython_typing.AudioSample] = None,
//|         modulator: Optional[circuitpython_typing.AudioSample] = None,
//|         mix: float = 1.0,
//|         sample_rate: int = 48000,
//|     ) -> None:
//|         """Multiply ``source`` by ``modulator``.
//|
//|         ``mix`` blends the product back against the untouched source, so
//|         0.0 is a bypass and 1.0 is the product alone."""
//|         ...

static mp_obj_t audiomath_multiply_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_source, ARG_modulator, ARG_mix, ARG_sample_rate,
           ARG_channel_count };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_source, MP_ARG_OBJ, {.u_obj = MP_ROM_NONE} },
        { MP_QSTR_modulator, MP_ARG_OBJ, {.u_obj = MP_ROM_NONE} },
        { MP_QSTR_mix, MP_ARG_OBJ, {.u_obj = MP_ROM_NONE} },
        { MP_QSTR_sample_rate, MP_ARG_INT, {.u_int = 48000} },
        { MP_QSTR_channel_count, MP_ARG_INT, {.u_int = 2} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    audiomath_multiply_obj_t *self =
        mp_obj_malloc(audiomath_multiply_obj_t, type);
    if (args[ARG_channel_count].u_int < 1 ||
        args[ARG_channel_count].u_int > 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("channel_count must be 1 or 2"));
    }
    self->base.sample_rate = (uint32_t)args[ARG_sample_rate].u_int;
    self->base.max_buffer_length = sizeof(self->buffer);
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)args[ARG_channel_count].u_int;
    self->base.samples_signed = 1;
    self->base.single_buffer = false;
    self->source = MP_OBJ_NULL;
    self->modulator = MP_OBJ_NULL;
    self->pending_source = NULL;
    self->pending_source_frames = 0;
    self->pending_modulator = NULL;
    self->pending_modulator_frames = 0;
    audiodsp_multiply_config_init(&self->config);
    audiodsp_multiply_set_channel_count(&self->config,
        (uint32_t)self->base.channel_count);

    if (args[ARG_source].u_obj != mp_const_none) {
        audiosample_base_t *sample = audiosample_check(args[ARG_source].u_obj);
        if (sample->channel_count != self->base.channel_count) {
            mp_raise_ValueError(MP_ERROR_TEXT(
                "source channel_count does not match Multiply"));
        }
        self->source = args[ARG_source].u_obj;
    }
    if (args[ARG_modulator].u_obj != mp_const_none) {
        audiosample_base_t *sample = audiosample_check(
            args[ARG_modulator].u_obj);
        if (sample->channel_count != self->base.channel_count) {
            mp_raise_ValueError(MP_ERROR_TEXT(
                "modulator channel_count does not match Multiply"));
        }
        self->modulator = args[ARG_modulator].u_obj;
    }
    if (args[ARG_mix].u_obj != mp_const_none) {
        audiodsp_multiply_set_mix(&self->config,
            (float)mp_obj_get_float(args[ARG_mix].u_obj));
    }
    return MP_OBJ_FROM_PTR(self);
}

//|     def play(self, sample: circuitpython_typing.AudioSample) -> None:
//|         """Set the signal - the input that gets multiplied."""
//|         ...
static mp_obj_t audiomath_multiply_play(mp_obj_t self_in, mp_obj_t sample) {
    audiomath_multiply_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_base_t *base = audiosample_check(sample);
    if (base->channel_count != self->base.channel_count) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "source channel_count does not match Multiply"));
    }
    self->source = sample;
    self->pending_source = NULL;
    self->pending_source_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiomath_multiply_play_obj,
    audiomath_multiply_play);

//|     def modulate(self, sample: circuitpython_typing.AudioSample) -> None:
//|         """Set what to multiply the signal by."""
//|         ...
static mp_obj_t audiomath_multiply_modulate(mp_obj_t self_in,
    mp_obj_t sample) {
    audiomath_multiply_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_base_t *base = audiosample_check(sample);
    if (base->channel_count != self->base.channel_count) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "modulator channel_count does not match Multiply"));
    }
    self->modulator = sample;
    self->pending_modulator = NULL;
    self->pending_modulator_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiomath_multiply_modulate_obj,
    audiomath_multiply_modulate);

//|     def set(self, *, mix: float) -> None:
//|         """Change the blend mid-stream."""
//|         ...
//|
//|
// `set(mix=...)` rather than a property, to match audiodynamics.Dynamics --
// the two are siblings, both new here, and one settable option does not earn
// three copies of the getter/setter boilerplate.
static mp_obj_t audiomath_multiply_set(size_t n_args, const mp_obj_t *args,
    mp_map_t *kw_args) {
    audiomath_multiply_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    (void)n_args;
    for (size_t i = 0; i < kw_args->alloc; ++i) {
        if (!mp_map_slot_is_filled(kw_args, i)) {
            continue;
        }
        qstr name = mp_obj_str_get_qstr(kw_args->table[i].key);
        if (name != MP_QSTR_mix) {
            mp_raise_msg_varg(&mp_type_TypeError,
                MP_ERROR_TEXT("unknown Multiply option '%q'"), name);
        }
        audiodsp_multiply_set_mix(&self->config,
            (float)mp_obj_get_float(kw_args->table[i].value));
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(audiomath_multiply_set_obj, 1,
    audiomath_multiply_set);

// `deinit()` releases what this binding holds and marks the node
// deinitialised, so the guarded getters raise afterwards. The MicroPython
// binding of this same type gained it on 2026-09-09 (audiodsp#58, #60, #63) and
// this copy did not, which is audiodsp#75: the two bindings are hand-written and
// nothing held them to each other. Same fields, same order, deliberately.
static mp_obj_t audiomath_multiply_deinit(mp_obj_t self_in) {
    audiomath_multiply_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    self->source = mp_const_none;
    self->modulator = mp_const_none;
    self->pending_source = NULL;
    self->pending_source_frames = 0;
    self->pending_modulator = NULL;
    self->pending_modulator_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiomath_multiply_deinit_obj, audiomath_multiply_deinit);

static const mp_rom_map_elem_t audiomath_multiply_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiomath_multiply_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiomath_multiply_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_modulate),
      MP_ROM_PTR(&audiomath_multiply_modulate_obj) },
    { MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&audiomath_multiply_set_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiomath_multiply_locals_dict,
    audiomath_multiply_locals_dict_table);

static const audiosample_p_t audiomath_multiply_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)
        audiomath_multiply_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)
        audiomath_multiply_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiomath_multiply_type,
    MP_QSTR_Multiply,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiomath_multiply_make_new,
    locals_dict, &audiomath_multiply_locals_dict,
    protocol, &audiomath_multiply_proto
    );
