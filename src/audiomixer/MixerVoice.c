// Ported from CircuitPython's shared-bindings/audiomixer/MixerVoice.c and
// shared-module/audiomixer/MixerVoice.c (upstream repo:
// https://github.com/adafruit/circuitpython, MIT), merged into one file
// (this port doesn't keep CP's shared-bindings/shared-module split).
//
// Deviation from upstream: level/panning are unconditionally
// synthio_block_slot_t (this port always has synthio) -- see MixerVoice.h.
// The dropped upstream shared-module include of
// shared-module/audiocore/RawSample.h was dead (nothing in this file
// references a RawSample symbol), same as audiocore/__init__.c's dropped
// includes -- verified via grep before dropping.
//
// SPDX-FileCopyrightText: Copyright (c) 2018 DeanM for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "audiomixer/Mixer.h"
#include "audiomixer/MixerVoice.h"
#include "cp_compat/objproperty.h"

#include "py/runtime.h"

static mp_obj_t audiomixer_mixervoice_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, false);

    audiomixer_mixervoice_obj_t *self = mp_obj_malloc(audiomixer_mixervoice_obj_t, &audiomixer_mixervoice_type);
    common_hal_audiomixer_mixervoice_construct(self);

    return MP_OBJ_FROM_PTR(self);
}

void common_hal_audiomixer_mixervoice_construct(audiomixer_mixervoice_obj_t *self) {
    self->sample = NULL;
    common_hal_audiomixer_mixervoice_set_level(self, mp_obj_new_float(1.0));
    common_hal_audiomixer_mixervoice_set_panning(self, mp_obj_new_float(0.0));
}

void common_hal_audiomixer_mixervoice_set_parent(audiomixer_mixervoice_obj_t *self, audiomixer_mixer_obj_t *parent) {
    self->parent = parent;
}

mp_obj_t common_hal_audiomixer_mixervoice_get_level(audiomixer_mixervoice_obj_t *self) {
    return self->level.obj;
}

void common_hal_audiomixer_mixervoice_set_level(audiomixer_mixervoice_obj_t *self, mp_obj_t arg) {
    synthio_block_assign_slot(arg, &self->level, MP_QSTR_level);
}

mp_obj_t common_hal_audiomixer_mixervoice_get_panning(audiomixer_mixervoice_obj_t *self) {
    return self->panning.obj;
}

void common_hal_audiomixer_mixervoice_set_panning(audiomixer_mixervoice_obj_t *self, mp_obj_t arg) {
    synthio_block_assign_slot(arg, &self->panning, MP_QSTR_panning);
}

bool common_hal_audiomixer_mixervoice_get_loop(audiomixer_mixervoice_obj_t *self) {
    return self->loop;
}

void common_hal_audiomixer_mixervoice_set_loop(audiomixer_mixervoice_obj_t *self, bool loop) {
    self->loop = loop;
}

void common_hal_audiomixer_mixervoice_play(audiomixer_mixervoice_obj_t *self, mp_obj_t sample_in, bool loop) {
    audiosample_must_match(&self->parent->base, sample_in, true);
    // cast is safe, checked by must_match
    audiosample_base_t *sample = MP_OBJ_TO_PTR(sample_in);
    self->sample = sample;
    self->loop = loop;

    common_hal_audiomixer_mixervoice_reset(self);

    // A LOOPING SOURCE THAT CANNOT FILL ONE PACKED WORD NEVER ENDS. A voice's
    // buffer is tracked in WORDS -- `buffer_length /= sizeof(uint32_t)` just
    // above, and again in Mixer.c -- so a two-byte sample measures ZERO words,
    // each pass of the mix-down consumes zero of them, and the fill does not
    // advance. Looping is what makes it unbounded: without it the voice
    // reaches the not-more-data-and-not-looping exit and stops. audioif#85.
    //
    // The condition is read off the fetch reset() just did rather than off the
    // sample's declared length, because that is what the CPython twin can see
    // too: on that target a source is any object with `_get_buffer`, and
    // `max_buffer_length` is not part of the surface. Same rule, same moment,
    // both targets -- which also catches a source whose declared buffer is
    // ample and whose content is not, such as a one-frame WaveFile.
    //
    // REFUSED RATHER THAN PADDED, on two counts. Padding a one-frame mono loop
    // up to two frames halves its loop rate, which for anything but silence is
    // a different sound delivered without a word said; and the padding would
    // have to be a COPY, where `RawSample` deliberately holds the caller's
    // buffer so that writes to that array are heard. Refusing is loud and
    // reversible; a pad is neither. ValueError, because that is already what
    // play() raises for a sample this mixer cannot take (audiosample_must_match
    // just above), so a caller's existing except clause covers it.
    //
    // `voice.loop = True` set AFTER play() is deliberately NOT guarded: the
    // voice's buffer is legitimately empty at the end of any sample, so there
    // is no honest way to tell that case from this one at that moment. The
    // mix-down backstop in Mixer.c covers it -- the voice stops rather than
    // spinning -- which is what the CPython twin has done since audioif#24.
    if (loop && self->buffer_length == 0 && !self->more_data) {
        // A refused play() leaves the voice stopped, not half-started.
        self->sample = NULL;
        self->loop = false;
        mp_raise_ValueError(MP_ERROR_TEXT("A looped sample must fill at least one 32-bit word"));
    }
}

