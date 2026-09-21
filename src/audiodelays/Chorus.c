// Ported from CircuitPython's shared-bindings+shared-module/audiodelays/
// Chorus.{h,c} (upstream repo: https://github.com/adafruit/circuitpython,
// MIT). Deviation: m_malloc_without_collect -> m_malloc (no mainline
// equivalent, see docs/upstream-diff.md). `attr, cp_compat_attr` added for
// delay_ms/voices/mix/playing. The custom `__exit__` (calling
// common_hal_audiodelays_chorus_deinit directly instead of the generic
// default___exit___obj method-dispatch helper) is kept verbatim -- it's
// upstream's own micro-optimization for this one type, not a port
// artifact.
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Mark Komus
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include "audiodelays/Chorus.h"

#include <math.h>
#include <string.h>

#include "cp_compat/argcheck.h"
#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/objproperty.h"

#include "py/runtime.h"
#include "shared/audiodsp_chorus.h"
#include "shared/audiodsp_pump_lock.h"

// --- shared-module (DSP engine) -------------------------------------------

void common_hal_audiodelays_chorus_construct(audiodelays_chorus_obj_t *self, uint32_t max_delay_ms,
    mp_obj_t delay_ms, mp_obj_t voices, mp_obj_t mix,
    uint32_t buffer_size, uint8_t bits_per_sample,
    bool samples_signed, uint8_t channel_count, uint32_t sample_rate) {

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

    if (voices == MP_OBJ_NULL) {
        voices = mp_obj_new_float(MICROPY_FLOAT_CONST(1.0));
    }
    synthio_block_assign_slot(voices, &self->voices, MP_QSTR_voices);

    if (delay_ms == MP_OBJ_NULL) {
        delay_ms = mp_obj_new_float(MICROPY_FLOAT_CONST(50.0));
    }
    synthio_block_assign_slot(delay_ms, &self->delay_ms, MP_QSTR_delay_ms);

    if (mix == MP_OBJ_NULL) {
        mix = mp_obj_new_float(MICROPY_FLOAT_CONST(0.5));
    }
    synthio_block_assign_slot(mix, &self->mix, MP_QSTR_mix);

    // Allocate the chorus buffer for the max possible delay, chorus is always 16-bit
    self->max_delay_ms = max_delay_ms;
    self->max_chorus_buffer_len = (uint32_t)(self->base.sample_rate / MICROPY_FLOAT_CONST(1000.0) * max_delay_ms * (self->base.channel_count * sizeof(uint16_t))); // bytes
    self->chorus_buffer = m_malloc(self->max_chorus_buffer_len);
    memset(self->chorus_buffer, 0, self->max_chorus_buffer_len);

    self->sample_ms = MICROPY_FLOAT_CONST(1000.0) / self->base.sample_rate;

    mp_float_t f_delay_ms = synthio_block_slot_get(&self->delay_ms);
    chorus_recalculate_delay(self, f_delay_ms);

    self->chorus_buffer_pos = 0;
}

bool common_hal_audiodelays_chorus_deinited(audiodelays_chorus_obj_t *self) {
    return self->chorus_buffer == NULL;
}

void common_hal_audiodelays_chorus_deinit(audiodelays_chorus_obj_t *self) {
    // The whole body. mark_deinit is not the damage; the pointer
    // nulling AFTER it is -- the funnel's guard has already let a
    // pull in by then, and the pull writes into a buffer that has
    // just become NULL. Detach under the lock, free afterwards.
    audiodsp_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    self->chorus_buffer = NULL;
    self->buffer[0] = NULL;
    self->buffer[1] = NULL;
    audiodsp_pump_lock_release();
}

mp_obj_t common_hal_audiodelays_chorus_get_delay_ms(audiodelays_chorus_obj_t *self) {
    return self->delay_ms.obj;
}

void common_hal_audiodelays_chorus_set_delay_ms(audiodelays_chorus_obj_t *self, mp_obj_t delay_ms) {
    synthio_block_assign_slot(delay_ms, &self->delay_ms, MP_QSTR_delay_ms);

    mp_float_t f_delay_ms = synthio_block_slot_get(&self->delay_ms);

    chorus_recalculate_delay(self, f_delay_ms);
}

void chorus_recalculate_delay(audiodelays_chorus_obj_t *self, mp_float_t f_delay_ms) {
    // Require that delay is at least 1 sample long
    f_delay_ms = MAX(f_delay_ms, self->sample_ms);

    uint32_t new_chorus_buffer_len = (uint32_t)(self->base.sample_rate / MICROPY_FLOAT_CONST(1000.0) * f_delay_ms) * (self->base.channel_count * sizeof(uint16_t));

    self->chorus_buffer_len = new_chorus_buffer_len;

    self->current_delay_ms = f_delay_ms;
}

