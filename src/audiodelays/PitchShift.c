// Ported from CircuitPython's shared-bindings+shared-module/audiodelays/
// PitchShift.{h,c} (upstream repo: https://github.com/adafruit/circuitpython,
// MIT). Deviation: m_malloc_without_collect -> m_malloc (no mainline
// equivalent, see docs/upstream-diff.md). `attr, cp_compat_attr` added for
// semitones/mix/playing. The custom `__exit__` is kept verbatim, same as
// Chorus (see that file's comment). Kept verbatim (not fixed): the
// unsigned-16-bit silence path's `memset(word_buffer, 32768, ...)` has the
// same fill-value-truncates-to-a-byte quirk as Distortion's (see that
// file's comment) -- reproduced exactly, not corrected. Also kept verbatim:
// `buf_offset` in the per-sample loop ignores `single_channel_output`
// (`channel == 1 || i % channel_count == 1`, unlike every sibling effect's
// `(single_channel_output && channel == 1) || (!single_channel_output && ...)`
// pattern) -- upstream's own inconsistency, not a port bug.
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Cooper Dalrymple
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include "audiodelays/PitchShift.h"

#include <math.h>
#include <string.h>

#include "cp_compat/argcheck.h"
#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/objproperty.h"
#include "shared/audiodsp_pitchshift.h"

#include "py/runtime.h"
#include "shared/audiodsp_pump_lock.h"

// --- shared-module (DSP engine) -------------------------------------------

void common_hal_audiodelays_pitch_shift_construct(audiodelays_pitch_shift_obj_t *self,
    mp_obj_t semitones, mp_obj_t mix, uint32_t window, uint32_t overlap,
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

    synthio_block_assign_slot(semitones, &self->semitones, MP_QSTR_semitones);
    synthio_block_assign_slot(mix, &self->mix, MP_QSTR_mix);

    self->window_len = window; // bytes
    self->window_buffer = m_malloc(self->window_len);
    memset(self->window_buffer, 0, self->window_len);

    self->overlap_len = overlap; // bytes
    if (self->overlap_len) {
        self->overlap_buffer = m_malloc(self->overlap_len);
        memset(self->overlap_buffer, 0, self->overlap_len);
    } else {
        self->overlap_buffer = NULL;
    }

    // The current position that the end of the overlap buffer will be written to the window buffer
    self->window_index = 0;
    // The position that the current sample will be written to the overlap buffer
    self->overlap_index = 0;
    // The position that the window buffer will be read from and written to the output
    self->read_index = 0;

    mp_float_t f_semitones = synthio_block_slot_get(&self->semitones);
    recalculate_rate(self, f_semitones);
}

void common_hal_audiodelays_pitch_shift_deinit(audiodelays_pitch_shift_obj_t *self) {
    // The whole body. mark_deinit is not the damage; the pointer
    // nulling AFTER it is -- the funnel's guard has already let a
    // pull in by then, and the pull writes into a buffer that has
    // just become NULL. Detach under the lock, free afterwards.
    audiodsp_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    self->window_buffer = NULL;
    self->overlap_buffer = NULL;
    self->buffer[0] = NULL;
    self->buffer[1] = NULL;
    audiodsp_pump_lock_release();
}

mp_obj_t common_hal_audiodelays_pitch_shift_get_semitones(audiodelays_pitch_shift_obj_t *self) {
    return self->semitones.obj;
}

void common_hal_audiodelays_pitch_shift_set_semitones(audiodelays_pitch_shift_obj_t *self, mp_obj_t delay_ms) {
    synthio_block_assign_slot(delay_ms, &self->semitones, MP_QSTR_semitones);
    mp_float_t semitones = synthio_block_slot_get(&self->semitones);
    recalculate_rate(self, semitones);
}

void recalculate_rate(audiodelays_pitch_shift_obj_t *self, mp_float_t semitones) {
    self->read_rate = (uint32_t)(MICROPY_FLOAT_C_FUN(pow)(2.0, semitones / MICROPY_FLOAT_CONST(12.0)) * (1 << PITCH_READ_SHIFT));
    self->current_semitones = semitones;
}

