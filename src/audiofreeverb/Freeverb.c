// Ported from CircuitPython's shared-bindings+shared-module/audiofreeverb/
// Freeverb.{h,c} (upstream repo: https://github.com/adafruit/circuitpython,
// MIT). Unchanged mixdown/DSP math, including upstream's `combfitlers`
// identifier typo (kept verbatim -- purely internal, no behavioral effect)
// and the type's lowercase `MP_QSTR_freeverb` name (so `type(x).__name__`
// prints "freeverb" even though the class is `audiofreeverb.Freeverb" --
// a genuine upstream quirk, kept for parity, not a transcription error).
// Type registration adds `attr, cp_compat_attr` for the roomsize/damp/mix/
// playing properties (see docs/upstream-diff.md, "Property invocation
// needs an explicit attr slot").
//
// Based on FreeVerb - https://github.com/sinshu/freeverb/tree/main
// Fixed point ideas from Paul Stoffregen's Teensy audio library
// (https://github.com/PaulStoffregen/Audio/blob/master/effect_freeverb.cpp)
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Mark Komus
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include "audiofreeverb/Freeverb.h"

#include <string.h>

#include "cp_compat/argcheck.h"
#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/objproperty.h"
#include "shared/audiodsp_freeverb.h"

#include "py/runtime.h"
#include "shared/audiodsp_pump_lock.h"

// --- shared-module (DSP engine) -------------------------------------------

void common_hal_audiofreeverb_freeverb_construct(audiofreeverb_freeverb_obj_t *self, mp_obj_t roomsize, mp_obj_t damp, mp_obj_t pre_filter, mp_obj_t post_filter, mp_obj_t mix,
    uint32_t buffer_size, uint8_t bits_per_sample,
    bool samples_signed, uint8_t channel_count, uint32_t sample_rate) {

    self->base.bits_per_sample = bits_per_sample;
    self->base.samples_signed = samples_signed;
    self->base.channel_count = channel_count;
    self->base.sample_rate = sample_rate;
    self->base.single_buffer = false;
    self->base.max_buffer_length = buffer_size;

    self->buffer_len = buffer_size; // in bytes

    self->buffer[0] = m_malloc_maybe(self->buffer_len);
    if (self->buffer[0] == NULL) {
        common_hal_audiofreeverb_freeverb_deinit(self);
        m_malloc_fail(self->buffer_len);
    }
    memset(self->buffer[0], 0, self->buffer_len);

    self->buffer[1] = m_malloc_maybe(self->buffer_len);
    if (self->buffer[1] == NULL) {
        common_hal_audiofreeverb_freeverb_deinit(self);
        m_malloc_fail(self->buffer_len);
    }
    memset(self->buffer[1], 0, self->buffer_len);

    self->last_buf_idx = 1;

    self->sample = NULL;
    self->sample_remaining_buffer = NULL;
    self->sample_buffer_length = 0;
    self->loop = false;
    self->more_data = false;

    if (roomsize == MP_OBJ_NULL) {
        roomsize = mp_obj_new_float(MICROPY_FLOAT_CONST(0.5));
    }
    synthio_block_assign_slot(roomsize, &self->roomsize, MP_QSTR_roomsize);
    common_hal_audiofreeverb_freeverb_set_roomsize(self, roomsize);

    if (damp == MP_OBJ_NULL) {
        damp = mp_obj_new_float(MICROPY_FLOAT_CONST(0.5));
    }
    synthio_block_assign_slot(damp, &self->damp, MP_QSTR_damp);
    common_hal_audiofreeverb_freeverb_set_damp(self, damp);

    common_hal_audiofreeverb_freeverb_set_pre_filter(self, pre_filter);
    common_hal_audiofreeverb_freeverb_set_post_filter(self, post_filter);

    if (mix == MP_OBJ_NULL) {
        mix = mp_obj_new_float(MICROPY_FLOAT_CONST(0.5));
    }
    synthio_block_assign_slot(mix, &self->mix, MP_QSTR_mix);
    common_hal_audiofreeverb_freeverb_set_mix(self, mix);

    // Values below come from FreeVerb, selected for the best reverb sound.
    self->combbuffersizes[0] = self->combbuffersizes[8] = 1116;
    self->combbuffersizes[1] = self->combbuffersizes[9] = 1188;
    self->combbuffersizes[2] = self->combbuffersizes[10] = 1277;
    self->combbuffersizes[3] = self->combbuffersizes[11] = 1356;
    self->combbuffersizes[4] = self->combbuffersizes[12] = 1422;
    self->combbuffersizes[5] = self->combbuffersizes[13] = 1491;
    self->combbuffersizes[6] = self->combbuffersizes[14] = 1557;
    self->combbuffersizes[7] = self->combbuffersizes[15] = 1617;
    for (uint32_t i = 0; i < 8 * channel_count; i++) {
        self->combbuffers[i] = m_malloc_maybe(self->combbuffersizes[i] * sizeof(uint16_t));
        if (self->combbuffers[i] == NULL) {
            common_hal_audiofreeverb_freeverb_deinit(self);
            m_malloc_fail(self->combbuffersizes[i]);
        }
        memset(self->combbuffers[i], 0, self->combbuffersizes[i]);

        self->combbufferindex[i] = 0;
        self->combfitlers[i] = 0;
    }

    self->allpassbuffersizes[0] = self->allpassbuffersizes[4] = 556;
    self->allpassbuffersizes[1] = self->allpassbuffersizes[5] = 441;
    self->allpassbuffersizes[2] = self->allpassbuffersizes[6] = 341;
    self->allpassbuffersizes[3] = self->allpassbuffersizes[7] = 225;
    for (uint32_t i = 0; i < 4 * channel_count; i++) {
        self->allpassbuffers[i] = m_malloc_maybe(self->allpassbuffersizes[i] * sizeof(uint16_t));
        if (self->allpassbuffers[i] == NULL) {
            common_hal_audiofreeverb_freeverb_deinit(self);
            m_malloc_fail(self->allpassbuffersizes[i]);
        }
        memset(self->allpassbuffers[i], 0, self->allpassbuffersizes[i]);

        self->allpassbufferindex[i] = 0;
    }
}

