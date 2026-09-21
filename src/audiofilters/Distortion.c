// Ported from CircuitPython's shared-bindings+shared-module/audiofilters/
// Distortion.{h,c} (upstream repo: https://github.com/adafruit/circuitpython,
// MIT). Deviations: m_malloc_without_collect -> m_malloc (no mainline
// equivalent, see docs/upstream-diff.md); `attr, cp_compat_attr` added to
// the type registration for its BlockInput/mode/soft_clip/playing
// properties.
//
// Reversed from an earlier "kept verbatim" decision (see
// docs/upstream-diff.md, "Distortion soft_clip: verbatim-kept union
// type-pun turned out to be architecture-dependent"): the make_new call
// site used to pass `args[ARG_soft_clip].u_obj` (an mp_arg_val_t union
// member) into a `bool soft_clip` parameter instead of `.u_bool` -- an
// upstream union type-pun that happened to produce the right truth value
// on x86-64 (this port's unix/windows targets, and CP's own unix coverage
// oracle), so it was kept verbatim and verified byte-for-byte there. Once
// this port gained a second architecture (wasm32, phase 8d), the same code
// produced a genuinely wrong `soft_clip` value -- and every real
// CircuitPython board is 32-bit ARM, not x86-64, so the byte-exact-on-unix
// result was the unrepresentative case, not the type-pun's "real" upstream
// behavior. Fixed to `.u_bool` here; still `memset(word_buffer, 32768,
// ...)` kept verbatim in the unsigned-16-bit silence path below (that one
// truncates to an `unsigned char` per the C standard on every architecture
// this project builds for, so it stays a genuine, portable, verbatim
// upstream quirk, not an ABI accident).
//
// Based on Godot's AudioEffectDistortion
// (https://docs.godotengine.org/en/stable/classes/class_audioeffectdistortion.html,
// https://github.com/godotengine/godot/blob/master/servers/audio/effects/audio_effect_distortion.cpp)
//
// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Cooper Dalrymple
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include "audiofilters/Distortion.h"

#include <math.h>
#include <string.h>

#include "cp_compat/argcheck.h"
#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/enum.h"
#include "cp_compat/objproperty.h"
#include "shared/audiodsp_distortion.h"

#include "py/runtime.h"
#include "shared/audiodsp_pump_lock.h"

// --- shared-module (DSP engine) -------------------------------------------

void common_hal_audiofilters_distortion_construct(audiofilters_distortion_obj_t *self,
    mp_obj_t drive, mp_obj_t pre_gain, mp_obj_t post_gain,
    audiofilters_distortion_mode mode, bool soft_clip, mp_obj_t mix,
    uint32_t buffer_size, uint8_t bits_per_sample, bool samples_signed,
    uint8_t channel_count, uint32_t sample_rate) {

    self->base.bits_per_sample = bits_per_sample;
    self->base.samples_signed = samples_signed;
    self->base.channel_count = channel_count;
    self->base.sample_rate = sample_rate;
    self->base.single_buffer = false;
    self->base.max_buffer_length = buffer_size;

    self->buffer_len = buffer_size; // in bytes

    self->buffer[0] = m_malloc(self->buffer_len);
    memset(self->buffer[0], 0, self->buffer_len);

    self->buffer[1] = m_malloc(self->buffer_len);
    memset(self->buffer[1], 0, self->buffer_len);

    self->last_buf_idx = 1;

    self->sample = NULL;
    self->sample_remaining_buffer = NULL;
    self->sample_buffer_length = 0;
    self->loop = false;
    self->more_data = false;

    synthio_block_assign_slot(drive, &self->drive, MP_QSTR_drive);
    synthio_block_assign_slot(pre_gain, &self->pre_gain, MP_QSTR_pre_gain);
    synthio_block_assign_slot(post_gain, &self->post_gain, MP_QSTR_post_gain);
    synthio_block_assign_slot(mix, &self->mix, MP_QSTR_mix);

    self->mode = mode;
    self->soft_clip = soft_clip;
}

void common_hal_audiofilters_distortion_deinit(audiofilters_distortion_obj_t *self) {
    // The whole body. mark_deinit is not the damage; the pointer
    // nulling AFTER it is -- the funnel's guard has already let a
    // pull in by then, and the pull writes into a buffer that has
    // just become NULL. Detach under the lock, free afterwards.
    audiodsp_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    self->buffer[0] = NULL;
    self->buffer[1] = NULL;
    audiodsp_pump_lock_release();
}