mp_obj_t common_hal_audiodelays_pitch_shift_get_mix(audiodelays_pitch_shift_obj_t *self) {
    return self->mix.obj;
}

void common_hal_audiodelays_pitch_shift_set_mix(audiodelays_pitch_shift_obj_t *self, mp_obj_t arg) {
    synthio_block_assign_slot(arg, &self->mix, MP_QSTR_mix);
}

void audiodelays_pitch_shift_reset_buffer(audiodelays_pitch_shift_obj_t *self,
    bool single_channel_output,
    uint8_t channel) {
    (void)single_channel_output;
    (void)channel;

    memset(self->buffer[0], 0, self->buffer_len);
    memset(self->buffer[1], 0, self->buffer_len);
    memset(self->window_buffer, 0, self->window_len);
    if (self->overlap_len) {
        memset(self->overlap_buffer, 0, self->overlap_len);
    }
}

bool common_hal_audiodelays_pitch_shift_get_playing(audiodelays_pitch_shift_obj_t *self) {
    return self->sample != NULL;
}

void common_hal_audiodelays_pitch_shift_play(audiodelays_pitch_shift_obj_t *self, mp_obj_t sample, bool loop) {
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

void common_hal_audiodelays_pitch_shift_stop(audiodelays_pitch_shift_obj_t *self) {
    self->sample = NULL;
}

audioio_get_buffer_result_t audiodelays_pitch_shift_get_buffer(audiodelays_pitch_shift_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {

    if (!single_channel_output) {
        channel = 0;
    }

    self->last_buf_idx = !self->last_buf_idx;

    int16_t *word_buffer = (int16_t *)self->buffer[self->last_buf_idx];
    int8_t *hword_buffer = self->buffer[self->last_buf_idx];
    uint32_t length = self->buffer_len / (self->base.bits_per_sample / 8);

    int16_t *window_buffer = (int16_t *)self->window_buffer;
    uint32_t window_size = self->window_len / sizeof(uint16_t) / self->base.channel_count;

    int16_t *overlap_buffer = NULL;
    uint32_t overlap_size = 0;
    if (self->overlap_len) {
        overlap_buffer = (int16_t *)self->overlap_buffer;
        overlap_size = self->overlap_len / sizeof(uint16_t) / self->base.channel_count;
    }

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
            (void)synthio_block_slot_get(&self->semitones);
            (void)synthio_block_slot_get(&self->mix);

            length = 0;
        } else {
            uint32_t n = MIN(MIN(self->sample_buffer_length, length), (uint32_t)(SYNTHIO_MAX_DUR * self->base.channel_count));

            int16_t *sample_src = (int16_t *)self->sample_remaining_buffer;
            int8_t *sample_hsrc = (int8_t *)self->sample_remaining_buffer;

            shared_bindings_synthio_lfo_tick(self->base.sample_rate, n / self->base.channel_count);
            mp_float_t semitones = synthio_block_slot_get(&self->semitones);
            mp_float_t mix = synthio_block_slot_get_limited(&self->mix, MICROPY_FLOAT_CONST(0.0), MICROPY_FLOAT_CONST(1.0)) * MICROPY_FLOAT_CONST(2.0);

            // Only recalculate rate if semitones has changed
            if (memcmp(&semitones, &self->current_semitones, sizeof(mp_float_t))) {
                recalculate_rate(self, semitones);
            }

            if (self->base.bits_per_sample == 16 && self->base.samples_signed &&
                !single_channel_output) {
                audiodsp_pitchshift_positions_t positions = {
                    self->window_index, self->overlap_index, self->read_index};
                audiodsp_pitchshift_process_s16(word_buffer, sample_src, n,
                    window_buffer, window_size, overlap_buffer, overlap_size,
                    self->base.channel_count, self->read_rate, mix, &positions);
                self->window_index = positions.window_index;
                self->overlap_index = positions.overlap_index;
                self->read_index = positions.read_index;
                goto pitchshift_samples_done;
            }

            for (uint32_t i = 0; i < n; i++) {
                bool buf_offset = (channel == 1 || i % self->base.channel_count == 1);

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

                if (overlap_size) {
                    // Copy last sample from overlap and store in buffer
                    window_buffer[self->window_index + window_size * buf_offset] = overlap_buffer[self->overlap_index + overlap_size * buf_offset];
                    // Save current sample in overlap
                    overlap_buffer[self->overlap_index + overlap_size * buf_offset] = (int16_t)sample_word;
                } else {
                    window_buffer[self->window_index + window_size * buf_offset] = (int16_t)sample_word;
                }

                uint32_t read_index = self->read_index >> PITCH_READ_SHIFT;
                uint32_t read_overlap_offset = read_index + window_size * (read_index < self->window_index) - self->window_index;

                int32_t word = (int32_t)window_buffer[read_index + window_size * buf_offset];

                if (overlap_size && read_overlap_offset > 0 && read_overlap_offset <= overlap_size) {
                    word *= (int32_t)read_overlap_offset;
                    word += (int32_t)overlap_buffer[((self->overlap_index + read_overlap_offset) % overlap_size) + overlap_size * buf_offset] * (int32_t)(overlap_size - read_overlap_offset);
                    word /= (int32_t)overlap_size;
                }

                word = (int32_t)((sample_word * MIN(MICROPY_FLOAT_CONST(2.0) - mix, MICROPY_FLOAT_CONST(1.0))) + (word * MIN(mix, MICROPY_FLOAT_CONST(1.0))));
                word = synthio_mix_down_sample(word, SYNTHIO_MIX_DOWN_SCALE(2));

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

                if (self->base.channel_count == 1 || buf_offset) {
                    self->window_index++;
                    if (self->window_index >= window_size) {
                        self->window_index = 0;
                    }

                    if (overlap_size) {
                        self->overlap_index++;
                        if (self->overlap_index >= overlap_size) {
                            self->overlap_index = 0;
                        }
                    }

                    self->read_index += self->read_rate;
                    if (self->read_index >= window_size << PITCH_READ_SHIFT) {
                        self->read_index -= window_size << PITCH_READ_SHIFT;
                    }
                }
            }

pitchshift_samples_done:
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

// --- Python bindings (from shared-bindings/audiodelays/PitchShift.c) -----

static mp_obj_t audiodelays_pitch_shift_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_semitones, ARG_mix, ARG_window, ARG_overlap, ARG_buffer_size, ARG_sample_rate, ARG_bits_per_sample, ARG_samples_signed, ARG_channel_count, };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_semitones, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(0)} },
        { MP_QSTR_mix, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(1)} },
        { MP_QSTR_window, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1024} },
        { MP_QSTR_overlap, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 128} },
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

    audiodelays_pitch_shift_obj_t *self =
        mp_obj_malloc(audiodelays_pitch_shift_obj_t, &audiodelays_pitch_shift_type);
    common_hal_audiodelays_pitch_shift_construct(self,
        args[ARG_semitones].u_obj,
        args[ARG_mix].u_obj,
        args[ARG_window].u_int,
        args[ARG_overlap].u_int,
        args[ARG_buffer_size].u_int,
        bits_per_sample,
        args[ARG_samples_signed].u_bool,
        channel_count,
        args[ARG_sample_rate].u_int);

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audiodelays_pitch_shift_deinit(mp_obj_t self_in) {
    audiodelays_pitch_shift_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_pitch_shift_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_pitch_shift_deinit_obj, audiodelays_pitch_shift_deinit);