bool common_hal_audiofreeverb_freeverb_deinited(audiofreeverb_freeverb_obj_t *self) {
    return self->buffer[0] == NULL;
}

void common_hal_audiofreeverb_freeverb_deinit(audiofreeverb_freeverb_obj_t *self) {
    // The whole body. mark_deinit is not the damage; the pointer
    // nulling AFTER it is -- the funnel's guard has already let a
    // pull in by then, and the pull writes into a buffer that has
    // just become NULL. Detach under the lock, free afterwards.
    audiodsp_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    audiofilters_deinit_filter_chain(&self->pre_filter);
    audiofilters_deinit_filter_chain(&self->post_filter);
    self->buffer[0] = NULL;
    self->buffer[1] = NULL;
    audiodsp_pump_lock_release();
}

mp_obj_t common_hal_audiofreeverb_freeverb_get_roomsize(audiofreeverb_freeverb_obj_t *self) {
    return self->roomsize.obj;
}

void common_hal_audiofreeverb_freeverb_set_roomsize(audiofreeverb_freeverb_obj_t *self, mp_obj_t roomsize_obj) {
    synthio_block_assign_slot(roomsize_obj, &self->roomsize, MP_QSTR_roomsize);
}

int16_t audiofreeverb_freeverb_get_roomsize_fixedpoint(mp_float_t n) {
    if (n > (mp_float_t)MICROPY_FLOAT_CONST(1.0)) {
        n = MICROPY_FLOAT_CONST(1.0);
    } else if (n < (mp_float_t)MICROPY_FLOAT_CONST(0.0)) {
        n = MICROPY_FLOAT_CONST(0.0);
    }

    return (int16_t)(n * (mp_float_t)MICROPY_FLOAT_CONST(9175.04)) + 22937; // 9175.04 = 0.28f fixed-point, 22937 = 0.7f
}

mp_obj_t common_hal_audiofreeverb_freeverb_get_damp(audiofreeverb_freeverb_obj_t *self) {
    return self->damp.obj;
}

void common_hal_audiofreeverb_freeverb_set_damp(audiofreeverb_freeverb_obj_t *self, mp_obj_t damp) {
    synthio_block_assign_slot(damp, &self->damp, MP_QSTR_damp);
}

mp_obj_t common_hal_audiofreeverb_freeverb_get_pre_filter(audiofreeverb_freeverb_obj_t *self) {
    return self->pre_filter.obj;
}

void common_hal_audiofreeverb_freeverb_set_pre_filter(audiofreeverb_freeverb_obj_t *self, mp_obj_t filter_in) {
    audiofilters_assign_filter_chain(&self->pre_filter, filter_in, self->base.channel_count);
}

mp_obj_t common_hal_audiofreeverb_freeverb_get_post_filter(audiofreeverb_freeverb_obj_t *self) {
    return self->post_filter.obj;
}

