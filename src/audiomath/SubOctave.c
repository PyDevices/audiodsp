// audiomath.SubOctave. See SubOctave.h for provenance.
// SPDX-License-Identifier: MIT

#include "audiomath/SubOctave.h"

#include <string.h>

#include "cp_compat/context_manager_helpers.h"
#include "py/runtime.h"
#include "shared/audiodsp_pump_lock.h"

//: Keyword -> the DSP's option slot, in the order
//: shared/audiodsp_suboctave.h declares them.
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

static mp_obj_t audiomath_suboctave_play(mp_obj_t self_in, mp_obj_t sample) {
    audiomath_suboctave_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_base_t *base = audiosample_check(sample);
    if (base->channel_count != self->base.channel_count) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "source channel_count does not match SubOctave"));
    }
    audiodsp_pump_lock_acquire();
    self->source = sample;
    self->pending = NULL;
    self->pending_frames = 0;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiomath_suboctave_play_obj,
    audiomath_suboctave_play);

// `set(...)` rather than properties, to match audiomath.Multiply and
// audioecho.FeedbackDelay. The count deliberately keeps running: changing the
// mix must not flip the output's polarity under the player.
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
static MP_DEFINE_CONST_FUN_OBJ_KW(audiomath_suboctave_set_obj, 1,
    audiomath_suboctave_set);

static mp_obj_t audiomath_suboctave_clear(mp_obj_t self_in) {
    audiomath_suboctave_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_acquire();
    audiodsp_suboctave_reset(&self->state);
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiomath_suboctave_clear_obj,
    audiomath_suboctave_clear);

static audioio_get_buffer_result_t audiomath_suboctave_get_buffer(
    mp_obj_t self_in, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    audiomath_suboctave_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const uint32_t width = 2u * self->base.channel_count;
    uint32_t produced = 0;
    while (produced < AUDIODSP_SUBOCTAVE_FRAMES) {
        if (self->pending_frames == 0) {
            if (self->source == MP_OBJ_NULL) {
                break;
            }
            uint8_t *raw = NULL;
            uint32_t raw_bytes = 0;
            audioio_get_buffer_result_t result = audiosample_get_buffer(
                self->source, false, 0, &raw, &raw_bytes);
            if (result == GET_BUFFER_ERROR || raw == NULL || raw_bytes < width) {
                break;
            }
            self->pending = (const int16_t *)raw;
            self->pending_frames = raw_bytes / width;
        }
        uint32_t run = AUDIODSP_SUBOCTAVE_FRAMES - produced;
        if (run > self->pending_frames) {
            run = self->pending_frames;
        }
        audiodsp_suboctave_process_s16(&self->config, &self->state,
            &self->buffer[produced * self->base.channel_count],
            self->pending, run);
        self->pending += run * self->base.channel_count;
        self->pending_frames -= run;
        produced += run;
    }
    // A starved chain gets silence rather than a short block: this node sits
    // in the middle of a live graph and never reports itself finished.
    if (produced == 0) {
        memset(self->buffer, 0, sizeof(self->buffer));
        produced = AUDIODSP_SUBOCTAVE_FRAMES;
    }
    *buffer = (uint8_t *)self->buffer;
    *buffer_length = produced * width;
    return GET_BUFFER_MORE_DATA;
}

static void audiomath_suboctave_reset_buffer(mp_obj_t self_in,
    bool single_channel_output, uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    audiomath_suboctave_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->pending = NULL;
    self->pending_frames = 0;
    // The divider holds no audio, so unlike a delay this drops nothing
    // anybody can hear; what it stops is a restarted chain beginning on the
    // inverted half of the count.
    audiodsp_suboctave_reset(&self->state);
}

// `deinit()` releases what this binding holds and marks the node
// deinitialised, which is what makes every guarded entry point raise
// afterwards -- `audiosample_get_buffer` and `audiosample_reset_buffer` in
// audiocore for the audio path, and the three shared properties. The node
// types audiodsp ported from CircuitPython have had this since they were
// ported; the ones audiodsp wrote itself did not, so no class built on them
// could release one and Tier 1's "deinit() releases every node the class
// built" was unmeasurable on a board (audiodsp#58, #60, #63).
//
// The inline buffers go with the object. What is cleared here is what the
// object holds a *reference* to: the upstream source, so releasing the tail
// of a chain lets the GC reclaim the rest of it, and every borrowed pointer
// into a source's buffer, so nothing dangles.
static mp_obj_t audiomath_suboctave_deinit(mp_obj_t self_in) {
    audiomath_suboctave_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    self->source = mp_const_none;
    self->pending = NULL;
    self->pending_frames = 0;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiomath_suboctave_deinit_obj, audiomath_suboctave_deinit);

static const mp_rom_map_elem_t audiomath_suboctave_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiomath_suboctave_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiomath_suboctave_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&audiomath_suboctave_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear),
      MP_ROM_PTR(&audiomath_suboctave_clear_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiomath_suboctave_locals,
    audiomath_suboctave_locals_table);

static const audiosample_p_t audiomath_suboctave_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = audiomath_suboctave_reset_buffer,
    .get_buffer = audiomath_suboctave_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiomath_suboctave_type,
    MP_QSTR_SubOctave,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiomath_suboctave_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiomath_suboctave_locals,
    protocol, &audiomath_suboctave_proto
    );