mp_obj_t common_hal_audiodelays_chorus_get_voices(audiodelays_chorus_obj_t *self) {
    return self->voices.obj;
}

void common_hal_audiodelays_chorus_set_voices(audiodelays_chorus_obj_t *self, mp_obj_t voices) {
    synthio_block_assign_slot(voices, &self->voices, MP_QSTR_voices);
}

void audiodelays_chorus_reset_buffer(audiodelays_chorus_obj_t *self,
    bool single_channel_output,
    uint8_t channel) {
    (void)single_channel_output;
    (void)channel;

    memset(self->buffer[0], 0, self->buffer_len);
    memset(self->buffer[1], 0, self->buffer_len);
    memset(self->chorus_buffer, 0, self->chorus_buffer_len);
}

mp_obj_t common_hal_audiodelays_chorus_get_mix(audiodelays_chorus_obj_t *self) {
    return self->mix.obj;
}

void common_hal_audiodelays_chorus_set_mix(audiodelays_chorus_obj_t *self, mp_obj_t arg) {
    synthio_block_assign_slot(arg, &self->mix, MP_QSTR_mix);
}

bool common_hal_audiodelays_chorus_get_playing(audiodelays_chorus_obj_t *self) {
    return self->sample != NULL;
}

void common_hal_audiodelays_chorus_play(audiodelays_chorus_obj_t *self, mp_obj_t sample, bool loop) {
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

void common_hal_audiodelays_chorus_stop(audiodelays_chorus_obj_t *self) {
    // Effect keeps ringing until its own caller stops pulling; only the
    // source sample stops.
    self->sample = NULL;
}

audioio_get_buffer_result_t audiodelays_chorus_get_buffer(audiodelays_chorus_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;

    self->last_buf_idx = !self->last_buf_idx;

    int16_t *word_buffer = (int16_t *)self->buffer[self->last_buf_idx];
    int8_t *hword_buffer = self->buffer[self->last_buf_idx];
    uint32_t length = self->buffer_len / (self->base.bits_per_sample / 8);

    int16_t *chorus_buffer = (int16_t *)self->chorus_buffer;
    uint32_t chorus_buf_len = self->chorus_buffer_len / sizeof(uint16_t);
    uint32_t max_chorus_buf_len = self->max_chorus_buffer_len / sizeof(uint16_t);

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

        uint32_t n;
        if (self->sample == NULL) {
            n = MIN(length, (uint32_t)(SYNTHIO_MAX_DUR * self->base.channel_count));
        } else {
            n = MIN(MIN(self->sample_buffer_length, length), (uint32_t)(SYNTHIO_MAX_DUR * self->base.channel_count));
        }

        shared_bindings_synthio_lfo_tick(self->base.sample_rate, n / self->base.channel_count);

        int32_t voices = (int32_t)MAX(synthio_block_slot_get(&self->voices), MICROPY_FLOAT_CONST(1.0));
        int32_t mix_down_scale = SYNTHIO_MIX_DOWN_SCALE(voices);
        mp_float_t mix = synthio_block_slot_get_limited(&self->mix, MICROPY_FLOAT_CONST(0.0), MICROPY_FLOAT_CONST(1.0));

        mp_float_t f_delay_ms = synthio_block_slot_get(&self->delay_ms);
        if (MICROPY_FLOAT_C_FUN(fabs)(self->current_delay_ms - f_delay_ms) >= self->sample_ms) {
            chorus_recalculate_delay(self, f_delay_ms);
        }

        if (self->sample == NULL) {
            if (self->base.samples_signed) {
                memset(word_buffer, 0, n * (self->base.bits_per_sample / 8));
            } else {
                // For unsigned samples set to the middle which is "quiet"
                if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                    uint16_t *uword_buffer = (uint16_t *)word_buffer;
                    for (uint32_t i = 0; i < n; i++) {
                        *uword_buffer++ = 32768;
                    }
                } else {
                    memset(hword_buffer, 128, n * (self->base.bits_per_sample / 8));
                }
            }
        } else {
            int16_t *sample_src = (int16_t *)self->sample_remaining_buffer;
            int8_t *sample_hsrc = (int8_t *)self->sample_remaining_buffer;

            if (self->base.bits_per_sample == 16 && self->base.samples_signed &&
                !single_channel_output) {
                self->chorus_buffer_pos = audiodsp_chorus_process_s16(
                    word_buffer, sample_src, n, chorus_buffer,
                    self->chorus_buffer_pos, chorus_buf_len,
                    max_chorus_buf_len, voices, mix);
                goto chorus_samples_done;
            }

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

                chorus_buffer[self->chorus_buffer_pos++] = (int16_t)sample_word;

                int32_t word = 0;
                if (voices == 1) {
                    word = sample_word;
                } else {
                    int32_t step = chorus_buf_len / (voices - 1) - 1;
                    int32_t c_pos = self->chorus_buffer_pos - 1;

                    for (int32_t v = 0; v < voices; v++) {
                        if (c_pos < 0) {
                            c_pos += max_chorus_buf_len;
                        }
                        word += chorus_buffer[c_pos];

                        c_pos -= step;
                    }

                    word = synthio_mix_down_sample(word, mix_down_scale);
                }

                word = sample_word + (int32_t)(word * mix);
                word = synthio_mix_down_sample(word, 2);

                if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                    word_buffer[i] = word;
                    if (!self->base.samples_signed) {
                        word_buffer[i] ^= 0x8000;
                    }
                } else {
                    int8_t out = word;
                    if (self->base.samples_signed) {
                        hword_buffer[i] = out;
                    } else {
                        hword_buffer[i] = (uint8_t)out ^ 0x80;
                    }
                }

                if (self->chorus_buffer_pos >= max_chorus_buf_len) {
                    self->chorus_buffer_pos = 0;
                }
            }
            chorus_samples_done:
            self->sample_remaining_buffer += (n * (self->base.bits_per_sample / 8));
            self->sample_buffer_length -= n;
        }
        length -= n;
        word_buffer += n;
        hword_buffer += n;
    }

    *buffer = (uint8_t *)self->buffer[self->last_buf_idx];
    *buffer_length = self->buffer_len;

    return GET_BUFFER_MORE_DATA;
}

