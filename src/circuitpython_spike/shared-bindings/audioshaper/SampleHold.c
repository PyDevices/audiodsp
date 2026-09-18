// audioshaper.SampleHold bindings for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <string.h>

#include "shared-bindings/audioshaper/SampleHold.h"
#include "shared-bindings/audiocore/__init__.h"

#include "py/objproperty.h"
#include "shared/runtime/context_manager_helpers.h"
#include "py/runtime.h"

//| class SampleHold:
//|     """A zero-order hold at an exact rational ratio: ``num`` frames carry
//|     ``den`` new values.
//|
//|     One source frame in, one frame out, at the source's own sample rate,
//|     channel count and bit depth - a rate *reducer*, not a resampler. The
//|     held value changes only when an accumulator wraps, and that accumulator
//|     is the exact remainder of ``n * den`` modulo ``num``, so a ratio is
//|     exact for as long as the stream runs and nothing drifts.
//|
//|     It is here rather than as a rate form on ``audiospeed.SpeedChanger``
//|     because that module is CircuitPython's: its rate is 16.16 fixed point,
//|     a down-then-up pair of them cannot be made reciprocal except at powers
//|     of two, and an exact-rational rate added to audioif's copy would not
//|     exist on a stock board."""
//|
//|     def __init__(self, source: circuitpython_typing.AudioSample,
//|                  num: int = 1, den: int = 1) -> None:
//|         """Hold ``source``, refreshing ``den`` times in every ``num``
//|         frames. ``den`` may not exceed ``num``: a hold cannot invent
//|         frames. The pair is reduced, so 48000/26040 is reported back as
//|         400/217, and the format is the source's own and fixed here."""
//|         ...

// The ratio is one setting, not two, so both halves arrive together
// everywhere -- the constructor and `set()` share this.
static void samplehold_check_ratio(mp_int_t num, mp_int_t den) {
    if (num < 1 || den < 1 || den > num ||
        num > (mp_int_t)AUDIOIF_SAMPLEHOLD_MAX_RATIO) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "num and den must be whole, den <= num (a hold cannot invent "
            "frames)"));
    }
}

static mp_obj_t audioshaper_samplehold_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_source, ARG_num, ARG_den };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_source, MP_ARG_REQUIRED | MP_ARG_OBJ, {} },
        { MP_QSTR_num, MP_ARG_INT, { .u_int = 1 } },
        { MP_QSTR_den, MP_ARG_INT, { .u_int = 1 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed),
        allowed, args);
    samplehold_check_ratio(args[ARG_num].u_int, args[ARG_den].u_int);

    mp_obj_t source = args[ARG_source].u_obj;
    audiosample_base_t *src = audiosample_check(source);
    uint32_t frame_bytes =
        (uint32_t)(src->bits_per_sample / 8u) * src->channel_count;
    if (frame_bytes < 1u ||
        frame_bytes > AUDIOIF_SAMPLEHOLD_MAX_FRAME_BYTES) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "source frames must be 1 or 2 channels of 8- or 16-bit audio"));
    }

    audioshaper_samplehold_obj_t *self =
        mp_obj_malloc(audioshaper_samplehold_obj_t, type);
    // The format is the source's own: this node resamples nothing, so every
    // field downstream reads is the one it would have read from the source.
    self->base.sample_rate = src->sample_rate;
    self->base.channel_count = src->channel_count;
    self->base.bits_per_sample = src->bits_per_sample;
    self->base.samples_signed = src->samples_signed;
    self->base.single_buffer = false;
    self->base.max_buffer_length = AUDIOIF_SAMPLEHOLD_FRAMES * frame_bytes;
    self->source = source;
    self->frame_bytes = (uint8_t)frame_bytes;
    self->pending = NULL;
    self->pending_frames = 0;
    self->source_done = false;
    self->source_exhausted = false;

    audioif_samplehold_config_init(&self->config,
        (uint32_t)args[ARG_num].u_int, (uint32_t)args[ARG_den].u_int);
    audioif_samplehold_state_init(&self->state, &self->config);
    return MP_OBJ_FROM_PTR(self);
}

//|     def play(self, sample: circuitpython_typing.AudioSample) -> None:
//|         """Set the source the hold reads from, in the format fixed at
//|         construction, and start its staircase over."""
//|         ...
static mp_obj_t audioshaper_samplehold_play(mp_obj_t self_in,
    mp_obj_t sample) {
    audioshaper_samplehold_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_base_t *src = audiosample_check(sample);
    if (src->bits_per_sample != self->base.bits_per_sample ||
        src->channel_count != self->base.channel_count) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "source format does not match the one this node was built with"));
    }
    self->source = sample;
    self->pending = NULL;
    self->pending_frames = 0;
    self->source_done = false;
    self->source_exhausted = false;
    audioif_samplehold_reset(&self->state, &self->config);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audioshaper_samplehold_play_obj,
    audioshaper_samplehold_play);