bool common_hal_audiomixer_mixervoice_get_playing(audiomixer_mixervoice_obj_t *self) {
    return self->sample != NULL;
}

void common_hal_audiomixer_mixervoice_stop(audiomixer_mixervoice_obj_t *self) {
    self->sample = NULL;
}

// Rewind to the start of whatever is playing, keeping it playing. New here:
// upstream has no equivalent, because its Mixer.reset_buffer stops the voices
// instead. See docs/upstream-diff.md.
void common_hal_audiomixer_mixervoice_reset(audiomixer_mixervoice_obj_t *self) {
    if (self->sample == NULL) {
        return;
    }
    audiosample_reset_buffer(self->sample, false, 0);
    audioio_get_buffer_result_t result = audiosample_get_buffer(self->sample, false, 0, (uint8_t **)&self->remaining_buffer, &self->buffer_length);
    // Track length in terms of words.
    self->buffer_length /= sizeof(uint32_t);
    self->more_data = result == GET_BUFFER_MORE_DATA;
}

void common_hal_audiomixer_mixervoice_end(audiomixer_mixervoice_obj_t *self) {
    if (self->sample != NULL) {
        self->loop = false;
    }
}

static mp_obj_t audiomixer_mixervoice_obj_play(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_sample, ARG_loop };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sample,    MP_ARG_OBJ | MP_ARG_REQUIRED, {} },
        { MP_QSTR_loop,      MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
    };
    audiomixer_mixervoice_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_obj_t sample = args[ARG_sample].u_obj;
    common_hal_audiomixer_mixervoice_play(self, sample, args[ARG_loop].u_bool);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audiomixer_mixervoice_play_obj, 1, audiomixer_mixervoice_obj_play);

static mp_obj_t audiomixer_mixervoice_obj_stop(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    // Upstream declares an unused "voice" arg here (MixerVoice.stop() takes
    // none) -- kept verbatim rather than "fixed", since it's parseable
    // input-compat behavior (an ignored voice= kwarg), not a bug that
    // affects output.
    enum { ARG_voice };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_voice, MP_ARG_INT, {.u_int = 0} },
    };
    audiomixer_mixervoice_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    common_hal_audiomixer_mixervoice_stop(self);

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audiomixer_mixervoice_stop_obj, 1, audiomixer_mixervoice_obj_stop);