mp_obj_t common_hal_audiofilters_distortion_get_drive(audiofilters_distortion_obj_t *self) {
    return self->drive.obj;
}

void common_hal_audiofilters_distortion_set_drive(audiofilters_distortion_obj_t *self, mp_obj_t arg) {
    synthio_block_assign_slot(arg, &self->drive, MP_QSTR_drive);
}

mp_obj_t common_hal_audiofilters_distortion_get_pre_gain(audiofilters_distortion_obj_t *self) {
    return self->pre_gain.obj;
}

void common_hal_audiofilters_distortion_set_pre_gain(audiofilters_distortion_obj_t *self, mp_obj_t arg) {
    synthio_block_assign_slot(arg, &self->pre_gain, MP_QSTR_pre_gain);
}

mp_obj_t common_hal_audiofilters_distortion_get_post_gain(audiofilters_distortion_obj_t *self) {
    return self->post_gain.obj;
}

void common_hal_audiofilters_distortion_set_post_gain(audiofilters_distortion_obj_t *self, mp_obj_t arg) {
    synthio_block_assign_slot(arg, &self->post_gain, MP_QSTR_post_gain);
}

audiofilters_distortion_mode common_hal_audiofilters_distortion_get_mode(audiofilters_distortion_obj_t *self) {
    return self->mode;
}

void common_hal_audiofilters_distortion_set_mode(audiofilters_distortion_obj_t *self, audiofilters_distortion_mode arg) {
    self->mode = arg;
}

bool common_hal_audiofilters_distortion_get_soft_clip(audiofilters_distortion_obj_t *self) {
    return self->soft_clip;
}

void common_hal_audiofilters_distortion_set_soft_clip(audiofilters_distortion_obj_t *self, bool soft_clip) {
    self->soft_clip = soft_clip;
}

mp_obj_t common_hal_audiofilters_distortion_get_mix(audiofilters_distortion_obj_t *self) {
    return self->mix.obj;
}

void common_hal_audiofilters_distortion_set_mix(audiofilters_distortion_obj_t *self, mp_obj_t arg) {
    synthio_block_assign_slot(arg, &self->mix, MP_QSTR_mix);
}

void audiofilters_distortion_reset_buffer(audiofilters_distortion_obj_t *self,
    bool single_channel_output,
    uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    memset(self->buffer[0], 0, self->buffer_len);
    memset(self->buffer[1], 0, self->buffer_len);
}

bool common_hal_audiofilters_distortion_get_playing(audiofilters_distortion_obj_t *self) {
    return self->sample != NULL;
}

void common_hal_audiofilters_distortion_play(audiofilters_distortion_obj_t *self, mp_obj_t sample, bool loop) {
    audiosample_must_match(&self->base, sample, false);

    // Prime the new source into locals FIRST, outside the lock. This pull can
    // read a file through the VFS, and holding the pump's lock across a disk
    // is the one thing the contract forbids -- the audio would stand still
    // for it. Then one locked store publishes all five words together, which
    // is what get_buffer reads, so a pull sees the whole new source or the
    // whole old one and never three words of each.
    uint8_t *primed = NULL;
    uint32_t primed_length = 0;
    audiosample_reset_buffer(sample, false, 0);
    audioio_get_buffer_result_t result = audiosample_get_buffer(sample, false,
        0, &primed, &primed_length);
    primed_length /= (self->base.bits_per_sample / 8);

    audiodsp_pump_lock_acquire();
    self->sample = sample;
    self->loop = loop;
    self->sample_remaining_buffer = (void *)primed;
    self->sample_buffer_length = primed_length;
    self->more_data = result == GET_BUFFER_MORE_DATA;
    audiodsp_pump_lock_release();
}

void common_hal_audiofilters_distortion_stop(audiofilters_distortion_obj_t *self) {
    self->sample = NULL;
}

static mp_float_t db_to_linear(mp_float_t value) {
    return MICROPY_FLOAT_C_FUN(exp)(value * MICROPY_FLOAT_CONST(0.11512925464970228420089957273422));
}