//|     def set(self, num: int, den: int) -> None:
//|         """Change the ratio mid-stream. The pair moves together or not at
//|         all. A ratio that actually changed re-arms the accumulator; one set
//|         to what it already was does nothing, so a class writing its
//|         settings every block does not restart the staircase."""
//|         ...
static mp_obj_t audioshaper_samplehold_set(size_t n_args,
    const mp_obj_t *args, mp_map_t *kw_args) {
    enum { ARG_num, ARG_den };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_num, MP_ARG_REQUIRED | MP_ARG_INT, {} },
        { MP_QSTR_den, MP_ARG_REQUIRED | MP_ARG_INT, {} },
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args - 1, args + 1, kw_args, MP_ARRAY_SIZE(allowed),
        allowed, parsed);
    samplehold_check_ratio(parsed[ARG_num].u_int, parsed[ARG_den].u_int);
    audioshaper_samplehold_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (audioif_samplehold_config_set(&self->config,
        (uint32_t)parsed[ARG_num].u_int, (uint32_t)parsed[ARG_den].u_int)) {
        audioif_samplehold_reset(&self->state, &self->config);
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(audioshaper_samplehold_set_obj, 1,
    audioshaper_samplehold_set);

//|     def clear(self) -> None:
//|         """Arm the accumulator and forget the held frame. That is the whole
//|         of this node's state: it has no delay line and no filter."""
//|         ...
static mp_obj_t audioshaper_samplehold_clear(mp_obj_t self_in) {
    audioshaper_samplehold_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audioif_samplehold_reset(&self->state, &self->config);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audioshaper_samplehold_clear_obj,
    audioshaper_samplehold_clear);

//|     num: int
//|     """The reduced ratio's numerator: frames per period. Read-only."""
static mp_obj_t audioshaper_samplehold_get_num(mp_obj_t self_in) {
    audioshaper_samplehold_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return MP_OBJ_NEW_SMALL_INT(self->config.num);
}
MP_DEFINE_CONST_FUN_OBJ_1(audioshaper_samplehold_get_num_obj,
    audioshaper_samplehold_get_num);
MP_PROPERTY_GETTER(audioshaper_samplehold_num_obj,
    (mp_obj_t)&audioshaper_samplehold_get_num_obj);

//|     den: int
//|     """The reduced ratio's denominator: new values per period. Read-only."""
static mp_obj_t audioshaper_samplehold_get_den(mp_obj_t self_in) {
    audioshaper_samplehold_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return MP_OBJ_NEW_SMALL_INT(self->config.den);
}
MP_DEFINE_CONST_FUN_OBJ_1(audioshaper_samplehold_get_den_obj,
    audioshaper_samplehold_get_den);
MP_PROPERTY_GETTER(audioshaper_samplehold_den_obj,
    (mp_obj_t)&audioshaper_samplehold_get_den_obj);

//|     latency: int
//|     """0, at every ratio: a refresh latches the frame it is looking at and
//|     emits it in the same frame. What a hold displaces is an event landing
//|     on a frame it drops - up to ``ceil(num/den) - 1`` frames - and that is
//|     the effect rather than a delay of this node."""
//|
//|
static mp_obj_t audioshaper_samplehold_get_latency(mp_obj_t self_in) {
    audioshaper_samplehold_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return MP_OBJ_NEW_SMALL_INT(0);
}
MP_DEFINE_CONST_FUN_OBJ_1(audioshaper_samplehold_get_latency_obj,
    audioshaper_samplehold_get_latency);
MP_PROPERTY_GETTER(audioshaper_samplehold_latency_obj,
    (mp_obj_t)&audioshaper_samplehold_get_latency_obj);

// `deinit()` releases what this binding holds and marks the node
// deinitialised, so the guarded getters raise afterwards. Same fields, same
// order as the MicroPython binding, deliberately (audioif#75).
static mp_obj_t audioshaper_samplehold_deinit(mp_obj_t self_in) {
    audioshaper_samplehold_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    self->source = mp_const_none;
    self->pending = NULL;
    self->pending_frames = 0;
    self->source_exhausted = true;
    audioif_samplehold_reset(&self->state, &self->config);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audioshaper_samplehold_deinit_obj,
    audioshaper_samplehold_deinit);

static const mp_rom_map_elem_t
audioshaper_samplehold_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit),
      MP_ROM_PTR(&audioshaper_samplehold_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play),
      MP_ROM_PTR(&audioshaper_samplehold_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_set),
      MP_ROM_PTR(&audioshaper_samplehold_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear),
      MP_ROM_PTR(&audioshaper_samplehold_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_num), MP_ROM_PTR(&audioshaper_samplehold_num_obj) },
    { MP_ROM_QSTR(MP_QSTR_den), MP_ROM_PTR(&audioshaper_samplehold_den_obj) },
    { MP_ROM_QSTR(MP_QSTR_latency),
      MP_ROM_PTR(&audioshaper_samplehold_latency_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audioshaper_samplehold_locals_dict,
    audioshaper_samplehold_locals_dict_table);

static const audiosample_p_t audioshaper_samplehold_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)
        audioshaper_samplehold_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)
        audioshaper_samplehold_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audioshaper_samplehold_type,
    MP_QSTR_SampleHold,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audioshaper_samplehold_make_new,
    locals_dict, &audioshaper_samplehold_locals_dict,
    protocol, &audioshaper_samplehold_proto
    );