static void check_for_deinit(audiodelays_pitch_shift_obj_t *self) {
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

static mp_obj_t audiodelays_pitch_shift_obj___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    common_hal_audiodelays_pitch_shift_deinit(args[0]);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiodelays_pitch_shift___exit___obj, 4, 4, audiodelays_pitch_shift_obj___exit__);

static mp_obj_t audiodelays_pitch_shift_obj_get_semitones(mp_obj_t self_in) {
    return common_hal_audiodelays_pitch_shift_get_semitones(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_pitch_shift_get_semitones_obj, audiodelays_pitch_shift_obj_get_semitones);

static mp_obj_t audiodelays_pitch_shift_obj_set_semitones(mp_obj_t self_in, mp_obj_t semitones_in) {
    audiodelays_pitch_shift_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_pitch_shift_set_semitones(self, semitones_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodelays_pitch_shift_set_semitones_obj, audiodelays_pitch_shift_obj_set_semitones);

MP_PROPERTY_GETSET(audiodelays_pitch_shift_semitones_obj,
    (mp_obj_t)&audiodelays_pitch_shift_get_semitones_obj,
    (mp_obj_t)&audiodelays_pitch_shift_set_semitones_obj);

static mp_obj_t audiodelays_pitch_shift_obj_get_mix(mp_obj_t self_in) {
    return common_hal_audiodelays_pitch_shift_get_mix(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_pitch_shift_get_mix_obj, audiodelays_pitch_shift_obj_get_mix);

static mp_obj_t audiodelays_pitch_shift_obj_set_mix(mp_obj_t self_in, mp_obj_t mix_in) {
    audiodelays_pitch_shift_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_pitch_shift_set_mix(self, mix_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodelays_pitch_shift_set_mix_obj, audiodelays_pitch_shift_obj_set_mix);

MP_PROPERTY_GETSET(audiodelays_pitch_shift_mix_obj,
    (mp_obj_t)&audiodelays_pitch_shift_get_mix_obj,
    (mp_obj_t)&audiodelays_pitch_shift_set_mix_obj);

static mp_obj_t audiodelays_pitch_shift_obj_get_playing(mp_obj_t self_in) {
    audiodelays_pitch_shift_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_bool(common_hal_audiodelays_pitch_shift_get_playing(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_pitch_shift_get_playing_obj, audiodelays_pitch_shift_obj_get_playing);

MP_PROPERTY_GETTER(audiodelays_pitch_shift_playing_obj,
    (mp_obj_t)&audiodelays_pitch_shift_get_playing_obj);

static mp_obj_t audiodelays_pitch_shift_obj_play(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_sample, ARG_loop };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sample,    MP_ARG_OBJ | MP_ARG_REQUIRED, {} },
        { MP_QSTR_loop,      MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
    };
    audiodelays_pitch_shift_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    check_for_deinit(self);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_obj_t sample = args[ARG_sample].u_obj;
    common_hal_audiodelays_pitch_shift_play(self, sample, args[ARG_loop].u_bool);

    return MP_OBJ_FROM_PTR(self);
}
MP_DEFINE_CONST_FUN_OBJ_KW(audiodelays_pitch_shift_play_obj, 1, audiodelays_pitch_shift_obj_play);

static mp_obj_t audiodelays_pitch_shift_obj_stop(mp_obj_t self_in) {
    audiodelays_pitch_shift_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_pitch_shift_stop(self);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_pitch_shift_stop_obj, audiodelays_pitch_shift_obj_stop);

static const mp_rom_map_elem_t audiodelays_pitch_shift_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiodelays_pitch_shift_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&audiodelays_pitch_shift___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiodelays_pitch_shift_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audiodelays_pitch_shift_stop_obj) },

    { MP_ROM_QSTR(MP_QSTR_playing), MP_ROM_PTR(&audiodelays_pitch_shift_playing_obj) },
    { MP_ROM_QSTR(MP_QSTR_semitones), MP_ROM_PTR(&audiodelays_pitch_shift_semitones_obj) },
    { MP_ROM_QSTR(MP_QSTR_mix), MP_ROM_PTR(&audiodelays_pitch_shift_mix_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiodelays_pitch_shift_locals_dict, audiodelays_pitch_shift_locals_dict_table);

static const audiosample_p_t audiodelays_pitch_shift_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audiodelays_pitch_shift_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audiodelays_pitch_shift_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiodelays_pitch_shift_type,
    MP_QSTR_PitchShift,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiodelays_pitch_shift_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiodelays_pitch_shift_locals_dict,
    protocol, &audiodelays_pitch_shift_proto
    );
