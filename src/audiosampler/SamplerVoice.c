// audiosampler.SamplerVoice. See SamplerVoice.h.
// SPDX-License-Identifier: MIT

#include "audiosampler/SamplerVoice.h"

#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/objproperty.h"
#include "py/runtime.h"
#include "shared/audiodsp_pump_lock.h"

mp_obj_t audiosampler_samplervoice_new(audiosampler_sampler_obj_t *parent,
    uint32_t index) {
    audiosampler_samplervoice_obj_t *self = mp_obj_malloc(
        audiosampler_samplervoice_obj_t, &audiosampler_samplervoice_type);
    self->parent = parent;
    self->index = index;
    self->sample = mp_const_none;
    self->level = mp_obj_new_float(1.0);
    self->pan = mp_obj_new_float(0.0);
    self->pitch = mp_obj_new_float(1.0);
    return MP_OBJ_FROM_PTR(self);
}

static float float_arg(mp_obj_t value, float fallback) {
    return value == mp_const_none ? fallback : (float)mp_obj_get_float(value);
}

// play(buffer, *, rate=None, pitch=1.0, loop_start=0, loop_end=None,
//      loop_mode=LOOP_NONE, level=1.0, pan=0.0, delay=0.0, attack=0.0,
//      hold=0.0, decay=0.0, sustain=1.0, release=0.0)
static mp_obj_t audiosampler_samplervoice_play(size_t n_args,
    const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_buffer, ARG_rate, ARG_pitch, ARG_loop_start, ARG_loop_end,
           ARG_loop_mode, ARG_level, ARG_pan, ARG_delay, ARG_attack, ARG_hold,
           ARG_decay, ARG_sustain, ARG_release };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_buffer, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_rate, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_pitch, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_loop_start, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_loop_end, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_loop_mode, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_level, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_pan, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_delay, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_attack, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_hold, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_decay, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_sustain, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_release, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
    };
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    audiosample_check_for_deinit(&self->parent->base);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t info;
    mp_get_buffer_raise(args[ARG_buffer].u_obj, &info, MP_BUFFER_READ);
    if (info.len < 2 || (info.len % 2) != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "buffer must hold at least one whole 16-bit sample"));
    }
    if (args[ARG_loop_start].u_int < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("loop_start must not be negative"));
    }
    mp_int_t loop_end = 0;
    if (args[ARG_loop_end].u_obj != mp_const_none) {
        loop_end = mp_obj_get_int(args[ARG_loop_end].u_obj);
        if (loop_end < 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("loop_end must not be negative"));
        }
    }

    audiodsp_sampler_note_t note;
    audiodsp_sampler_note_init(&note);
    note.rate = float_arg(args[ARG_rate].u_obj, 0.0f);
    note.pitch = float_arg(args[ARG_pitch].u_obj, 1.0f);
    note.loop_start = (uint32_t)args[ARG_loop_start].u_int;
    note.loop_end = (uint32_t)loop_end;
    note.loop_mode = (uint32_t)args[ARG_loop_mode].u_int;
    note.level = float_arg(args[ARG_level].u_obj, 1.0f);
    note.pan = float_arg(args[ARG_pan].u_obj, 0.0f);
    note.delay = float_arg(args[ARG_delay].u_obj, 0.0f);
    note.attack = float_arg(args[ARG_attack].u_obj, 0.0f);
    note.hold = float_arg(args[ARG_hold].u_obj, 0.0f);
    note.decay = float_arg(args[ARG_decay].u_obj, 0.0f);
    note.sustain = float_arg(args[ARG_sustain].u_obj, 1.0f);
    note.release = float_arg(args[ARG_release].u_obj, 0.0f);

    // Read and checked first, stored under the lock: a pull landing between
    // the kernel's play() and the reference store would read a buffer the
    // GC may already be free to take.
    audiodsp_pump_lock_acquire();
    bool started = audiodsp_sampler_play(&self->parent->sampler, self->index,
        (const int16_t *)info.buf, (uint32_t)(info.len / 2), &note);
    if (started) {
        self->sample = args[ARG_buffer].u_obj;
    }
    audiodsp_pump_lock_release();
    if (!started) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "loop_mode must be 0, 1 or 3 and the loop must fit the buffer"));
    }
    self->level = args[ARG_level].u_obj == mp_const_none
        ? mp_obj_new_float(1.0) : args[ARG_level].u_obj;
    self->pan = args[ARG_pan].u_obj == mp_const_none
        ? mp_obj_new_float(0.0) : args[ARG_pan].u_obj;
    self->pitch = args[ARG_pitch].u_obj == mp_const_none
        ? mp_obj_new_float(1.0) : args[ARG_pitch].u_obj;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audiosampler_samplervoice_play_obj, 1,
    audiosampler_samplervoice_play);

// Let the key go: enter the release stage. In LOOP_UNTIL_RELEASE the voice
// also stops looping and plays on to the end.
static mp_obj_t audiosampler_samplervoice_release(mp_obj_t self_in) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    audiodsp_pump_lock_acquire();
    audiodsp_sampler_release(&self->parent->sampler, self->index);
    if (!audiodsp_sampler_playing(&self->parent->sampler, self->index)) {
        self->sample = mp_const_none;
    }
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_samplervoice_release_obj,
    audiosampler_samplervoice_release);