void common_hal_audiofreeverb_freeverb_set_post_filter(audiofreeverb_freeverb_obj_t *self, mp_obj_t filter_in) {
    audiofilters_assign_filter_chain(&self->post_filter, filter_in, self->base.channel_count);
}

void audiofreeverb_freeverb_get_damp_fixedpoint(mp_float_t n, int16_t *damp1, int16_t *damp2) {
    if (n > (mp_float_t)MICROPY_FLOAT_CONST(1.0)) {
        n = MICROPY_FLOAT_CONST(1.0);
    } else if (n < (mp_float_t)MICROPY_FLOAT_CONST(0.0)) {
        n = MICROPY_FLOAT_CONST(0.0);
    }

    *damp1 = (int16_t)(n * (mp_float_t)MICROPY_FLOAT_CONST(13107.2)); // 0.4f scaling factor
    *damp2 = (int16_t)(32768 - *damp1); // inverse of damp1
}

mp_obj_t common_hal_audiofreeverb_freeverb_get_mix(audiofreeverb_freeverb_obj_t *self) {
    return self->mix.obj;
}

void common_hal_audiofreeverb_freeverb_set_mix(audiofreeverb_freeverb_obj_t *self, mp_obj_t mix) {
    synthio_block_assign_slot(mix, &self->mix, MP_QSTR_mix);
}

void audiofreeverb_freeverb_get_mix_fixedpoint(mp_float_t mix, int16_t *mix_sample, int16_t *mix_effect) {
    mix = mix * (mp_float_t)MICROPY_FLOAT_CONST(2.0);
    *mix_sample = (int16_t)(MIN((mp_float_t)MICROPY_FLOAT_CONST(2.0) - mix, (mp_float_t)MICROPY_FLOAT_CONST(1.0)) * 32767);
    *mix_effect = (int16_t)(MIN(mix, (mp_float_t)MICROPY_FLOAT_CONST(1.0)) * 32767);
}

void audiofreeverb_freeverb_reset_buffer(audiofreeverb_freeverb_obj_t *self,
    bool single_channel_output,
    uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    memset(self->buffer[0], 0, self->buffer_len);
    memset(self->buffer[1], 0, self->buffer_len);
    audiofilters_reset_filter_chain(&self->pre_filter, self->base.channel_count);
    audiofilters_reset_filter_chain(&self->post_filter, self->base.channel_count);
}

bool common_hal_audiofreeverb_freeverb_get_playing(audiofreeverb_freeverb_obj_t *self) {
    return self->sample != NULL;
}