audioio_get_buffer_result_t audiofilters_distortion_get_buffer(audiofilters_distortion_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;

    self->last_buf_idx = !self->last_buf_idx;

    int16_t *word_buffer = (int16_t *)self->buffer[self->last_buf_idx];
    int8_t *hword_buffer = self->buffer[self->last_buf_idx];
    uint32_t length = self->buffer_len / (self->base.bits_per_sample / 8);

    while (length != 0) {
        if (self->sample_buffer_length == 0) {
            if (!self->more_data) {
                if (self->loop && self->sample) {
                    audiosample_reset_buffer(self->sample, false, 0);
                } else {
                    self->sample = NULL;
                }
            }
            if (self->sample) {
                audioio_get_buffer_result_t result = audiosample_get_buffer(self->sample, false, 0, (uint8_t **)&self->sample_remaining_buffer, &self->sample_buffer_length);
                self->sample_buffer_length /= (self->base.bits_per_sample / 8);
                self->more_data = result == GET_BUFFER_MORE_DATA;
            }
        }

        if (self->sample == NULL) {
            if (self->base.samples_signed) {
                memset(word_buffer, 0, length * (self->base.bits_per_sample / 8));
            } else {
                // For unsigned samples set to the middle which is "quiet"
                if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                    memset(word_buffer, 32768, length * (self->base.bits_per_sample / 8));
                } else {
                    memset(hword_buffer, 128, length * (self->base.bits_per_sample / 8));
                }
            }

            shared_bindings_synthio_lfo_tick(self->base.sample_rate, length / self->base.channel_count);
            (void)synthio_block_slot_get(&self->drive);
            (void)synthio_block_slot_get(&self->pre_gain);
            (void)synthio_block_slot_get(&self->post_gain);
            (void)synthio_block_slot_get(&self->mix);

            length = 0;
        } else {
            uint32_t n = MIN(MIN(self->sample_buffer_length, length), (uint32_t)(SYNTHIO_MAX_DUR * self->base.channel_count));

            int16_t *sample_src = (int16_t *)self->sample_remaining_buffer;
            int8_t *sample_hsrc = (int8_t *)self->sample_remaining_buffer;

            shared_bindings_synthio_lfo_tick(self->base.sample_rate, n / self->base.channel_count);
            mp_float_t drive = synthio_block_slot_get_limited(&self->drive, MICROPY_FLOAT_CONST(0.0), MICROPY_FLOAT_CONST(1.0));
            mp_float_t pre_gain = db_to_linear(synthio_block_slot_get_limited(&self->pre_gain, MICROPY_FLOAT_CONST(-60.0), MICROPY_FLOAT_CONST(60.0)));
            mp_float_t post_gain = db_to_linear(synthio_block_slot_get_limited(&self->post_gain, MICROPY_FLOAT_CONST(-80.0), MICROPY_FLOAT_CONST(24.0)));
            mp_float_t mix = synthio_block_slot_get_limited(&self->mix, MICROPY_FLOAT_CONST(0.0), MICROPY_FLOAT_CONST(1.0));

            uint32_t word_mask = 0;
            if (self->mode == DISTORTION_MODE_CLIP) {
                drive = MICROPY_FLOAT_CONST(1.0001) - drive;
            } else if (self->mode == DISTORTION_MODE_WAVESHAPE) {
                drive = MICROPY_FLOAT_CONST(2.0) * drive / (MICROPY_FLOAT_CONST(1.0001) - drive);
            } else if (self->mode == DISTORTION_MODE_LOFI) {
                word_mask = 0xFFFFFFFF ^ ((1 << (uint32_t)MICROPY_FLOAT_C_FUN(round)(drive * MICROPY_FLOAT_CONST(14.0))) - 1);
            }

            if (mix <= MICROPY_FLOAT_CONST(0.01)) {
                for (uint32_t i = 0; i < n; i++) {
                    if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                        word_buffer[i] = sample_src[i];
                    } else {
                        hword_buffer[i] = sample_hsrc[i];
                    }
                }
            } else {
                for (uint32_t i = 0; i < n; i++) {
                    int32_t sample_word = 0;
                    if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                        sample_word = sample_src[i];
                    } else {
                        if (self->base.samples_signed) {
                            sample_word = sample_hsrc[i];
                        } else {
                            // Careful: 8-bit unsigned -> 32-bit signed
                            sample_word = (int8_t)(((uint8_t)sample_hsrc[i]) ^ 0x80);
                        }
                    }

                    int32_t word = audiodsp_distortion_sample(sample_word,
                        drive, pre_gain, post_gain,
                        (audiodsp_distortion_mode_t)self->mode,
                        self->soft_clip, mix, word_mask);

                    if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                        word_buffer[i] = (int16_t)word;
                        if (!self->base.samples_signed) {
                            word_buffer[i] ^= 0x8000;
                        }
                    } else {
                        int8_t mixed = (int8_t)word;
                        if (self->base.samples_signed) {
                            hword_buffer[i] = mixed;
                        } else {
                            hword_buffer[i] = (uint8_t)mixed ^ 0x80;
                        }
                    }
                }
            }

            length -= n;
            word_buffer += n;
            hword_buffer += n;
            self->sample_remaining_buffer += (n * (self->base.bits_per_sample / 8));
            self->sample_buffer_length -= n;
        }
    }

    *buffer = (uint8_t *)self->buffer[self->last_buf_idx];
    *buffer_length = self->buffer_len;

    return GET_BUFFER_MORE_DATA;
}