static mp_obj_t audiosampler_samplervoice_stop(mp_obj_t self_in) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    audiodsp_pump_lock_acquire();
    audiodsp_sampler_stop(&self->parent->sampler, self->index);
    self->sample = mp_const_none;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_samplervoice_stop_obj,
    audiosampler_samplervoice_stop);

static mp_obj_t audiosampler_samplervoice_obj_get_playing(mp_obj_t self_in) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    return mp_obj_new_bool(
        audiodsp_sampler_playing(&self->parent->sampler, self->index));
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_samplervoice_get_playing_obj,
    audiosampler_samplervoice_obj_get_playing);
MP_PROPERTY_GETTER(audiosampler_samplervoice_playing_obj,
    (mp_obj_t)&audiosampler_samplervoice_get_playing_obj);

static mp_obj_t audiosampler_samplervoice_obj_get_level(mp_obj_t self_in) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    return self->level;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_samplervoice_get_level_obj,
    audiosampler_samplervoice_obj_get_level);
static mp_obj_t audiosampler_samplervoice_obj_set_level(mp_obj_t self_in,
    mp_obj_t value) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    float level = (float)mp_obj_get_float(value);
    audiodsp_pump_lock_acquire();
    audiodsp_sampler_set_level(&self->parent->sampler, self->index, level);
    self->level = value;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiosampler_samplervoice_set_level_obj,
    audiosampler_samplervoice_obj_set_level);
MP_PROPERTY_GETSET(audiosampler_samplervoice_level_obj,
    (mp_obj_t)&audiosampler_samplervoice_get_level_obj,
    (mp_obj_t)&audiosampler_samplervoice_set_level_obj);

static mp_obj_t audiosampler_samplervoice_obj_get_pan(mp_obj_t self_in) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    return self->pan;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_samplervoice_get_pan_obj,
    audiosampler_samplervoice_obj_get_pan);
static mp_obj_t audiosampler_samplervoice_obj_set_pan(mp_obj_t self_in,
    mp_obj_t value) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    float pan = (float)mp_obj_get_float(value);
    audiodsp_pump_lock_acquire();
    audiodsp_sampler_set_pan(&self->parent->sampler, self->index, pan);
    self->pan = value;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiosampler_samplervoice_set_pan_obj,
    audiosampler_samplervoice_obj_set_pan);
MP_PROPERTY_GETSET(audiosampler_samplervoice_pan_obj,
    (mp_obj_t)&audiosampler_samplervoice_get_pan_obj,
    (mp_obj_t)&audiosampler_samplervoice_set_pan_obj);

static mp_obj_t audiosampler_samplervoice_obj_get_pitch(mp_obj_t self_in) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    return self->pitch;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_samplervoice_get_pitch_obj,
    audiosampler_samplervoice_obj_get_pitch);
static mp_obj_t audiosampler_samplervoice_obj_set_pitch(mp_obj_t self_in,
    mp_obj_t value) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    float pitch = (float)mp_obj_get_float(value);
    audiodsp_pump_lock_acquire();
    audiodsp_sampler_set_pitch(&self->parent->sampler, self->index, pitch);
    self->pitch = value;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiosampler_samplervoice_set_pitch_obj,
    audiosampler_samplervoice_obj_set_pitch);
MP_PROPERTY_GETSET(audiosampler_samplervoice_pitch_obj,
    (mp_obj_t)&audiosampler_samplervoice_get_pitch_obj,
    (mp_obj_t)&audiosampler_samplervoice_set_pitch_obj);

// `deinit()` on a voice is `stop()` plus letting go of the buffer: a voice
// has nothing of its own to release, it belongs to its Sampler, but every
// type in these bindings answers deinit() so a caller can release whatever
// it holds without asking what it is.
static mp_obj_t audiosampler_samplervoice_deinit(mp_obj_t self_in) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_acquire();
    audiodsp_sampler_stop(&self->parent->sampler, self->index);
    self->sample = mp_const_none;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_samplervoice_deinit_obj,
    audiosampler_samplervoice_deinit);

static const mp_rom_map_elem_t audiosampler_samplervoice_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit),
      MP_ROM_PTR(&audiosampler_samplervoice_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiosampler_samplervoice_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_release),
      MP_ROM_PTR(&audiosampler_samplervoice_release_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audiosampler_samplervoice_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_playing),
      MP_ROM_PTR(&audiosampler_samplervoice_playing_obj) },
    { MP_ROM_QSTR(MP_QSTR_level), MP_ROM_PTR(&audiosampler_samplervoice_level_obj) },
    { MP_ROM_QSTR(MP_QSTR_pan), MP_ROM_PTR(&audiosampler_samplervoice_pan_obj) },
    { MP_ROM_QSTR(MP_QSTR_pitch), MP_ROM_PTR(&audiosampler_samplervoice_pitch_obj) },
};
static MP_DEFINE_CONST_DICT(audiosampler_samplervoice_locals,
    audiosampler_samplervoice_locals_table);

// No make_new: a voice belongs to the Sampler that made it.
MP_DEFINE_CONST_OBJ_TYPE(
    audiosampler_samplervoice_type,
    MP_QSTR_SamplerVoice,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    attr, cp_compat_attr,
    locals_dict, &audiosampler_samplervoice_locals
    );
