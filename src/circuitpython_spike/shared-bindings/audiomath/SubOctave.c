// audiomath.SubOctave bindings for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "shared-bindings/audiomath/SubOctave.h"
#include "shared-bindings/audiocore/__init__.h"

#include "py/objproperty.h"
#include "shared/runtime/context_manager_helpers.h"
#include "py/runtime.h"

//| class SubOctave:
//|     """An analog-style octave divider: a comparator into a chain of
//|     flip-flops, which is what is inside a Boss OC-2 and every pedal like
//|     it.
//|
//|     Every other cycle of the signal is inverted, so the output's period is
//|     twice the input's - an octave down - with the input's own timbre and
//|     no latency at all. `audiodelays.PitchShift` also goes down an octave,
//|     but granularly: windowed grains, a comb, and a buffer per instance. A
//|     divider is none of that.
//|
//|     Processes signed 16-bit audio and hands out 256 frames at a time. It
//|     sits in an audiosample chain like any other effect, and never reports
//|     itself finished - a starved chain gets silence.
//|
//|     One divider drives every channel, clocked by the mean of the frame, so
//|     a stereo pair cannot come apart and cancel in mono."""
//|
//|     def __init__(
//|         self,
//|         source: Optional[circuitpython_typing.AudioSample] = None,
//|         order: int = 1,
//|         mix: float = 1.0,
//|         threshold: float = 0.01,
//|         hold_ms: float = 1.0,
//|         sample_rate: int = 48000,
//|         channel_count: int = 2,
//|     ) -> None:
//|         """Divide ``source`` down in frequency.
//|
//|         ``order`` is 1 for one octave down and 2 for two. ``mix`` blends
//|         the divided signal against the untouched source, so 0.0 is a
//|         bypass and 1.0 is the divider alone.
//|
//|         ``threshold`` is the comparator's hysteresis as a fraction of full
//|         scale, and ``hold_ms`` is how long after an accepted edge another
//|         is ignored. Both exist to stop a harmonic from clocking the chain:
//|         a waveform with a strong second harmonic crosses zero more than
//|         twice per period, and a divider that counts those extra crossings
//|         drops an octave too far. Neither default is a substitute for
//|         filtering the input - set ``hold_ms`` from the lowest note the
//|         effect is built for, or raise ``threshold`` above the harmonic's
//|         own excursion."""
//|         ...

static mp_obj_t audiomath_suboctave_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_source, ARG_order, ARG_mix, ARG_threshold, ARG_hold_ms,
           ARG_sample_rate, ARG_channel_count };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_source, MP_ARG_OBJ, {.u_obj = MP_ROM_NONE} },
        { MP_QSTR_order, MP_ARG_OBJ, {.u_obj = MP_ROM_NONE} },
        { MP_QSTR_mix, MP_ARG_OBJ, {.u_obj = MP_ROM_NONE} },
        { MP_QSTR_threshold, MP_ARG_OBJ, {.u_obj = MP_ROM_NONE} },
        { MP_QSTR_hold_ms, MP_ARG_OBJ, {.u_obj = MP_ROM_NONE} },
        { MP_QSTR_sample_rate, MP_ARG_INT, {.u_int = 48000} },
        { MP_QSTR_channel_count, MP_ARG_INT, {.u_int = 2} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    audiomath_suboctave_obj_t *self =
        mp_obj_malloc(audiomath_suboctave_obj_t, type);
    if (args[ARG_channel_count].u_int < 1 ||
        args[ARG_channel_count].u_int > 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("channel_count must be 1 or 2"));
    }
    if (args[ARG_sample_rate].u_int < 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("sample_rate must be at least 1"));
    }
    self->base.sample_rate = (uint32_t)args[ARG_sample_rate].u_int;
    self->base.max_buffer_length = sizeof(self->buffer);
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)args[ARG_channel_count].u_int;
    self->base.samples_signed = 1;
    self->base.single_buffer = false;
    self->source = MP_OBJ_NULL;
    self->pending = NULL;
    self->pending_frames = 0;
    audiodsp_suboctave_config_init(&self->config, self->base.sample_rate);
    audiodsp_suboctave_set_channel_count(&self->config,
        (uint32_t)self->base.channel_count);
    audiodsp_suboctave_state_init(&self->state);

    static const audiodsp_suboctave_option_t positional[] = {
        AUDIODSP_SUBOCTAVE_OPT_ORDER,
        AUDIODSP_SUBOCTAVE_OPT_MIX,
        AUDIODSP_SUBOCTAVE_OPT_THRESHOLD,
        AUDIODSP_SUBOCTAVE_OPT_HOLD_MS,
    };
    for (size_t i = 0; i < MP_ARRAY_SIZE(positional); ++i) {
        mp_obj_t value = args[ARG_order + i].u_obj;
        if (value != mp_const_none) {
            audiodsp_suboctave_configure(&self->config, positional[i],
                (float)mp_obj_get_float(value));
        }
    }
    audiodsp_suboctave_config_finish(&self->config);

    if (args[ARG_source].u_obj != mp_const_none) {
        audiosample_base_t *sample = audiosample_check(args[ARG_source].u_obj);
        if (sample->channel_count != self->base.channel_count) {
            mp_raise_ValueError(MP_ERROR_TEXT(
                "source channel_count does not match SubOctave"));
        }
        self->source = args[ARG_source].u_obj;
    }
    return MP_OBJ_FROM_PTR(self);
}