// --- Python bindings (from shared-bindings/audiodelays/Chorus.c) ---------

static mp_obj_t audiodelays_chorus_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_max_delay_ms, ARG_delay_ms, ARG_voices, ARG_mix, ARG_buffer_size, ARG_sample_rate, ARG_bits_per_sample, ARG_samples_signed, ARG_channel_count, };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_max_delay_ms, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 50 } },
        { MP_QSTR_delay_ms, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_voices, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_mix, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_buffer_size, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 512} },
        { MP_QSTR_sample_rate, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 8000} },
        { MP_QSTR_bits_per_sample, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 16} },
        { MP_QSTR_samples_signed, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = true} },
        { MP_QSTR_channel_count, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1 } },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t max_delay_ms = mp_arg_validate_int_range(args[ARG_max_delay_ms].u_int, 1, 4000, MP_QSTR_max_delay_ms);

    mp_int_t channel_count = mp_arg_validate_int_range(args[ARG_channel_count].u_int, 1, 2, MP_QSTR_channel_count);
    mp_arg_validate_int_min(args[ARG_sample_rate].u_int, 1, MP_QSTR_sample_rate);
    mp_int_t bits_per_sample = args[ARG_bits_per_sample].u_int;
    if (bits_per_sample != 8 && bits_per_sample != 16) {
        mp_raise_ValueError(MP_ERROR_TEXT("bits_per_sample must be 8 or 16"));
    }

    audiodelays_chorus_obj_t *self = mp_obj_malloc(audiodelays_chorus_obj_t, &audiodelays_chorus_type);
    common_hal_audiodelays_chorus_construct(self, max_delay_ms, args[ARG_delay_ms].u_obj, args[ARG_voices].u_obj, args[ARG_mix].u_obj, args[ARG_buffer_size].u_int, bits_per_sample, args[ARG_samples_signed].u_bool, channel_count, args[ARG_sample_rate].u_int);

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audiodelays_chorus_deinit(mp_obj_t self_in) {
    audiodelays_chorus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_chorus_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_chorus_deinit_obj, audiodelays_chorus_deinit);

static void check_for_deinit(audiodelays_chorus_obj_t *self) {
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

static mp_obj_t audiodelays_chorus_obj___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    common_hal_audiodelays_chorus_deinit(args[0]);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiodelays_chorus___exit___obj, 4, 4, audiodelays_chorus_obj___exit__);

