// audiobiquad.Biquad. See Biquad.h for provenance.
// SPDX-License-Identifier: MIT

#include "audiobiquad/Biquad.h"

#include <string.h>

#include "cp_compat/objproperty.h"
#include "cp_compat/context_manager_helpers.h"
#include "py/runtime.h"
#include "shared/audiodsp_pump_lock.h"

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
        { MP_QSTR_gain_db, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(0)} },
        { MP_QSTR_mix, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(1)} },
        { MP_QSTR_sample_rate, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 48000} },
        { MP_QSTR_channel_count, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 2} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t channel_count = args[ARG_channel_count].u_int;
    if (channel_count < 1 || channel_count > 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("channel_count must be 1 or 2"));
    }
    if (args[ARG_sample_rate].u_int < 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("sample_rate must be at least 1"));
    }

    audiobiquad_biquad_obj_t *self =
        mp_obj_malloc(audiobiquad_biquad_obj_t, type);
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

    // The default Q is the Butterworth one, spelled as a number rather than
    // as `None` so a caller can read it back.
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

// Read every block input and hand the values to the kernel, which clamps
// them. Doing the clamping in one place is what keeps the four targets
// identical: the CPython twin passes the raw values down too.
static void biquad_refresh(audiobiquad_biquad_obj_t *self) {
    audiodsp_biquad_f32_configure(&self->config,
        AUDIODSP_BIQUAD_F32_OPT_FREQUENCY,
        (float)synthio_block_slot_get(&self->frequency));
    audiodsp_biquad_f32_configure(&self->config, AUDIODSP_BIQUAD_F32_OPT_Q,
        (float)synthio_block_slot_get(&self->Q));
    audiodsp_biquad_f32_configure(&self->config,
        AUDIODSP_BIQUAD_F32_OPT_GAIN_DB,
        (float)synthio_block_slot_get(&self->gain_db));
    audiodsp_biquad_f32_configure(&self->config, AUDIODSP_BIQUAD_F32_OPT_MIX,
        (float)synthio_block_slot_get(&self->mix));
    audiodsp_biquad_f32_config_finish(&self->config);
}

// One chunk of the block layer, then the values it produced.
static void biquad_apply_blocks(audiobiquad_biquad_obj_t *self,
    uint32_t frames) {
    shared_bindings_synthio_lfo_tick(self->base.sample_rate,
        (uint16_t)frames);
    biquad_refresh(self);
}

static mp_obj_t audiobiquad_biquad_play(mp_obj_t self_in, mp_obj_t sample) {
    audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in);
    (void)audiosample_check(sample);
    audiodsp_pump_lock_acquire();
    self->source = sample;
    self->pending = NULL;
    self->pending_frames = 0;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiobiquad_biquad_play_obj,
    audiobiquad_biquad_play);

static mp_obj_t audiobiquad_biquad_stop(mp_obj_t self_in) {
    audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_acquire();
    self->source = MP_OBJ_NULL;
    self->pending = NULL;
    self->pending_frames = 0;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiobiquad_biquad_stop_obj,
    audiobiquad_biquad_stop);

static mp_obj_t audiobiquad_biquad_clear(mp_obj_t self_in) {
    audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_acquire();
    audiodsp_biquad_f32_reset(&self->state);
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiobiquad_biquad_clear_obj,
    audiobiquad_biquad_clear);

static mp_obj_t audiobiquad_biquad_obj_get_playing(mp_obj_t self_in) {
    audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(self->source != MP_OBJ_NULL);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiobiquad_biquad_get_playing_obj,
    audiobiquad_biquad_obj_get_playing);
MP_PROPERTY_GETTER(audiobiquad_biquad_playing_obj,
    (mp_obj_t)&audiobiquad_biquad_get_playing_obj);

#define BIQUAD_SLOT_PROPERTY(name) \
    static mp_obj_t audiobiquad_biquad_obj_get_##name(mp_obj_t self_in) { \
        audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in); \
        return self->name.obj; \
    } \
    static MP_DEFINE_CONST_FUN_OBJ_1( \
        audiobiquad_biquad_get_##name##_obj, \
        audiobiquad_biquad_obj_get_##name); \
    static mp_obj_t audiobiquad_biquad_obj_set_##name(mp_obj_t self_in, \
        mp_obj_t value) { \
        audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in); \
        synthio_block_assign_slot(value, &self->name, MP_QSTR_##name); \
        return mp_const_none; \
    } \
    static MP_DEFINE_CONST_FUN_OBJ_2( \
        audiobiquad_biquad_set_##name##_obj, \
        audiobiquad_biquad_obj_set_##name); \
    MP_PROPERTY_GETSET(audiobiquad_biquad_##name##_obj, \
        (mp_obj_t)&audiobiquad_biquad_get_##name##_obj, \
        (mp_obj_t)&audiobiquad_biquad_set_##name##_obj)

BIQUAD_SLOT_PROPERTY(frequency);
BIQUAD_SLOT_PROPERTY(Q);
BIQUAD_SLOT_PROPERTY(gain_db);
BIQUAD_SLOT_PROPERTY(mix);

static mp_obj_t audiobiquad_biquad_obj_get_mode(mp_obj_t self_in) {
    audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->config.mode);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiobiquad_biquad_get_mode_obj,
    audiobiquad_biquad_obj_get_mode);

static mp_obj_t audiobiquad_biquad_obj_set_mode(mp_obj_t self_in,
    mp_obj_t value) {
    audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in);
    biquad_set_mode(self, mp_obj_get_int(value));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiobiquad_biquad_set_mode_obj,
    audiobiquad_biquad_obj_set_mode);