static mp_obj_t audiomixer_mixervoice_obj_end(mp_obj_t self_in) {
    audiomixer_mixervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiomixer_mixervoice_end(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiomixer_mixervoice_end_obj, audiomixer_mixervoice_obj_end);

static mp_obj_t audiomixer_mixervoice_obj_get_level(mp_obj_t self_in) {
    return common_hal_audiomixer_mixervoice_get_level(self_in);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiomixer_mixervoice_get_level_obj, audiomixer_mixervoice_obj_get_level);

static mp_obj_t audiomixer_mixervoice_obj_set_level(mp_obj_t self_in, mp_obj_t level_in) {
    audiomixer_mixervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiomixer_mixervoice_set_level(self, level_in);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiomixer_mixervoice_set_level_obj, audiomixer_mixervoice_obj_set_level);

MP_PROPERTY_GETSET(audiomixer_mixervoice_level_obj,
    (mp_obj_t)&audiomixer_mixervoice_get_level_obj,
    (mp_obj_t)&audiomixer_mixervoice_set_level_obj);

static mp_obj_t audiomixer_mixervoice_obj_get_panning(mp_obj_t self_in) {
    return common_hal_audiomixer_mixervoice_get_panning(self_in);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiomixer_mixervoice_get_panning_obj, audiomixer_mixervoice_obj_get_panning);

static mp_obj_t audiomixer_mixervoice_obj_set_panning(mp_obj_t self_in, mp_obj_t panning_in) {
    audiomixer_mixervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiomixer_mixervoice_set_panning(self, panning_in);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiomixer_mixervoice_set_panning_obj, audiomixer_mixervoice_obj_set_panning);

MP_PROPERTY_GETSET(audiomixer_mixervoice_panning_obj,
    (mp_obj_t)&audiomixer_mixervoice_get_panning_obj,
    (mp_obj_t)&audiomixer_mixervoice_set_panning_obj);

static mp_obj_t audiomixer_mixervoice_obj_get_loop(mp_obj_t self_in) {
    return mp_obj_new_bool(common_hal_audiomixer_mixervoice_get_loop(self_in));
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiomixer_mixervoice_get_loop_obj, audiomixer_mixervoice_obj_get_loop);

static mp_obj_t audiomixer_mixervoice_obj_set_loop(mp_obj_t self_in, mp_obj_t loop_in) {
    audiomixer_mixervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    bool loop = mp_obj_is_true(loop_in);
    common_hal_audiomixer_mixervoice_set_loop(self, loop);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiomixer_mixervoice_set_loop_obj, audiomixer_mixervoice_obj_set_loop);

MP_PROPERTY_GETSET(audiomixer_mixervoice_loop_obj,
    (mp_obj_t)&audiomixer_mixervoice_get_loop_obj,
    (mp_obj_t)&audiomixer_mixervoice_set_loop_obj);

static mp_obj_t audiomixer_mixervoice_obj_get_playing(mp_obj_t self_in) {
    audiomixer_mixervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(common_hal_audiomixer_mixervoice_get_playing(self));
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiomixer_mixervoice_get_playing_obj, audiomixer_mixervoice_obj_get_playing);

MP_PROPERTY_GETTER(audiomixer_mixervoice_playing_obj,
    (mp_obj_t)&audiomixer_mixervoice_get_playing_obj);

static const mp_rom_map_elem_t audiomixer_mixervoice_locals_dict_table[] = {
    // Methods
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiomixer_mixervoice_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audiomixer_mixervoice_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_end), MP_ROM_PTR(&audiomixer_mixervoice_end_obj) },

    // Properties
    { MP_ROM_QSTR(MP_QSTR_playing), MP_ROM_PTR(&audiomixer_mixervoice_playing_obj) },
    { MP_ROM_QSTR(MP_QSTR_level), MP_ROM_PTR(&audiomixer_mixervoice_level_obj) },
    { MP_ROM_QSTR(MP_QSTR_panning), MP_ROM_PTR(&audiomixer_mixervoice_panning_obj) },
    { MP_ROM_QSTR(MP_QSTR_loop), MP_ROM_PTR(&audiomixer_mixervoice_loop_obj) },
};
static MP_DEFINE_CONST_DICT(audiomixer_mixervoice_locals_dict, audiomixer_mixervoice_locals_dict_table);

// Deviation from upstream: adds `attr, cp_compat_attr` -- see
// cp_compat/objproperty.c for why a ported type's MP_PROPERTY_GETTER/GETSET
// locals_dict entries need this to actually be invoked on this port.
MP_DEFINE_CONST_OBJ_TYPE(
    audiomixer_mixervoice_type,
    MP_QSTR_MixerVoice,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiomixer_mixervoice_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiomixer_mixervoice_locals_dict
    );