//|     def play(self, sample: circuitpython_typing.AudioSample) -> None:
//|         """Set the signal to divide."""
//|         ...
static mp_obj_t audiomath_suboctave_play(mp_obj_t self_in, mp_obj_t sample) {
    audiomath_suboctave_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_base_t *base = audiosample_check(sample);
    if (base->channel_count != self->base.channel_count) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "source channel_count does not match SubOctave"));
    }
    self->source = sample;
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiomath_suboctave_play_obj,
    audiomath_suboctave_play);

//|     def set(
//|         self,
//|         *,
//|         order: int = ...,
//|         mix: float = ...,
//|         threshold: float = ...,
//|         hold_ms: float = ...,
//|     ) -> None:
//|         """Change settings mid-stream. The count keeps running: changing
//|         the mix must not flip the output's polarity under the player."""
//|         ...
static bool suboctave_option_slot(qstr name,
    audiodsp_suboctave_option_t *option) {
    if (name == MP_QSTR_order) {
        *option = AUDIODSP_SUBOCTAVE_OPT_ORDER;
    } else if (name == MP_QSTR_mix) {
        *option = AUDIODSP_SUBOCTAVE_OPT_MIX;
    } else if (name == MP_QSTR_threshold) {
        *option = AUDIODSP_SUBOCTAVE_OPT_THRESHOLD;
    } else if (name == MP_QSTR_hold_ms) {
        *option = AUDIODSP_SUBOCTAVE_OPT_HOLD_MS;
    } else {
        return false;
    }
    return true;
}

static mp_obj_t audiomath_suboctave_set(size_t n_args, const mp_obj_t *args,
    mp_map_t *kw_args) {
    audiomath_suboctave_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    (void)n_args;
    for (size_t i = 0; i < kw_args->alloc; ++i) {
        if (!mp_map_slot_is_filled(kw_args, i)) {
            continue;
        }
        qstr name = mp_obj_str_get_qstr(kw_args->table[i].key);
        audiodsp_suboctave_option_t option;
        if (!suboctave_option_slot(name, &option)) {
            mp_raise_msg_varg(&mp_type_TypeError,
                MP_ERROR_TEXT("unknown SubOctave option '%q'"), name);
        }
        audiodsp_suboctave_configure(&self->config, option,
            (float)mp_obj_get_float(kw_args->table[i].value));
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(audiomath_suboctave_set_obj, 1,
    audiomath_suboctave_set);

//|     def clear(self) -> None:
//|         """Back to the pass-through polarity, comparator and lockout
//|         clear."""
//|         ...
//|
//|
static mp_obj_t audiomath_suboctave_clear(mp_obj_t self_in) {
    audiomath_suboctave_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_suboctave_reset(&self->state);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiomath_suboctave_clear_obj,
    audiomath_suboctave_clear);

// `deinit()` releases what this binding holds and marks the node
// deinitialised, so the guarded getters raise afterwards. The MicroPython
// binding of this same type gained it on 2026-09-09 (audiodsp#58, #60, #63) and
// this copy did not, which is audiodsp#75: the two bindings are hand-written and
// nothing held them to each other. Same fields, same order, deliberately.
static mp_obj_t audiomath_suboctave_deinit(mp_obj_t self_in) {
    audiomath_suboctave_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    self->source = mp_const_none;
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiomath_suboctave_deinit_obj, audiomath_suboctave_deinit);

static const mp_rom_map_elem_t audiomath_suboctave_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiomath_suboctave_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiomath_suboctave_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&audiomath_suboctave_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear),
      MP_ROM_PTR(&audiomath_suboctave_clear_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiomath_suboctave_locals_dict,
    audiomath_suboctave_locals_dict_table);

static const audiosample_p_t audiomath_suboctave_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)
        audiomath_suboctave_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)
        audiomath_suboctave_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiomath_suboctave_type,
    MP_QSTR_SubOctave,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiomath_suboctave_make_new,
    locals_dict, &audiomath_suboctave_locals_dict,
    protocol, &audiomath_suboctave_proto
    );