MP_PROPERTY_GETSET(audiobiquad_biquad_mode_obj,
    (mp_obj_t)&audiobiquad_biquad_get_mode_obj,
    (mp_obj_t)&audiobiquad_biquad_set_mode_obj);

// (b0, b1, b2, a1, a2), normalized, at the settings in force -- what a test
// compares with shared/audiodsp_biquad.c's fixed-point five without rendering.
// Reads the block inputs but does not advance them, so looking at this does
// not move an LFO along.
static mp_obj_t audiobiquad_biquad_obj_get_coefficients(mp_obj_t self_in) {
    audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in);
    biquad_refresh(self);
    mp_obj_t items[5] = {
        mp_obj_new_float((mp_float_t)self->config.b0),
        mp_obj_new_float((mp_float_t)self->config.b1),
        mp_obj_new_float((mp_float_t)self->config.b2),
        mp_obj_new_float((mp_float_t)self->config.a1),
        mp_obj_new_float((mp_float_t)self->config.a2),
    };
    return mp_obj_new_tuple(5, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiobiquad_biquad_get_coefficients_obj,
    audiobiquad_biquad_obj_get_coefficients);
MP_PROPERTY_GETTER(audiobiquad_biquad_coefficients_obj,
    (mp_obj_t)&audiobiquad_biquad_get_coefficients_obj);

static audioio_get_buffer_result_t audiobiquad_biquad_get_buffer(
    mp_obj_t self_in, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint32_t produced = 0;
    while (produced < AUDIODSP_FILTER_F32_FRAMES) {
        if (self->pending_frames == 0) {
            if (self->source == MP_OBJ_NULL) {
                break;
            }
            uint8_t *raw = NULL;
            uint32_t raw_bytes = 0;
            audioio_get_buffer_result_t result = audiosample_get_buffer(
                self->source, false, 0, &raw, &raw_bytes);
            const uint32_t width = 2u * self->base.channel_count;
            if (result == GET_BUFFER_ERROR || raw == NULL ||
                raw_bytes < width) {
                break;
            }
            self->pending = (const int16_t *)raw;
            self->pending_frames = raw_bytes / width;
        }
        uint32_t run = AUDIODSP_FILTER_F32_FRAMES - produced;
        if (run > self->pending_frames) {
            run = self->pending_frames;
        }
        biquad_apply_blocks(self, run);
        audiodsp_biquad_f32_process_s16(&self->config, &self->state,
            &self->buffer[produced * self->base.channel_count],
            self->pending, run);
        self->pending += run * self->base.channel_count;
        self->pending_frames -= run;
        produced += run;
    }
    // A starved chain gets silence rather than a short block: this node sits
    // in the middle of a live graph and never reports itself finished. It
    // does not tick the block layer on a starved block either -- the tail
    // stops with the source, exactly as audioecho.FeedbackDelay's does.
    if (produced == 0) {
        memset(self->buffer, 0, sizeof(self->buffer));
        produced = AUDIODSP_FILTER_F32_FRAMES;
    }
    *buffer = (uint8_t *)self->buffer;
    *buffer_length = produced * 2u * self->base.channel_count;
    return GET_BUFFER_MORE_DATA;
}

static void audiobiquad_biquad_reset_buffer(mp_obj_t self_in,
    bool single_channel_output, uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->pending = NULL;
    self->pending_frames = 0;
    // Everything goes. A filter's memory is audible: a chain restarted with
    // the previous take still in it plays that take's tail over the new one.
    audiodsp_biquad_f32_reset(&self->state);
    // No unlock here, and there never was a lock to match it. reset_buffer is
    // reached through audiocore's funnel, which holds the pump lock across the
    // whole call -- so this body is already protected, and the release that
    // used to close it was DROPPING THE FUNNEL'S HOLD. Windows does not check
    // who is unlocking a CRITICAL_SECTION: each stray one takes its
    // RecursionCount further below zero until the section is one nobody can
    // enter, and the next control call never returns.
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
static mp_obj_t audiobiquad_biquad_deinit(mp_obj_t self_in) {
    audiobiquad_biquad_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    self->source = mp_const_none;
    self->pending = NULL;
    self->pending_frames = 0;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiobiquad_biquad_deinit_obj, audiobiquad_biquad_deinit);

static const mp_rom_map_elem_t audiobiquad_biquad_locals_table[] = {
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
static MP_DEFINE_CONST_DICT(audiobiquad_biquad_locals,
    audiobiquad_biquad_locals_table);

// What is behind this node, for audiosample_find_type()'s walk.
// audiodsp#112: the pump refuses a file-backed source at handover,
// and the protocol had no way to look past the tail.
static mp_obj_t audiobiquad_biquad_sources(mp_obj_t self_in, uint8_t index) {
    return index == 0 ? ((audiobiquad_biquad_obj_t *)MP_OBJ_TO_PTR(self_in))->source
                      : MP_OBJ_NULL;
}

static const audiosample_p_t audiobiquad_biquad_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = audiobiquad_biquad_reset_buffer,
    .get_buffer = audiobiquad_biquad_get_buffer,
    .sources = audiobiquad_biquad_sources,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiobiquad_biquad_type,
    MP_QSTR_Biquad,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiobiquad_biquad_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiobiquad_biquad_locals,
    protocol, &audiobiquad_biquad_proto
    );