void common_hal_audiofreeverb_freeverb_play(audiofreeverb_freeverb_obj_t *self, mp_obj_t sample, bool loop) {
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

void common_hal_audiofreeverb_freeverb_stop(audiofreeverb_freeverb_obj_t *self) {
    // Effect keeps ringing until its own caller stops pulling; only the
    // source sample stops.
    self->sample = NULL;
}

audioio_get_buffer_result_t audiofreeverb_freeverb_get_buffer(audiofreeverb_freeverb_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;

    self->last_buf_idx = !self->last_buf_idx;

    int16_t *word_buffer = (int16_t *)self->buffer[self->last_buf_idx];
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

        uint32_t n;
        if (self->sample == NULL) {
            n = MIN(length, (uint32_t)(SYNTHIO_MAX_DUR * self->base.channel_count));
        } else {
            n = MIN(MIN(self->sample_buffer_length, length), (uint32_t)(SYNTHIO_MAX_DUR * self->base.channel_count));
        }

        shared_bindings_synthio_lfo_tick(self->base.sample_rate, n / self->base.channel_count);
        mp_float_t damp = synthio_block_slot_get_limited(&self->damp, MICROPY_FLOAT_CONST(0.0), MICROPY_FLOAT_CONST(1.0));
        mp_float_t mix = synthio_block_slot_get_limited(&self->mix, MICROPY_FLOAT_CONST(0.0), MICROPY_FLOAT_CONST(1.0));
        mp_float_t roomsize = synthio_block_slot_get_limited(&self->roomsize, MICROPY_FLOAT_CONST(0.0), MICROPY_FLOAT_CONST(1.0));
        int16_t *sample_src = (int16_t *)self->sample_remaining_buffer;
        audiofilters_tick_filter_chain(&self->pre_filter);
        audiofilters_tick_filter_chain(&self->post_filter);
        if (self->pre_filter.objs_len == 0 && self->post_filter.objs_len == 0) {
            audiodsp_freeverb_process_s16_banks(word_buffer,
                self->sample == NULL ? NULL : sample_src, n, self->combbuffers,
                self->combbuffersizes, self->combbufferindex, self->combfitlers,
                self->allpassbuffers, self->allpassbuffersizes,
                self->allpassbufferindex, roomsize, damp, mix);
        } else {
            int16_t damp1, damp2;
            audiofreeverb_freeverb_get_damp_fixedpoint(damp, &damp1, &damp2);
            int16_t mix_sample, mix_effect;
            audiofreeverb_freeverb_get_mix_fixedpoint(mix, &mix_sample, &mix_effect);
            int16_t feedback = audiofreeverb_freeverb_get_roomsize_fixedpoint(roomsize);
            for (uint32_t i = 0; i < n; i++) {
                int32_t sample_word = 0;
                if (self->sample != NULL) {
                    sample_word = sample_src[i];
                }
                int32_t word, sum;
                int16_t input, bufout, output;
                uint32_t channel_comb_offset = 0, channel_allpass_offset = 0;
                input = (int16_t)audiofilters_process_filter_chain(
                    &self->pre_filter, self->base.channel_count,
                    n % self->base.channel_count, sample_word);
                input = synthio_sat16((int32_t)input * 8738, 17);
                sum = 0;
                for (uint32_t j = 0 + channel_comb_offset; j < 8 + channel_comb_offset; j++) {
                    bufout = self->combbuffers[j][self->combbufferindex[j]];
                    sum += bufout;
                    self->combfitlers[j] = synthio_sat16(bufout * damp2 + self->combfitlers[j] * damp1, 15);
                    self->combbuffers[j][self->combbufferindex[j]] = synthio_sat16(input + synthio_sat16(self->combfitlers[j] * feedback, 15), 0);
                    if (++self->combbufferindex[j] >= self->combbuffersizes[j]) {
                        self->combbufferindex[j] = 0;
                    }
                }
                output = synthio_sat16(sum * 31457, 17);
                for (uint32_t j = 0 + channel_allpass_offset; j < 4 + channel_allpass_offset; j++) {
                    bufout = self->allpassbuffers[j][self->allpassbufferindex[j]];
                    self->allpassbuffers[j][self->allpassbufferindex[j]] = output + (bufout >> 1);
                    output = synthio_sat16(bufout - output, 1);
                    if (++self->allpassbufferindex[j] >= self->allpassbuffersizes[j]) {
                        self->allpassbufferindex[j] = 0;
                    }
                }
                output = (int16_t)audiofilters_process_filter_chain(
                    &self->post_filter, self->base.channel_count,
                    n % self->base.channel_count, (int32_t)output);
                word = output * 30;
                word = synthio_sat16(sample_word * mix_sample, 15) + synthio_sat16(word * mix_effect, 15);
                word = synthio_mix_down_sample(word, SYNTHIO_MIX_DOWN_SCALE(2));
                word_buffer[i] = (int16_t)word;
                if ((self->base.channel_count == 2) && (channel_comb_offset == 0)) {
                    channel_comb_offset = 8;
                    channel_allpass_offset = 4;
                } else {
                    channel_comb_offset = 0;
                    channel_allpass_offset = 0;
                }
            }
        }

        length -= n;
        word_buffer += n;
        self->sample_remaining_buffer += (n * (self->base.bits_per_sample / 8));
        self->sample_buffer_length -= n;
    }

    *buffer = (uint8_t *)self->buffer[self->last_buf_idx];
    *buffer_length = self->buffer_len;

    return GET_BUFFER_MORE_DATA;
}

// --- Python bindings (from shared-bindings/audiofreeverb/Freeverb.c) -----

