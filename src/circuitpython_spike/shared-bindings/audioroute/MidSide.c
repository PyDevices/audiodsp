// audioroute.MidSide bindings for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "shared-bindings/audioroute/MidSide.h"
#include "shared-bindings/audiocore/__init__.h"

#include "py/objproperty.h"
#include "shared/runtime/context_manager_helpers.h"
#include "py/runtime.h"

//| class MidSide:
//|     """A stereo pair taken apart into its mono sum and its difference, the
//|     difference scaled, and the pair put back together.
//|
//|     Processes signed 16-bit stereo, and hands out 256 frames at a time. It
//|     sits in an audiosample chain like any other effect, and never reports
//|     itself finished - a starved chain gets silence.
//|
//|     ``width=0`` collapses the pair to mono, ``1`` passes it through
//|     untouched, ``2`` doubles the sides. The identity at ``width=1`` is
//|     exact - the output bytes are the input bytes, for every int16 pair -
//|     so the node costs nothing to leave in a chain that is not using it.
//|     There is no state and no latency: a block boundary is not observable."""
//|
//|     def __init__(
//|         self,
//|         source: Optional[circuitpython_typing.AudioSample] = None,
//|         width: float = 1.0,
//|         sample_rate: int = 48000,
//|         channel_count: int = 2,
//|     ) -> None:
//|         """Scale the difference between ``source``'s channels.
//|
//|         ``width`` is clamped to 0.0 .. 2.0. A mono ``source`` passes
//|         through: there is no difference to scale."""
//|         ...

static mp_obj_t audioroute_midside_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_source, ARG_width, ARG_sample_rate, ARG_channel_count };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_source, MP_ARG_OBJ, {.u_obj = MP_ROM_NONE} },
        { MP_QSTR_width, MP_ARG_OBJ, {.u_obj = MP_ROM_NONE} },
        { MP_QSTR_sample_rate, MP_ARG_INT, {.u_int = 48000} },
        { MP_QSTR_channel_count, MP_ARG_INT, {.u_int = 2} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_channel_count].u_int < 1 ||
        args[ARG_channel_count].u_int > 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("channel_count must be 1 or 2"));
    }
    audioroute_midside_obj_t *self =
        mp_obj_malloc(audioroute_midside_obj_t, type);
    self->base.sample_rate = (uint32_t)args[ARG_sample_rate].u_int;
    self->base.max_buffer_length = sizeof(self->buffer);
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)args[ARG_channel_count].u_int;
    self->base.samples_signed = 1;
    self->base.single_buffer = false;
    self->source = MP_OBJ_NULL;
    self->pending = NULL;
    self->pending_frames = 0;
    audiodsp_midside_config_init(&self->config);
    audiodsp_midside_set_channel_count(&self->config,
        (uint32_t)self->base.channel_count);

    if (args[ARG_source].u_obj != mp_const_none) {
        audiosample_base_t *sample = audiosample_check(args[ARG_source].u_obj);
        if (sample->channel_count != self->base.channel_count) {
            mp_raise_ValueError(MP_ERROR_TEXT(
                "source channel_count does not match MidSide"));
        }
        self->source = args[ARG_source].u_obj;
    }
    if (args[ARG_width].u_obj != mp_const_none) {
        audiodsp_midside_set_width(&self->config,
            (float)mp_obj_get_float(args[ARG_width].u_obj));
    }
    return MP_OBJ_FROM_PTR(self);
}

//|     def play(self, sample: circuitpython_typing.AudioSample) -> None:
//|         """Set the source the matrix reads from."""
//|         ...
static mp_obj_t audioroute_midside_play(mp_obj_t self_in, mp_obj_t sample) {
    audioroute_midside_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_base_t *base = audiosample_check(sample);
    if (base->channel_count != self->base.channel_count) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "source channel_count does not match MidSide"));
    }
    self->source = sample;
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audioroute_midside_play_obj,
    audioroute_midside_play);

//|     def set(self, **options: float) -> None:
//|         """Change ``width`` mid-stream. There is no state to carry, so the
//|         new width applies from the next frame out."""
//|         ...
//|
//|
static mp_obj_t audioroute_midside_set(size_t n_args, const mp_obj_t *args,
    mp_map_t *kw_args) {
    audioroute_midside_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    (void)n_args;
    for (size_t i = 0; i < kw_args->alloc; ++i) {
        if (!mp_map_slot_is_filled(kw_args, i)) {
            continue;
        }
        qstr name = mp_obj_str_get_qstr(kw_args->table[i].key);
        if (name != MP_QSTR_width) {
            mp_raise_msg_varg(&mp_type_TypeError,
                MP_ERROR_TEXT("unknown MidSide option '%q'"), name);
        }
        audiodsp_midside_set_width(&self->config,
            (float)mp_obj_get_float(kw_args->table[i].value));
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(audioroute_midside_set_obj, 1,
    audioroute_midside_set);

// `deinit()` releases what this binding holds and marks the node
// deinitialised, so the guarded getters raise afterwards. The MicroPython
// binding of this same type gained it on 2026-09-09 (audiodsp#58, #60, #63) and
// this copy did not, which is audiodsp#75: the two bindings are hand-written and
// nothing held them to each other. Same fields, same order, deliberately.
static mp_obj_t audioroute_midside_deinit(mp_obj_t self_in) {
    audioroute_midside_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    self->source = mp_const_none;
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audioroute_midside_deinit_obj, audioroute_midside_deinit);

static const mp_rom_map_elem_t audioroute_midside_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audioroute_midside_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audioroute_midside_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&audioroute_midside_set_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audioroute_midside_locals_dict,
    audioroute_midside_locals_dict_table);

static const audiosample_p_t audioroute_midside_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)
        audioroute_midside_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)
        audioroute_midside_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audioroute_midside_type,
    MP_QSTR_MidSide,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audioroute_midside_make_new,
    locals_dict, &audioroute_midside_locals_dict,
    protocol, &audioroute_midside_proto
    );
