// audiofm.Synth. See Synth.h.
// SPDX-License-Identifier: MIT

#include "audiofm/Synth.h"

#include <string.h>

#include "cp_compat/context_manager_helpers.h"
#include "py/runtime.h"
#include "shared/audiodsp_pump_lock.h"

static audiofm_patch_obj_t *audiofm_check_patch(mp_obj_t value) {
    if (!mp_obj_is_type(value, &audiofm_patch_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("patch must be an audiofm.Patch"));
    }
    return MP_OBJ_TO_PTR(value);
}

static mp_obj_t audiofm_synth_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_voices, ARG_sample_rate, ARG_channel_count, ARG_patch };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_voices, MP_ARG_INT, {.u_int = 16} },
        { MP_QSTR_sample_rate, MP_ARG_INT, {.u_int = 48000} },
        { MP_QSTR_channel_count, MP_ARG_INT, {.u_int = 1} },
        { MP_QSTR_patch, MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t voices = args[ARG_voices].u_int;
    mp_int_t sample_rate = args[ARG_sample_rate].u_int;
    mp_int_t channel_count = args[ARG_channel_count].u_int;
    if (voices < 1 || voices > (mp_int_t)AUDIODSP_FM_MAX_VOICES) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("voices must be 1 to %d"),
            (int)AUDIODSP_FM_MAX_VOICES);
    }
    if (channel_count < 1 || channel_count > 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("channel_count must be 1 or 2"));
    }
    if (sample_rate < 1000) {
        mp_raise_ValueError(MP_ERROR_TEXT("sample_rate must be at least 1000"));
    }
    audiofm_patch_obj_t *patch = args[ARG_patch].u_obj == mp_const_none ?
        audiofm_patch_new() : audiofm_check_patch(args[ARG_patch].u_obj);

    audiofm_synth_obj_t *self = mp_obj_malloc(audiofm_synth_obj_t, type);
    self->base.sample_rate = (uint32_t)sample_rate;
    self->base.max_buffer_length = AUDIODSP_FM_FRAMES * 2u *
        (uint32_t)channel_count;
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)channel_count;
    self->base.samples_signed = 1;
    self->base.single_buffer = false;
    self->voices = m_malloc((size_t)voices * sizeof(audiodsp_fm_voice_t));
    audiodsp_fm_init(&self->synth, (uint32_t)sample_rate,
        (uint32_t)channel_count, self->voices, (uint32_t)voices);
    self->patch = patch;
    self->bend = 0;
    self->mod_wheel = 0;
    self->bend_range = 2;
    self->sustain = false;
    return MP_OBJ_FROM_PTR(self);
}

static mp_int_t audiofm_note_arg(mp_obj_t value, qstr name) {
    mp_int_t v = mp_obj_get_int(value);
    if (v < 0 || v > 127) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("%q must be 0 to 127"), name);
    }
    return v;
}

// press(note, velocity=100)
static mp_obj_t audiofm_synth_press(size_t n_args, const mp_obj_t *args) {
    audiofm_synth_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    audiosample_check_for_deinit(&self->base);
    mp_int_t note = audiofm_note_arg(args[1], MP_QSTR_note);
    mp_int_t velocity = n_args > 2 ?
        audiofm_note_arg(args[2], MP_QSTR_velocity) : 100;
    // The patch is read now, under the lock, so an edit to it reaches the
    // next note and never one already sounding.
    audiodsp_pump_lock_acquire();
    audiodsp_fm_press(&self->synth, self->patch->data, (uint8_t)note,
        (uint8_t)velocity);
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiofm_synth_press_obj, 2, 3,
    audiofm_synth_press);

static mp_obj_t audiofm_synth_release(mp_obj_t self_in, mp_obj_t note_in) {
    audiofm_synth_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    mp_int_t note = audiofm_note_arg(note_in, MP_QSTR_note);
    audiodsp_pump_lock_acquire();
    audiodsp_fm_release(&self->synth, (uint8_t)note);
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiofm_synth_release_obj,
    audiofm_synth_release);

static mp_obj_t audiofm_synth_release_all(mp_obj_t self_in) {
    audiofm_synth_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    audiodsp_pump_lock_acquire();
    audiodsp_fm_release_all(&self->synth);
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofm_synth_release_all_obj,
    audiofm_synth_release_all);

// --- properties ----------------------------------------------------------------

static mp_obj_t audiofm_synth_get_patch(mp_obj_t self_in) {
    audiofm_synth_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return MP_OBJ_FROM_PTR(self->patch);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofm_synth_get_patch_obj,
    audiofm_synth_get_patch);