// --- Python bindings (from shared-bindings/audiofilters/Distortion.c) ----

MAKE_ENUM_VALUE(audiofilters_distortion_mode_type, distortion_mode, CLIP, DISTORTION_MODE_CLIP);
MAKE_ENUM_VALUE(audiofilters_distortion_mode_type, distortion_mode, LOFI, DISTORTION_MODE_LOFI);
MAKE_ENUM_VALUE(audiofilters_distortion_mode_type, distortion_mode, OVERDRIVE, DISTORTION_MODE_OVERDRIVE);
MAKE_ENUM_VALUE(audiofilters_distortion_mode_type, distortion_mode, WAVESHAPE, DISTORTION_MODE_WAVESHAPE);

MAKE_ENUM_MAP(audiofilters_distortion_mode) {
    MAKE_ENUM_MAP_ENTRY(distortion_mode, CLIP),
    MAKE_ENUM_MAP_ENTRY(distortion_mode, LOFI),
    MAKE_ENUM_MAP_ENTRY(distortion_mode, OVERDRIVE),
    MAKE_ENUM_MAP_ENTRY(distortion_mode, WAVESHAPE),
};

static MP_DEFINE_CONST_DICT(audiofilters_distortion_mode_locals_dict, audiofilters_distortion_mode_locals_table);

MAKE_PRINTER(audiofilters, audiofilters_distortion_mode);

MAKE_ENUM_TYPE(audiofilters, DistortionMode, audiofilters_distortion_mode);

static audiofilters_distortion_mode validate_distortion_mode(mp_obj_t obj, qstr arg_name) {
    return cp_enum_value(&audiofilters_distortion_mode_type, obj, arg_name);
}

static mp_obj_t audiofilters_distortion_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_drive, ARG_pre_gain, ARG_post_gain, ARG_mode, ARG_soft_clip, ARG_mix, ARG_buffer_size, ARG_sample_rate, ARG_bits_per_sample, ARG_samples_signed, ARG_channel_count, };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_drive, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(0)} },
        { MP_QSTR_pre_gain, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(0)} },
        { MP_QSTR_post_gain, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(0)} },
        { MP_QSTR_mode, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_soft_clip, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
        { MP_QSTR_mix, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(1)} },
        { MP_QSTR_buffer_size, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 512} },
        { MP_QSTR_sample_rate, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 8000} },
        { MP_QSTR_bits_per_sample, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 16} },
        { MP_QSTR_samples_signed, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = true} },
        { MP_QSTR_channel_count, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1 } },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t channel_count = mp_arg_validate_int_range(args[ARG_channel_count].u_int, 1, 2, MP_QSTR_channel_count);
    mp_arg_validate_int_min(args[ARG_sample_rate].u_int, 1, MP_QSTR_sample_rate);
    mp_int_t bits_per_sample = args[ARG_bits_per_sample].u_int;
    if (bits_per_sample != 8 && bits_per_sample != 16) {
        mp_raise_ValueError(MP_ERROR_TEXT("bits_per_sample must be 8 or 16"));
    }

    audiofilters_distortion_mode mode = DISTORTION_MODE_CLIP;
    if (args[ARG_mode].u_obj != MP_OBJ_NULL) {
        mode = validate_distortion_mode(args[ARG_mode].u_obj, MP_QSTR_mode);
    }

    audiofilters_distortion_obj_t *self =
        mp_obj_malloc(audiofilters_distortion_obj_t, &audiofilters_distortion_type);
    common_hal_audiofilters_distortion_construct(self,
        args[ARG_drive].u_obj,
        args[ARG_pre_gain].u_obj,
        args[ARG_post_gain].u_obj,
        mode,
        args[ARG_soft_clip].u_bool,
        args[ARG_mix].u_obj,
        args[ARG_buffer_size].u_int,
        bits_per_sample,
        args[ARG_samples_signed].u_bool,
        channel_count,
        args[ARG_sample_rate].u_int);

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audiofilters_distortion_deinit(mp_obj_t self_in) {
    audiofilters_distortion_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiofilters_distortion_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofilters_distortion_deinit_obj, audiofilters_distortion_deinit);

