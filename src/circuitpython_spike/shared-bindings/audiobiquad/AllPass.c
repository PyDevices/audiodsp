// audiobiquad.AllPass bindings for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <string.h>

#include "shared-bindings/audiobiquad/AllPass.h"
#include "shared-bindings/audiocore/__init__.h"

#include "py/objproperty.h"
#include "shared/runtime/context_manager_helpers.h"
#include "py/runtime.h"

//| class AllPass:
//|     """A cascade of first-order all-pass stages with float state and an
//|     unclamped feedback.
//|
//|     `audiofilters.Phaser` is the same topology in int16, and two things
//|     are out of reach on it. Its all-pass memory settles on a non-zero word
//|     and holds it, so a tail never reaches zero; and its feedback is
//|     clamped to 0.1..0.9, so the feedback-free topology every script phaser
//|     uses cannot be asked for and its notches stop well short of a null.
//|     Here ``feedback=0.0`` is zero, and negative feedback - the Electric
//|     Mistress's inverted loop - is allowed.
//|
//|     ``frequency`` is the frequency at which one stage's phase passes -90
//|     degrees, and it really is: the coefficient is the bilinear
//|     ``(tan(pi f / fs) - 1) / (tan(pi f / fs) + 1)`` rather than its
//|     small-angle form, so nothing has to pre-warp it."""
//|
//|     def __init__(
//|         self,
//|         *,
//|         stages: int = 4,
//|         frequency: synthio.BlockInput = 1000.0,
//|         feedback: synthio.BlockInput = 0.0,
//|         mix: synthio.BlockInput = 1.0,
//|         sample_rate: int = 48000,
//|         channel_count: int = 2,
//|     ) -> None:
//|         """Create an all-pass cascade. ``stages`` sizes the state and
//|         cannot change afterwards. ``mix`` crossfades: 0 is a wire, 0.5 is
//|         the equal sum of dry and cascade that a script phaser makes - and
//|         where the notches are deepest - and 1 is the bare cascade, which is
//|         magnitude-flat and so sounds like nothing."""
//|         ...

static mp_obj_t audiobiquad_allpass_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_stages, ARG_frequency, ARG_feedback, ARG_mix, ARG_sample_rate,
           ARG_channel_count };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_stages, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 4} },
        { MP_QSTR_frequency, MP_ARG_OBJ | MP_ARG_KW_ONLY,
          {.u_obj = MP_ROM_INT(1000)} },
        { MP_QSTR_feedback, MP_ARG_OBJ | MP_ARG_KW_ONLY,
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
    mp_int_t stages = mp_arg_validate_int_range(args[ARG_stages].u_int, 1,
        (mp_int_t)AUDIODSP_FILTER_F32_MAX_STAGES, MP_QSTR_stages);

    audiobiquad_allpass_obj_t *self =
        mp_obj_malloc(audiobiquad_allpass_obj_t, &audiobiquad_allpass_type);
    self->base.sample_rate = (uint32_t)args[ARG_sample_rate].u_int;
    self->base.max_buffer_length = sizeof(self->buffer);
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)channel_count;
    self->base.samples_signed = 1;
    self->base.single_buffer = false;
    self->source = MP_OBJ_NULL;
    self->pending = NULL;
    self->pending_frames = 0;

    audiodsp_allpass_f32_config_init(&self->config, self->base.sample_rate,
        (uint32_t)channel_count, (uint32_t)stages);
    // `stages` sizes the state, so the binding allocates it and the DSP
    // layer only borrows the pointer -- the same division of labour
    // audioecho and audioconvolve use.
    const uint32_t count = (uint32_t)(channel_count * stages);
    float *lanes = m_malloc((size_t)count * sizeof(float));
    memset(lanes, 0, (size_t)count * sizeof(float));
    audiodsp_allpass_f32_state_init(&self->state, lanes, count);

    synthio_block_assign_slot(args[ARG_frequency].u_obj, &self->frequency,
        MP_QSTR_frequency);
    synthio_block_assign_slot(args[ARG_feedback].u_obj, &self->feedback,
        MP_QSTR_feedback);
    synthio_block_assign_slot(args[ARG_mix].u_obj, &self->mix, MP_QSTR_mix);
    audiodsp_allpass_f32_config_finish(&self->config);
    return MP_OBJ_FROM_PTR(self);
}

//|     def play(self, sample: circuitpython_typing.AudioSample) -> None:
//|         """Set the source the cascade reads from."""
//|         ...
static mp_obj_t audiobiquad_allpass_play(mp_obj_t self_in, mp_obj_t sample) {
    audiobiquad_allpass_obj_t *self = MP_OBJ_TO_PTR(self_in);
    (void)audiosample_check(sample);
    self->source = sample;
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiobiquad_allpass_play_obj,
    audiobiquad_allpass_play);

//|     def stop(self) -> None:
//|         """Let go of the source. The node keeps handing out silence."""
//|         ...
static mp_obj_t audiobiquad_allpass_stop(mp_obj_t self_in) {
    audiobiquad_allpass_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->source = MP_OBJ_NULL;
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiobiquad_allpass_stop_obj,
    audiobiquad_allpass_stop);

//|     def clear(self) -> None:
//|         """Zero every stage and the feedback path."""
//|         ...
static mp_obj_t audiobiquad_allpass_clear(mp_obj_t self_in) {
    audiobiquad_allpass_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_allpass_f32_reset(&self->state);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiobiquad_allpass_clear_obj,
    audiobiquad_allpass_clear);