static mp_obj_t audiofm_synth_set_patch(mp_obj_t self_in, mp_obj_t value) {
    audiofm_synth_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    audiofm_patch_obj_t *patch = audiofm_check_patch(value);
    audiodsp_pump_lock_acquire();
    self->patch = patch;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiofm_synth_set_patch_obj,
    audiofm_synth_set_patch);
MP_PROPERTY_GETSET(audiofm_synth_patch_obj,
    (mp_obj_t)&audiofm_synth_get_patch_obj,
    (mp_obj_t)&audiofm_synth_set_patch_obj);

// The bend goes to the kernel as whole 14-bit steps times whole semitones,
// in integers, so every runtime computes the same Q24 offset.
static void audiofm_apply_bend(audiofm_synth_obj_t *self) {
    mp_int_t steps = (mp_int_t)(self->bend * 8192);
    steps = steps < -8192 ? -8192 : steps > 8191 ? 8191 : steps;
    int32_t q24 = (int32_t)((steps * (mp_int_t)self->bend_range * 1398101) >>
        13);
    audiodsp_pump_lock_acquire();
    audiodsp_fm_set_bend(&self->synth, q24);
    audiodsp_pump_lock_release();
}

static mp_obj_t audiofm_synth_get_pitch_bend(mp_obj_t self_in) {
    audiofm_synth_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return mp_obj_new_float(self->bend);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofm_synth_get_pitch_bend_obj,
    audiofm_synth_get_pitch_bend);