static void check_for_deinit(audiofilters_distortion_obj_t *self) {
    // One word read under the lock, and the RAISE OUTSIDE IT. The lock's
    // contract is that nothing which can longjmp runs while it is held
    // (shared/audiodsp_pump_lock.h): a raise from in here never reaches the
    // release, so the mutex is left owned by a thread that has gone back to
    // the interpreter, and the pump blocks on it for ever. This is the guard
    // on every Python-facing method of this class, so it is the most reached
    // statement in the file.
    audiodsp_pump_lock_acquire();
    const bool released = audiosample_deinited(&self->base);
    audiodsp_pump_lock_release();
    if (released) {
        audiosample_check_for_deinit(&self->base);
    }
}

static mp_obj_t audiofilters_distortion_obj_get_drive(mp_obj_t self_in) {
    return common_hal_audiofilters_distortion_get_drive(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiofilters_distortion_get_drive_obj, audiofilters_distortion_obj_get_drive);

static mp_obj_t audiofilters_distortion_obj_set_drive(mp_obj_t self_in, mp_obj_t drive_in) {
    audiofilters_distortion_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiofilters_distortion_set_drive(self, drive_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiofilters_distortion_set_drive_obj, audiofilters_distortion_obj_set_drive);

MP_PROPERTY_GETSET(audiofilters_distortion_drive_obj,
    (mp_obj_t)&audiofilters_distortion_get_drive_obj,
    (mp_obj_t)&audiofilters_distortion_set_drive_obj);

static mp_obj_t audiofilters_distortion_obj_get_pre_gain(mp_obj_t self_in) {
    return common_hal_audiofilters_distortion_get_pre_gain(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiofilters_distortion_get_pre_gain_obj, audiofilters_distortion_obj_get_pre_gain);

static mp_obj_t audiofilters_distortion_obj_set_pre_gain(mp_obj_t self_in, mp_obj_t pre_gain_in) {
    audiofilters_distortion_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiofilters_distortion_set_pre_gain(self, pre_gain_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiofilters_distortion_set_pre_gain_obj, audiofilters_distortion_obj_set_pre_gain);

MP_PROPERTY_GETSET(audiofilters_distortion_pre_gain_obj,
    (mp_obj_t)&audiofilters_distortion_get_pre_gain_obj,
    (mp_obj_t)&audiofilters_distortion_set_pre_gain_obj);

static mp_obj_t audiofilters_distortion_obj_get_post_gain(mp_obj_t self_in) {
    return common_hal_audiofilters_distortion_get_post_gain(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiofilters_distortion_get_post_gain_obj, audiofilters_distortion_obj_get_post_gain);

static mp_obj_t audiofilters_distortion_obj_set_post_gain(mp_obj_t self_in, mp_obj_t post_gain_in) {
    audiofilters_distortion_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiofilters_distortion_set_post_gain(self, post_gain_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiofilters_distortion_set_post_gain_obj, audiofilters_distortion_obj_set_post_gain);

MP_PROPERTY_GETSET(audiofilters_distortion_post_gain_obj,
    (mp_obj_t)&audiofilters_distortion_get_post_gain_obj,
    (mp_obj_t)&audiofilters_distortion_set_post_gain_obj);

static mp_obj_t audiofilters_distortion_obj_get_mode(mp_obj_t self_in) {
    audiofilters_distortion_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return cp_enum_find(&audiofilters_distortion_mode_type, common_hal_audiofilters_distortion_get_mode(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiofilters_distortion_get_mode_obj, audiofilters_distortion_obj_get_mode);

static mp_obj_t audiofilters_distortion_obj_set_mode(mp_obj_t self_in, mp_obj_t mode_in) {
    audiofilters_distortion_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiofilters_distortion_mode mode = validate_distortion_mode(mode_in, MP_QSTR_mode);
    common_hal_audiofilters_distortion_set_mode(self, mode);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiofilters_distortion_set_mode_obj, audiofilters_distortion_obj_set_mode);

MP_PROPERTY_GETSET(audiofilters_distortion_mode_obj,
    (mp_obj_t)&audiofilters_distortion_get_mode_obj,
    (mp_obj_t)&audiofilters_distortion_set_mode_obj);

static mp_obj_t audiofilters_distortion_obj_get_soft_clip(mp_obj_t self_in) {
    return mp_obj_new_bool(common_hal_audiofilters_distortion_get_soft_clip(self_in));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiofilters_distortion_get_soft_clip_obj, audiofilters_distortion_obj_get_soft_clip);

static mp_obj_t audiofilters_distortion_obj_set_soft_clip(mp_obj_t self_in, mp_obj_t soft_clip_in) {
    audiofilters_distortion_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiofilters_distortion_set_soft_clip(self, mp_obj_is_true(soft_clip_in));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiofilters_distortion_set_soft_clip_obj, audiofilters_distortion_obj_set_soft_clip);

MP_PROPERTY_GETSET(audiofilters_distortion_soft_clip_obj,
    (mp_obj_t)&audiofilters_distortion_get_soft_clip_obj,
    (mp_obj_t)&audiofilters_distortion_set_soft_clip_obj);

static mp_obj_t audiofilters_distortion_obj_get_mix(mp_obj_t self_in) {
    return common_hal_audiofilters_distortion_get_mix(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiofilters_distortion_get_mix_obj, audiofilters_distortion_obj_get_mix);

static mp_obj_t audiofilters_distortion_obj_set_mix(mp_obj_t self_in, mp_obj_t mix_in) {
    audiofilters_distortion_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiofilters_distortion_set_mix(self, mix_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiofilters_distortion_set_mix_obj, audiofilters_distortion_obj_set_mix);

MP_PROPERTY_GETSET(audiofilters_distortion_mix_obj,
    (mp_obj_t)&audiofilters_distortion_get_mix_obj,
    (mp_obj_t)&audiofilters_distortion_set_mix_obj);

static mp_obj_t audiofilters_distortion_obj_get_playing(mp_obj_t self_in) {
    audiofilters_distortion_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_bool(common_hal_audiofilters_distortion_get_playing(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiofilters_distortion_get_playing_obj, audiofilters_distortion_obj_get_playing);

MP_PROPERTY_GETTER(audiofilters_distortion_playing_obj,
    (mp_obj_t)&audiofilters_distortion_get_playing_obj);

static mp_obj_t audiofilters_distortion_obj_play(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_sample, ARG_loop };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sample,    MP_ARG_OBJ | MP_ARG_REQUIRED, {} },
        { MP_QSTR_loop,      MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
    };
    audiofilters_distortion_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    check_for_deinit(self);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_obj_t sample = args[ARG_sample].u_obj;
    common_hal_audiofilters_distortion_play(self, sample, args[ARG_loop].u_bool);

    return MP_OBJ_FROM_PTR(self);
}
MP_DEFINE_CONST_FUN_OBJ_KW(audiofilters_distortion_play_obj, 1, audiofilters_distortion_obj_play);

static mp_obj_t audiofilters_distortion_obj_stop(mp_obj_t self_in) {
    audiofilters_distortion_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiofilters_distortion_stop(self);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiofilters_distortion_stop_obj, audiofilters_distortion_obj_stop);

static const mp_rom_map_elem_t audiofilters_distortion_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiofilters_distortion_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiofilters_distortion_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audiofilters_distortion_stop_obj) },

    { MP_ROM_QSTR(MP_QSTR_playing), MP_ROM_PTR(&audiofilters_distortion_playing_obj) },
    { MP_ROM_QSTR(MP_QSTR_drive), MP_ROM_PTR(&audiofilters_distortion_drive_obj) },
    { MP_ROM_QSTR(MP_QSTR_pre_gain), MP_ROM_PTR(&audiofilters_distortion_pre_gain_obj) },
    { MP_ROM_QSTR(MP_QSTR_post_gain), MP_ROM_PTR(&audiofilters_distortion_post_gain_obj) },
    { MP_ROM_QSTR(MP_QSTR_mode), MP_ROM_PTR(&audiofilters_distortion_mode_obj) },
    { MP_ROM_QSTR(MP_QSTR_soft_clip), MP_ROM_PTR(&audiofilters_distortion_soft_clip_obj) },
    { MP_ROM_QSTR(MP_QSTR_mix), MP_ROM_PTR(&audiofilters_distortion_mix_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiofilters_distortion_locals_dict, audiofilters_distortion_locals_dict_table);

static const audiosample_p_t audiofilters_distortion_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audiofilters_distortion_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audiofilters_distortion_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiofilters_distortion_type,
    MP_QSTR_Distortion,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiofilters_distortion_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiofilters_distortion_locals_dict,
    protocol, &audiofilters_distortion_proto
    );