static mp_obj_t audiodelays_chorus_obj_get_delay_ms(mp_obj_t self_in) {
    return common_hal_audiodelays_chorus_get_delay_ms(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_chorus_get_delay_ms_obj, audiodelays_chorus_obj_get_delay_ms);

static mp_obj_t audiodelays_chorus_obj_set_delay_ms(mp_obj_t self_in, mp_obj_t delay_ms_in) {
    audiodelays_chorus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_chorus_set_delay_ms(self, delay_ms_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodelays_chorus_set_delay_ms_obj, audiodelays_chorus_obj_set_delay_ms);

MP_PROPERTY_GETSET(audiodelays_chorus_delay_ms_obj,
    (mp_obj_t)&audiodelays_chorus_get_delay_ms_obj,
    (mp_obj_t)&audiodelays_chorus_set_delay_ms_obj);

static mp_obj_t audiodelays_chorus_obj_get_voices(mp_obj_t self_in) {
    return common_hal_audiodelays_chorus_get_voices(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_chorus_get_voices_obj, audiodelays_chorus_obj_get_voices);

static mp_obj_t audiodelays_chorus_obj_set_voices(mp_obj_t self_in, mp_obj_t voices_in) {
    audiodelays_chorus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_chorus_set_voices(self, voices_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodelays_chorus_set_voices_obj, audiodelays_chorus_obj_set_voices);

MP_PROPERTY_GETSET(audiodelays_chorus_voices_obj,
    (mp_obj_t)&audiodelays_chorus_get_voices_obj,
    (mp_obj_t)&audiodelays_chorus_set_voices_obj);

static mp_obj_t audiodelays_chorus_obj_get_mix(mp_obj_t self_in) {
    return common_hal_audiodelays_chorus_get_mix(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_chorus_get_mix_obj, audiodelays_chorus_obj_get_mix);

static mp_obj_t audiodelays_chorus_obj_set_mix(mp_obj_t self_in, mp_obj_t mix_in) {
    audiodelays_chorus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_chorus_set_mix(self, mix_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodelays_chorus_set_mix_obj, audiodelays_chorus_obj_set_mix);

MP_PROPERTY_GETSET(audiodelays_chorus_mix_obj,
    (mp_obj_t)&audiodelays_chorus_get_mix_obj,
    (mp_obj_t)&audiodelays_chorus_set_mix_obj);

static mp_obj_t audiodelays_chorus_obj_get_playing(mp_obj_t self_in) {
    audiodelays_chorus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_bool(common_hal_audiodelays_chorus_get_playing(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_chorus_get_playing_obj, audiodelays_chorus_obj_get_playing);

MP_PROPERTY_GETTER(audiodelays_chorus_playing_obj,
    (mp_obj_t)&audiodelays_chorus_get_playing_obj);

static mp_obj_t audiodelays_chorus_obj_play(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_sample, ARG_loop };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sample,    MP_ARG_OBJ | MP_ARG_REQUIRED, {} },
        { MP_QSTR_loop,      MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
    };
    audiodelays_chorus_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    check_for_deinit(self);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_obj_t sample = args[ARG_sample].u_obj;
    common_hal_audiodelays_chorus_play(self, sample, args[ARG_loop].u_bool);

    return MP_OBJ_FROM_PTR(self);
}
MP_DEFINE_CONST_FUN_OBJ_KW(audiodelays_chorus_play_obj, 1, audiodelays_chorus_obj_play);

static mp_obj_t audiodelays_chorus_obj_stop(mp_obj_t self_in) {
    audiodelays_chorus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_chorus_stop(self);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_chorus_stop_obj, audiodelays_chorus_obj_stop);

static const mp_rom_map_elem_t audiodelays_chorus_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiodelays_chorus_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&audiodelays_chorus___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiodelays_chorus_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audiodelays_chorus_stop_obj) },

    { MP_ROM_QSTR(MP_QSTR_playing), MP_ROM_PTR(&audiodelays_chorus_playing_obj) },
    { MP_ROM_QSTR(MP_QSTR_delay_ms), MP_ROM_PTR(&audiodelays_chorus_delay_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_voices), MP_ROM_PTR(&audiodelays_chorus_voices_obj) },
    { MP_ROM_QSTR(MP_QSTR_mix), MP_ROM_PTR(&audiodelays_chorus_mix_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiodelays_chorus_locals_dict, audiodelays_chorus_locals_dict_table);

static const audiosample_p_t audiodelays_chorus_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audiodelays_chorus_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audiodelays_chorus_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiodelays_chorus_type,
    MP_QSTR_Chorus,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiodelays_chorus_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiodelays_chorus_locals_dict,
    protocol, &audiodelays_chorus_proto
    );