//|     playing: bool
//|     """True while a source is attached."""
static mp_obj_t audiobiquad_allpass_obj_get_playing(mp_obj_t self_in) {
    audiobiquad_allpass_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(self->source != MP_OBJ_NULL);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiobiquad_allpass_get_playing_obj,
    audiobiquad_allpass_obj_get_playing);
MP_PROPERTY_GETTER(audiobiquad_allpass_playing_obj,
    (mp_obj_t)&audiobiquad_allpass_get_playing_obj);

//|     stages: int
//|     """How many all-pass sections are cascaded. Read-only: it sizes the
//|     state."""
static mp_obj_t audiobiquad_allpass_obj_get_stages(mp_obj_t self_in) {
    audiobiquad_allpass_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->config.stages);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiobiquad_allpass_get_stages_obj,
    audiobiquad_allpass_obj_get_stages);
MP_PROPERTY_GETTER(audiobiquad_allpass_stages_obj,
    (mp_obj_t)&audiobiquad_allpass_get_stages_obj);

//|     frequency: synthio.BlockInput
//|     """Where one stage's phase passes -90 degrees."""
//|
//|     feedback: synthio.BlockInput
//|     """-0.99..0.99, and zero is zero. The bound is stability, not taste."""
//|
//|     mix: synthio.BlockInput
//|     """0 is a wire, 0.5 the script phaser's equal sum, 1 the bare
//|     cascade."""
#define ALLPASS_SLOT_PROPERTY(name) \
    static mp_obj_t audiobiquad_allpass_obj_get_##name(mp_obj_t self_in) { \
        audiobiquad_allpass_obj_t *self = MP_OBJ_TO_PTR(self_in); \
        return self->name.obj; \
    } \
    MP_DEFINE_CONST_FUN_OBJ_1(audiobiquad_allpass_get_##name##_obj, \
        audiobiquad_allpass_obj_get_##name); \
    static mp_obj_t audiobiquad_allpass_obj_set_##name(mp_obj_t self_in, \
        mp_obj_t value) { \
        audiobiquad_allpass_obj_t *self = MP_OBJ_TO_PTR(self_in); \
        synthio_block_assign_slot(value, &self->name, MP_QSTR_##name); \
        return mp_const_none; \
    } \
    MP_DEFINE_CONST_FUN_OBJ_2(audiobiquad_allpass_set_##name##_obj, \
        audiobiquad_allpass_obj_set_##name); \
    MP_PROPERTY_GETSET(audiobiquad_allpass_##name##_obj, \
        (mp_obj_t)&audiobiquad_allpass_get_##name##_obj, \
        (mp_obj_t)&audiobiquad_allpass_set_##name##_obj)

ALLPASS_SLOT_PROPERTY(frequency);
ALLPASS_SLOT_PROPERTY(feedback);
ALLPASS_SLOT_PROPERTY(mix);

//|     coefficient: float
//|     """The all-pass coefficient at the frequency in force.
//|
//|     Reads the block inputs but does not advance them, so looking at this
//|     does not move an LFO along."""
//|
//|
static mp_obj_t audiobiquad_allpass_obj_get_coefficient(mp_obj_t self_in) {
    audiobiquad_allpass_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiobiquad_allpass_refresh(self);
    return mp_obj_new_float((mp_float_t)self->config.coefficient);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiobiquad_allpass_get_coefficient_obj,
    audiobiquad_allpass_obj_get_coefficient);
MP_PROPERTY_GETTER(audiobiquad_allpass_coefficient_obj,
    (mp_obj_t)&audiobiquad_allpass_get_coefficient_obj);

// `deinit()` releases what this binding holds and marks the node
// deinitialised, so the guarded getters raise afterwards. The MicroPython
// binding of this same type gained it on 2026-09-09 (audiodsp#58, #60, #63) and
// this copy did not, which is audiodsp#75: the two bindings are hand-written and
// nothing held them to each other. Same fields, same order, deliberately.
static mp_obj_t audiobiquad_allpass_deinit(mp_obj_t self_in) {
    audiobiquad_allpass_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    self->source = mp_const_none;
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiobiquad_allpass_deinit_obj, audiobiquad_allpass_deinit);

static const mp_rom_map_elem_t audiobiquad_allpass_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiobiquad_allpass_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiobiquad_allpass_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audiobiquad_allpass_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear),
      MP_ROM_PTR(&audiobiquad_allpass_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_playing),
      MP_ROM_PTR(&audiobiquad_allpass_playing_obj) },
    { MP_ROM_QSTR(MP_QSTR_stages),
      MP_ROM_PTR(&audiobiquad_allpass_stages_obj) },
    { MP_ROM_QSTR(MP_QSTR_frequency),
      MP_ROM_PTR(&audiobiquad_allpass_frequency_obj) },
    { MP_ROM_QSTR(MP_QSTR_feedback),
      MP_ROM_PTR(&audiobiquad_allpass_feedback_obj) },
    { MP_ROM_QSTR(MP_QSTR_mix), MP_ROM_PTR(&audiobiquad_allpass_mix_obj) },
    { MP_ROM_QSTR(MP_QSTR_coefficient),
      MP_ROM_PTR(&audiobiquad_allpass_coefficient_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiobiquad_allpass_locals_dict,
    audiobiquad_allpass_locals_dict_table);

static const audiosample_p_t audiobiquad_allpass_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)
        audiobiquad_allpass_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)
        audiobiquad_allpass_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiobiquad_allpass_type,
    MP_QSTR_AllPass,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiobiquad_allpass_make_new,
    locals_dict, &audiobiquad_allpass_locals_dict,
    protocol, &audiobiquad_allpass_proto
    );