static mp_obj_t audiofreeverb_freeverb_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_roomsize, ARG_damp, ARG_pre_filter, ARG_post_filter, ARG_mix, ARG_buffer_size, ARG_sample_rate, ARG_bits_per_sample, ARG_samples_signed, ARG_channel_count, };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_roomsize, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_damp, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_pre_filter, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_NONE } },
        { MP_QSTR_post_filter, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_NONE } },
        { MP_QSTR_mix, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
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
    if (args[ARG_samples_signed].u_bool != true) {
        mp_raise_ValueError(MP_ERROR_TEXT("samples_signed must be true"));
    }
    mp_int_t bits_per_sample = args[ARG_bits_per_sample].u_int;
    if (bits_per_sample != 16) {
        mp_raise_ValueError(MP_ERROR_TEXT("bits_per_sample must be 16"));
    }

    audiofreeverb_freeverb_obj_t *self = mp_obj_malloc(audiofreeverb_freeverb_obj_t, &audiofreeverb_freeverb_type);
    common_hal_audiofreeverb_freeverb_construct(self, args[ARG_roomsize].u_obj, args[ARG_damp].u_obj, args[ARG_pre_filter].u_obj, args[ARG_post_filter].u_obj, args[ARG_mix].u_obj,
        args[ARG_buffer_size].u_int, bits_per_sample, args[ARG_samples_signed].u_bool, channel_count, args[ARG_sample_rate].u_int);

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audiofreeverb_freeverb_deinit(mp_obj_t self_in) {
    audiofreeverb_freeverb_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiofreeverb_freeverb_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofreeverb_freeverb_deinit_obj, audiofreeverb_freeverb_deinit);

static void check_for_deinit(audiofreeverb_freeverb_obj_t *self) {
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

static mp_obj_t audiofreeverb_freeverb_obj_get_roomsize(mp_obj_t self_in) {
    return common_hal_audiofreeverb_freeverb_get_roomsize(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiofreeverb_freeverb_get_roomsize_obj, audiofreeverb_freeverb_obj_get_roomsize);

static mp_obj_t audiofreeverb_freeverb_obj_set_roomsize(mp_obj_t self_in, mp_obj_t roomsize) {
    audiofreeverb_freeverb_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiofreeverb_freeverb_set_roomsize(self, roomsize);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiofreeverb_freeverb_set_roomsize_obj, audiofreeverb_freeverb_obj_set_roomsize);

MP_PROPERTY_GETSET(audiofreeverb_freeverb_roomsize_obj,
    (mp_obj_t)&audiofreeverb_freeverb_get_roomsize_obj,
    (mp_obj_t)&audiofreeverb_freeverb_set_roomsize_obj);

static mp_obj_t audiofreeverb_freeverb_obj_get_damp(mp_obj_t self_in) {
    return common_hal_audiofreeverb_freeverb_get_damp(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiofreeverb_freeverb_get_damp_obj, audiofreeverb_freeverb_obj_get_damp);

static mp_obj_t audiofreeverb_freeverb_obj_set_damp(mp_obj_t self_in, mp_obj_t damp) {
    audiofreeverb_freeverb_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiofreeverb_freeverb_set_damp(self, damp);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiofreeverb_freeverb_set_damp_obj, audiofreeverb_freeverb_obj_set_damp);

MP_PROPERTY_GETSET(audiofreeverb_freeverb_damp_obj,
    (mp_obj_t)&audiofreeverb_freeverb_get_damp_obj,
    (mp_obj_t)&audiofreeverb_freeverb_set_damp_obj);

static mp_obj_t audiofreeverb_freeverb_obj_get_pre_filter(mp_obj_t self_in) {
    return common_hal_audiofreeverb_freeverb_get_pre_filter(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiofreeverb_freeverb_get_pre_filter_obj, audiofreeverb_freeverb_obj_get_pre_filter);

static mp_obj_t audiofreeverb_freeverb_obj_set_pre_filter(mp_obj_t self_in, mp_obj_t filter_in) {
    audiofreeverb_freeverb_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiofreeverb_freeverb_set_pre_filter(self, filter_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiofreeverb_freeverb_set_pre_filter_obj, audiofreeverb_freeverb_obj_set_pre_filter);

MP_PROPERTY_GETSET(audiofreeverb_freeverb_pre_filter_obj,
    (mp_obj_t)&audiofreeverb_freeverb_get_pre_filter_obj,
    (mp_obj_t)&audiofreeverb_freeverb_set_pre_filter_obj);

static mp_obj_t audiofreeverb_freeverb_obj_get_post_filter(mp_obj_t self_in) {
    return common_hal_audiofreeverb_freeverb_get_post_filter(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiofreeverb_freeverb_get_post_filter_obj, audiofreeverb_freeverb_obj_get_post_filter);

static mp_obj_t audiofreeverb_freeverb_obj_set_post_filter(mp_obj_t self_in, mp_obj_t filter_in) {
    audiofreeverb_freeverb_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiofreeverb_freeverb_set_post_filter(self, filter_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiofreeverb_freeverb_set_post_filter_obj, audiofreeverb_freeverb_obj_set_post_filter);

MP_PROPERTY_GETSET(audiofreeverb_freeverb_post_filter_obj,
    (mp_obj_t)&audiofreeverb_freeverb_get_post_filter_obj,
    (mp_obj_t)&audiofreeverb_freeverb_set_post_filter_obj);

static mp_obj_t audiofreeverb_freeverb_obj_get_mix(mp_obj_t self_in) {
    return common_hal_audiofreeverb_freeverb_get_mix(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiofreeverb_freeverb_get_mix_obj, audiofreeverb_freeverb_obj_get_mix);

static mp_obj_t audiofreeverb_freeverb_obj_set_mix(mp_obj_t self_in, mp_obj_t mix_in) {
    audiofreeverb_freeverb_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiofreeverb_freeverb_set_mix(self, mix_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiofreeverb_freeverb_set_mix_obj, audiofreeverb_freeverb_obj_set_mix);

MP_PROPERTY_GETSET(audiofreeverb_freeverb_mix_obj,
    (mp_obj_t)&audiofreeverb_freeverb_get_mix_obj,
    (mp_obj_t)&audiofreeverb_freeverb_set_mix_obj);

static mp_obj_t audiofreeverb_freeverb_obj_get_playing(mp_obj_t self_in) {
    audiofreeverb_freeverb_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_bool(common_hal_audiofreeverb_freeverb_get_playing(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiofreeverb_freeverb_get_playing_obj, audiofreeverb_freeverb_obj_get_playing);

MP_PROPERTY_GETTER(audiofreeverb_freeverb_playing_obj,
    (mp_obj_t)&audiofreeverb_freeverb_get_playing_obj);

static mp_obj_t audiofreeverb_freeverb_obj_play(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_sample, ARG_loop };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sample,    MP_ARG_OBJ | MP_ARG_REQUIRED, {} },
        { MP_QSTR_loop,      MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
    };
    audiofreeverb_freeverb_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    check_for_deinit(self);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_obj_t sample = args[ARG_sample].u_obj;
    common_hal_audiofreeverb_freeverb_play(self, sample, args[ARG_loop].u_bool);

    return MP_OBJ_FROM_PTR(self);
}
MP_DEFINE_CONST_FUN_OBJ_KW(audiofreeverb_freeverb_play_obj, 1, audiofreeverb_freeverb_obj_play);

static mp_obj_t audiofreeverb_freeverb_obj_stop(mp_obj_t self_in) {
    audiofreeverb_freeverb_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiofreeverb_freeverb_stop(self);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiofreeverb_freeverb_stop_obj, audiofreeverb_freeverb_obj_stop);

static const mp_rom_map_elem_t audiofreeverb_freeverb_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiofreeverb_freeverb_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiofreeverb_freeverb_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audiofreeverb_freeverb_stop_obj) },

    { MP_ROM_QSTR(MP_QSTR_playing), MP_ROM_PTR(&audiofreeverb_freeverb_playing_obj) },
    { MP_ROM_QSTR(MP_QSTR_roomsize), MP_ROM_PTR(&audiofreeverb_freeverb_roomsize_obj) },
    { MP_ROM_QSTR(MP_QSTR_damp), MP_ROM_PTR(&audiofreeverb_freeverb_damp_obj) },
    { MP_ROM_QSTR(MP_QSTR_pre_filter), MP_ROM_PTR(&audiofreeverb_freeverb_pre_filter_obj) },
    { MP_ROM_QSTR(MP_QSTR_post_filter), MP_ROM_PTR(&audiofreeverb_freeverb_post_filter_obj) },
    { MP_ROM_QSTR(MP_QSTR_mix), MP_ROM_PTR(&audiofreeverb_freeverb_mix_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiofreeverb_freeverb_locals_dict, audiofreeverb_freeverb_locals_dict_table);

// What is behind this node, for audiosample_find_type()'s walk.
// audiodsp#112: the pump refuses a file-backed source at handover,
// and the protocol had no way to look past the tail.
static mp_obj_t audiofreeverb_freeverb_sources(mp_obj_t self_in, uint8_t index) {
    return index == 0 ? ((audiofreeverb_freeverb_obj_t *)MP_OBJ_TO_PTR(self_in))->sample
                      : MP_OBJ_NULL;
}

static const audiosample_p_t audiofreeverb_freeverb_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audiofreeverb_freeverb_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audiofreeverb_freeverb_get_buffer,
    .sources = audiofreeverb_freeverb_sources,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiofreeverb_freeverb_type,
    MP_QSTR_freeverb,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiofreeverb_freeverb_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiofreeverb_freeverb_locals_dict,
    protocol, &audiofreeverb_freeverb_proto
    );