static mp_obj_t audiofm_synth_set_pitch_bend(mp_obj_t self_in,
    mp_obj_t value) {
    audiofm_synth_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    mp_float_t bend = mp_obj_get_float(value);
    self->bend = bend < -1 ? -1 : bend > 1 ? 1 : bend;
    audiofm_apply_bend(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiofm_synth_set_pitch_bend_obj,
    audiofm_synth_set_pitch_bend);
MP_PROPERTY_GETSET(audiofm_synth_pitch_bend_obj,
    (mp_obj_t)&audiofm_synth_get_pitch_bend_obj,
    (mp_obj_t)&audiofm_synth_set_pitch_bend_obj);

static mp_obj_t audiofm_synth_get_bend_range(mp_obj_t self_in) {
    audiofm_synth_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return MP_OBJ_NEW_SMALL_INT(self->bend_range);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofm_synth_get_bend_range_obj,
    audiofm_synth_get_bend_range);

static mp_obj_t audiofm_synth_set_bend_range(mp_obj_t self_in,
    mp_obj_t value) {
    audiofm_synth_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    mp_int_t range = mp_obj_get_int(value);
    if (range < 0 || range > 24) {
        mp_raise_ValueError(MP_ERROR_TEXT("bend_range must be 0 to 24"));
    }
    self->bend_range = (uint8_t)range;
    audiofm_apply_bend(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiofm_synth_set_bend_range_obj,
    audiofm_synth_set_bend_range);
MP_PROPERTY_GETSET(audiofm_synth_bend_range_obj,
    (mp_obj_t)&audiofm_synth_get_bend_range_obj,
    (mp_obj_t)&audiofm_synth_set_bend_range_obj);

static mp_obj_t audiofm_synth_get_mod_wheel(mp_obj_t self_in) {
    audiofm_synth_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return mp_obj_new_float(self->mod_wheel);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofm_synth_get_mod_wheel_obj,
    audiofm_synth_get_mod_wheel);

static mp_obj_t audiofm_synth_set_mod_wheel(mp_obj_t self_in,
    mp_obj_t value) {
    audiofm_synth_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    mp_float_t wheel = mp_obj_get_float(value);
    self->mod_wheel = wheel < 0 ? 0 : wheel > 1 ? 1 : wheel;
    uint8_t steps = (uint8_t)(mp_int_t)(self->mod_wheel * 127 +
        (mp_float_t)0.5);
    audiodsp_pump_lock_acquire();
    audiodsp_fm_set_mod_wheel(&self->synth, steps);
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiofm_synth_set_mod_wheel_obj,
    audiofm_synth_set_mod_wheel);
MP_PROPERTY_GETSET(audiofm_synth_mod_wheel_obj,
    (mp_obj_t)&audiofm_synth_get_mod_wheel_obj,
    (mp_obj_t)&audiofm_synth_set_mod_wheel_obj);

static mp_obj_t audiofm_synth_get_sustain(mp_obj_t self_in) {
    audiofm_synth_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return mp_obj_new_bool(self->sustain);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofm_synth_get_sustain_obj,
    audiofm_synth_get_sustain);

static mp_obj_t audiofm_synth_set_sustain(mp_obj_t self_in, mp_obj_t value) {
    audiofm_synth_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    self->sustain = mp_obj_is_true(value);
    audiodsp_pump_lock_acquire();
    audiodsp_fm_set_sustain(&self->synth, self->sustain);
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiofm_synth_set_sustain_obj,
    audiofm_synth_set_sustain);
MP_PROPERTY_GETSET(audiofm_synth_sustain_obj,
    (mp_obj_t)&audiofm_synth_get_sustain_obj,
    (mp_obj_t)&audiofm_synth_set_sustain_obj);

static mp_obj_t audiofm_synth_get_voices(mp_obj_t self_in) {
    audiofm_synth_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return MP_OBJ_NEW_SMALL_INT(self->synth.voice_count);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofm_synth_get_voices_obj,
    audiofm_synth_get_voices);
MP_PROPERTY_GETTER(audiofm_synth_voices_obj,
    (mp_obj_t)&audiofm_synth_get_voices_obj);

static mp_obj_t audiofm_synth_get_active(mp_obj_t self_in) {
    audiofm_synth_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return MP_OBJ_NEW_SMALL_INT(audiodsp_fm_active(&self->synth));
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofm_synth_get_active_obj,
    audiofm_synth_get_active);
MP_PROPERTY_GETTER(audiofm_synth_active_obj,
    (mp_obj_t)&audiofm_synth_get_active_obj);

// --- the audiosample protocol --------------------------------------------------

static audioio_get_buffer_result_t audiofm_synth_get_buffer(mp_obj_t self_in,
    bool single_channel_output, uint8_t channel, uint8_t **buffer,
    uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    audiofm_synth_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_fm_render_s16(&self->synth, self->buffer, AUDIODSP_FM_FRAMES);
    *buffer = (uint8_t *)self->buffer;
    *buffer_length = AUDIODSP_FM_FRAMES * 2u * self->base.channel_count;
    return GET_BUFFER_MORE_DATA;
}

// A host reset leaves the notes alone: a synth has no position to rewind,
// and dropping held notes because the output restarted would be a surprise.
static void audiofm_synth_reset_buffer(mp_obj_t self_in,
    bool single_channel_output, uint8_t channel) {
    (void)self_in;
    (void)single_channel_output;
    (void)channel;
}

static mp_obj_t audiofm_synth_deinit(mp_obj_t self_in) {
    audiofm_synth_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    audiodsp_fm_reset(&self->synth);
    // The voices are most of what a synth costs. Dropping the only pointer
    // to them lets the next collection take them back, rather than waiting
    // for the last reference to the object to go.
    self->synth.voices = NULL;
    self->synth.voice_count = 0;
    self->voices = NULL;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofm_synth_deinit_obj,
    audiofm_synth_deinit);

static const mp_rom_map_elem_t audiofm_synth_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiofm_synth_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_press), MP_ROM_PTR(&audiofm_synth_press_obj) },
    { MP_ROM_QSTR(MP_QSTR_release), MP_ROM_PTR(&audiofm_synth_release_obj) },
    { MP_ROM_QSTR(MP_QSTR_release_all),
      MP_ROM_PTR(&audiofm_synth_release_all_obj) },
    { MP_ROM_QSTR(MP_QSTR_patch), MP_ROM_PTR(&audiofm_synth_patch_obj) },
    { MP_ROM_QSTR(MP_QSTR_pitch_bend),
      MP_ROM_PTR(&audiofm_synth_pitch_bend_obj) },
    { MP_ROM_QSTR(MP_QSTR_bend_range),
      MP_ROM_PTR(&audiofm_synth_bend_range_obj) },
    { MP_ROM_QSTR(MP_QSTR_mod_wheel),
      MP_ROM_PTR(&audiofm_synth_mod_wheel_obj) },
    { MP_ROM_QSTR(MP_QSTR_sustain), MP_ROM_PTR(&audiofm_synth_sustain_obj) },
    { MP_ROM_QSTR(MP_QSTR_voices), MP_ROM_PTR(&audiofm_synth_voices_obj) },
    { MP_ROM_QSTR(MP_QSTR_active), MP_ROM_PTR(&audiofm_synth_active_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiofm_synth_locals, audiofm_synth_locals_table);

static const audiosample_p_t audiofm_synth_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = audiofm_synth_reset_buffer,
    .get_buffer = audiofm_synth_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiofm_synth_type,
    MP_QSTR_Synth,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiofm_synth_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiofm_synth_locals,
    protocol, &audiofm_synth_proto
    );
