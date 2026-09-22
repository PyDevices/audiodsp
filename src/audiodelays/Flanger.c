// Ported from CircuitPython's shared-bindings+shared-module/audiodelays/
// Flanger.{h,c} (upstream repo: https://github.com/adafruit/circuitpython,
// MIT). Deviation: m_malloc_without_collect -> m_malloc (no mainline
// equivalent, see docs/upstream-diff.md). `attr, cp_compat_attr` added for
// min_delay_ms/rate/depth/feedback/mix/invert/lfo_value/playing. The custom
// `__exit__` (calling common_hal_audiodelays_flanger_deinit directly
// instead of the generic default___exit___obj method-dispatch helper) is
// kept verbatim -- it's upstream's own micro-optimization for this one
// type, not a port artifact. The signed-16 kernel follows the CPython
// twin's arithmetic where the two differ (audiodsp#74).
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Tim Cocks for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include "audiodelays/Flanger.h"

#include <math.h>
#include <string.h>

#include "cp_compat/argcheck.h"
#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/objproperty.h"

#include "py/runtime.h"
#include "shared/audiodsp_flanger.h"
#include "shared/audiodsp_pump_lock.h"

// --- shared-module (DSP engine) -------------------------------------------

void common_hal_audiodelays_flanger_construct(audiodelays_flanger_obj_t *self, uint32_t max_delay_ms,
    mp_obj_t min_delay_ms, mp_obj_t rate, mp_obj_t depth, mp_obj_t feedback, mp_obj_t mix, bool invert,
    uint32_t buffer_size, uint8_t bits_per_sample,
    bool samples_signed, uint8_t channel_count, uint32_t sample_rate) {

    self->base.bits_per_sample = bits_per_sample;
    self->base.samples_signed = samples_signed;
    self->base.channel_count = channel_count;
    self->base.sample_rate = sample_rate;
    self->base.single_buffer = false;
    self->base.max_buffer_length = buffer_size;

    self->buffer_len = buffer_size;

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

    if (min_delay_ms == MP_OBJ_NULL) {
        min_delay_ms = mp_obj_new_float(MICROPY_FLOAT_CONST(1.0));
    }
    synthio_block_assign_slot(min_delay_ms, &self->min_delay_ms, MP_QSTR_min_delay_ms);

    if (rate == MP_OBJ_NULL) {
        rate = mp_obj_new_float(MICROPY_FLOAT_CONST(0.5));
    }
    synthio_block_assign_slot(rate, &self->rate, MP_QSTR_rate);

    if (depth == MP_OBJ_NULL) {
        depth = mp_obj_new_float(MICROPY_FLOAT_CONST(0.5));
    }
    synthio_block_assign_slot(depth, &self->depth, MP_QSTR_depth);

    if (feedback == MP_OBJ_NULL) {
        feedback = mp_obj_new_float(MICROPY_FLOAT_CONST(0.5));
    }
    synthio_block_assign_slot(feedback, &self->feedback, MP_QSTR_feedback);

    if (mix == MP_OBJ_NULL) {
        mix = mp_obj_new_float(MICROPY_FLOAT_CONST(0.5));
    }
    synthio_block_assign_slot(mix, &self->mix, MP_QSTR_mix);

    self->invert = invert;
    self->max_delay_ms = max_delay_ms;
    self->sample_ms = MICROPY_FLOAT_CONST(1000.0) / self->base.sample_rate;

    self->delay_buffer_frames = (uint32_t)(self->base.sample_rate / MICROPY_FLOAT_CONST(1000.0) * max_delay_ms) + 2;
    size_t delay_buffer_size = self->delay_buffer_frames * self->base.channel_count * sizeof(int16_t);
    self->delay_buffer = m_malloc(delay_buffer_size);
    memset(self->delay_buffer, 0, delay_buffer_size);

    self->flanger.write_pos[0] = self->flanger.write_pos[1] = 0;
    self->flanger.lfo_phase[0] = self->flanger.lfo_phase[1] = 0;
}

bool common_hal_audiodelays_flanger_deinited(audiodelays_flanger_obj_t *self) {
    return self->delay_buffer == NULL;
}

void common_hal_audiodelays_flanger_deinit(audiodelays_flanger_obj_t *self) {
    // The whole body. mark_deinit is not the damage; the pointer
    // nulling AFTER it is -- the funnel's guard has already let a
    // pull in by then, and the pull writes into a buffer that has
    // just become NULL. Detach under the lock, free afterwards.
    audiodsp_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    self->delay_buffer = NULL;
    self->buffer[0] = NULL;
    self->buffer[1] = NULL;
    audiodsp_pump_lock_release();
}

mp_obj_t common_hal_audiodelays_flanger_get_min_delay_ms(audiodelays_flanger_obj_t *self) {
    return self->min_delay_ms.obj;
}

void common_hal_audiodelays_flanger_set_min_delay_ms(audiodelays_flanger_obj_t *self, mp_obj_t min_delay_ms) {
    synthio_block_assign_slot(min_delay_ms, &self->min_delay_ms, MP_QSTR_min_delay_ms);
}

mp_obj_t common_hal_audiodelays_flanger_get_rate(audiodelays_flanger_obj_t *self) {
    return self->rate.obj;
}

void common_hal_audiodelays_flanger_set_rate(audiodelays_flanger_obj_t *self, mp_obj_t rate) {
    synthio_block_assign_slot(rate, &self->rate, MP_QSTR_rate);
}

mp_obj_t common_hal_audiodelays_flanger_get_depth(audiodelays_flanger_obj_t *self) {
    return self->depth.obj;
}

void common_hal_audiodelays_flanger_set_depth(audiodelays_flanger_obj_t *self, mp_obj_t depth) {
    synthio_block_assign_slot(depth, &self->depth, MP_QSTR_depth);
}

mp_obj_t common_hal_audiodelays_flanger_get_feedback(audiodelays_flanger_obj_t *self) {
    return self->feedback.obj;
}

void common_hal_audiodelays_flanger_set_feedback(audiodelays_flanger_obj_t *self, mp_obj_t feedback) {
    synthio_block_assign_slot(feedback, &self->feedback, MP_QSTR_feedback);
}

mp_obj_t common_hal_audiodelays_flanger_get_mix(audiodelays_flanger_obj_t *self) {
    return self->mix.obj;
}

void common_hal_audiodelays_flanger_set_mix(audiodelays_flanger_obj_t *self, mp_obj_t arg) {
    synthio_block_assign_slot(arg, &self->mix, MP_QSTR_mix);
}

mp_float_t common_hal_audiodelays_flanger_get_lfo_value(audiodelays_flanger_obj_t *self) {
    return (mp_float_t)audiodsp_flanger_triangle(self->flanger.lfo_phase[0]) /
        MICROPY_FLOAT_CONST(65535.0);
}

bool common_hal_audiodelays_flanger_get_invert(audiodelays_flanger_obj_t *self) {
    return self->invert;
}

void common_hal_audiodelays_flanger_set_invert(audiodelays_flanger_obj_t *self, bool invert) {
    self->invert = invert;
}

void audiodelays_flanger_reset_buffer(audiodelays_flanger_obj_t *self,
    bool single_channel_output,
    uint8_t channel) {
    (void)single_channel_output;
    (void)channel;

    memset(self->buffer[0], 0, self->buffer_len);
    memset(self->buffer[1], 0, self->buffer_len);
    memset(self->delay_buffer, 0, self->delay_buffer_frames * self->base.channel_count * sizeof(int16_t));
    self->flanger.write_pos[0] = self->flanger.write_pos[1] = 0;
    self->flanger.lfo_phase[0] = self->flanger.lfo_phase[1] = 0;
}

bool common_hal_audiodelays_flanger_get_playing(audiodelays_flanger_obj_t *self) {
    return self->sample != NULL;
}

void common_hal_audiodelays_flanger_play(audiodelays_flanger_obj_t *self, mp_obj_t sample, bool loop) {
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

void common_hal_audiodelays_flanger_stop(audiodelays_flanger_obj_t *self) {
    self->sample = NULL;
}

audioio_get_buffer_result_t audiodelays_flanger_get_buffer(audiodelays_flanger_obj_t *self, bool single_channel_output, uint8_t channel,
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

        uint32_t n;
        if (self->sample == NULL) {
            n = MIN(length, (uint32_t)(SYNTHIO_MAX_DUR * self->base.channel_count));
        } else {
            n = MIN(MIN(self->sample_buffer_length, length), (uint32_t)(SYNTHIO_MAX_DUR * self->base.channel_count));
        }

        shared_bindings_synthio_lfo_tick(self->base.sample_rate, n / self->base.channel_count);

        mp_float_t f_min_delay_ms = synthio_block_slot_get_limited(&self->min_delay_ms, self->sample_ms, (mp_float_t)self->max_delay_ms);
        mp_float_t f_rate = synthio_block_slot_get_limited(&self->rate, MICROPY_FLOAT_CONST(0.0), MICROPY_FLOAT_CONST(20.0));
        mp_float_t f_depth = synthio_block_slot_get_limited(&self->depth, MICROPY_FLOAT_CONST(0.0), MICROPY_FLOAT_CONST(1.0));
        int32_t feedback = (int32_t)(synthio_block_slot_get_limited(&self->feedback, MICROPY_FLOAT_CONST(-0.95), MICROPY_FLOAT_CONST(0.95)) * 32767);
        int32_t mix = (int32_t)(synthio_block_slot_get_limited(&self->mix, MICROPY_FLOAT_CONST(0.0), MICROPY_FLOAT_CONST(1.0)) * 32767);

        mp_float_t sweep_top_ms = f_min_delay_ms + f_depth * ((mp_float_t)self->max_delay_ms - f_min_delay_ms);
        uint32_t delay_min = audiodsp_flanger_ms_to_frames_q16(f_min_delay_ms, self->base.sample_rate, self->delay_buffer_frames);
        uint32_t delay_span = audiodsp_flanger_ms_to_frames_q16(sweep_top_ms, self->base.sample_rate, self->delay_buffer_frames) - delay_min;
        mp_float_t phase_inc_f = MIN(f_rate / self->base.sample_rate, MICROPY_FLOAT_CONST(0.5));
        uint32_t phase_inc = (uint32_t)(phase_inc_f * MICROPY_FLOAT_CONST(4294967296.0));

        if (self->sample == NULL) {
            if (self->base.samples_signed) {
                memset(word_buffer, 0, n * (self->base.bits_per_sample / 8));
            } else {
                if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                    uint16_t *uword_buffer = (uint16_t *)word_buffer;
                    for (uint32_t i = 0; i < n; i++) {
                        *uword_buffer++ = 32768;
                    }
                } else {
                    memset(hword_buffer, 128, n * (self->base.bits_per_sample / 8));
                }
            }
        } else if (self->base.bits_per_sample == 16 && self->base.samples_signed &&
            !single_channel_output) {
            audiodsp_flanger_process_s16(word_buffer,
                (int16_t *)self->sample_remaining_buffer, n, self->delay_buffer,
                self->delay_buffer_frames, self->base.channel_count,
                &self->flanger, delay_min, delay_span, phase_inc, feedback,
                mix, self->invert);
            self->sample_remaining_buffer += (n * (self->base.bits_per_sample / 8));
            self->sample_buffer_length -= n;
        } else {
            int16_t conv_in[SYNTHIO_MAX_DUR * 2];
            int16_t conv_out[SYNTHIO_MAX_DUR * 2];
            int16_t *sample_src = (int16_t *)self->sample_remaining_buffer;
            int8_t *sample_hsrc = (int8_t *)self->sample_remaining_buffer;
            for (uint32_t i = 0; i < n; i++) {
                int32_t sample_word;
                if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                    sample_word = sample_src[i];
                    if (!self->base.samples_signed) {
                        sample_word = (int16_t)(sample_src[i] ^ 0x8000);
                    }
                } else if (self->base.samples_signed) {
                    sample_word = sample_hsrc[i];
                } else {
                    sample_word = (int8_t)(((uint8_t)sample_hsrc[i]) ^ 0x80);
                }
                conv_in[i] = (int16_t)sample_word;
            }
            audiodsp_flanger_process_s16(conv_out, conv_in, n, self->delay_buffer,
                self->delay_buffer_frames, self->base.channel_count,
                &self->flanger, delay_min, delay_span, phase_inc, feedback,
                mix, self->invert);
            for (uint32_t i = 0; i < n; i++) {
                int32_t word = conv_out[i];
                if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                    word_buffer[i] = (int16_t)word;
                    if (!self->base.samples_signed) {
                        word_buffer[i] ^= 0x8000;
                    }
                } else {
                    int8_t out = MIN(MAX(word, -128), 127);
                    if (self->base.samples_signed) {
                        hword_buffer[i] = out;
                    } else {
                        hword_buffer[i] = (uint8_t)out ^ 0x80;
                    }
                }
            }
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

// --- Python bindings (from shared-bindings/audiodelays/Flanger.c) ---------

static mp_obj_t audiodelays_flanger_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_max_delay_ms, ARG_min_delay_ms, ARG_rate, ARG_depth, ARG_feedback, ARG_mix, ARG_invert, ARG_buffer_size, ARG_sample_rate, ARG_bits_per_sample, ARG_samples_signed, ARG_channel_count, };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_max_delay_ms, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 10 } },
        { MP_QSTR_min_delay_ms, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_rate, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_depth, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_feedback, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_mix, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_invert, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
        { MP_QSTR_buffer_size, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 512} },
        { MP_QSTR_sample_rate, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 8000} },
        { MP_QSTR_bits_per_sample, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 16} },
        { MP_QSTR_samples_signed, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = true} },
        { MP_QSTR_channel_count, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1 } },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t max_delay_ms = mp_arg_validate_int_range(args[ARG_max_delay_ms].u_int, 1, 100, MP_QSTR_max_delay_ms);

    mp_int_t channel_count = mp_arg_validate_int_range(args[ARG_channel_count].u_int, 1, 2, MP_QSTR_channel_count);
    mp_int_t sample_rate = mp_arg_validate_int_min(args[ARG_sample_rate].u_int, 1, MP_QSTR_sample_rate);
    mp_int_t bits_per_sample = args[ARG_bits_per_sample].u_int;
    if (bits_per_sample != 8 && bits_per_sample != 16) {
        mp_raise_ValueError(MP_ERROR_TEXT("bits_per_sample must be 8 or 16"));
    }

    audiodelays_flanger_obj_t *self = mp_obj_malloc(audiodelays_flanger_obj_t, &audiodelays_flanger_type);
    common_hal_audiodelays_flanger_construct(self, max_delay_ms, args[ARG_min_delay_ms].u_obj, args[ARG_rate].u_obj, args[ARG_depth].u_obj, args[ARG_feedback].u_obj, args[ARG_mix].u_obj, args[ARG_invert].u_bool, args[ARG_buffer_size].u_int, bits_per_sample, args[ARG_samples_signed].u_bool, channel_count, sample_rate);

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audiodelays_flanger_deinit(mp_obj_t self_in) {
    audiodelays_flanger_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_flanger_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_flanger_deinit_obj, audiodelays_flanger_deinit);

static void check_for_deinit(audiodelays_flanger_obj_t *self) {
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

static mp_obj_t audiodelays_flanger_obj___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    common_hal_audiodelays_flanger_deinit(args[0]);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiodelays_flanger___exit___obj, 4, 4, audiodelays_flanger_obj___exit__);

static mp_obj_t audiodelays_flanger_obj_get_min_delay_ms(mp_obj_t self_in) {
    return common_hal_audiodelays_flanger_get_min_delay_ms(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_flanger_get_min_delay_ms_obj, audiodelays_flanger_obj_get_min_delay_ms);

static mp_obj_t audiodelays_flanger_obj_set_min_delay_ms(mp_obj_t self_in, mp_obj_t min_delay_ms_in) {
    audiodelays_flanger_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_flanger_set_min_delay_ms(self, min_delay_ms_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodelays_flanger_set_min_delay_ms_obj, audiodelays_flanger_obj_set_min_delay_ms);

MP_PROPERTY_GETSET(audiodelays_flanger_min_delay_ms_obj,
    (mp_obj_t)&audiodelays_flanger_get_min_delay_ms_obj,
    (mp_obj_t)&audiodelays_flanger_set_min_delay_ms_obj);

static mp_obj_t audiodelays_flanger_obj_get_rate(mp_obj_t self_in) {
    return common_hal_audiodelays_flanger_get_rate(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_flanger_get_rate_obj, audiodelays_flanger_obj_get_rate);

static mp_obj_t audiodelays_flanger_obj_set_rate(mp_obj_t self_in, mp_obj_t rate_in) {
    audiodelays_flanger_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_flanger_set_rate(self, rate_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodelays_flanger_set_rate_obj, audiodelays_flanger_obj_set_rate);

MP_PROPERTY_GETSET(audiodelays_flanger_rate_obj,
    (mp_obj_t)&audiodelays_flanger_get_rate_obj,
    (mp_obj_t)&audiodelays_flanger_set_rate_obj);

static mp_obj_t audiodelays_flanger_obj_get_depth(mp_obj_t self_in) {
    return common_hal_audiodelays_flanger_get_depth(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_flanger_get_depth_obj, audiodelays_flanger_obj_get_depth);

static mp_obj_t audiodelays_flanger_obj_set_depth(mp_obj_t self_in, mp_obj_t depth_in) {
    audiodelays_flanger_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_flanger_set_depth(self, depth_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodelays_flanger_set_depth_obj, audiodelays_flanger_obj_set_depth);

MP_PROPERTY_GETSET(audiodelays_flanger_depth_obj,
    (mp_obj_t)&audiodelays_flanger_get_depth_obj,
    (mp_obj_t)&audiodelays_flanger_set_depth_obj);

static mp_obj_t audiodelays_flanger_obj_get_feedback(mp_obj_t self_in) {
    return common_hal_audiodelays_flanger_get_feedback(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_flanger_get_feedback_obj, audiodelays_flanger_obj_get_feedback);

static mp_obj_t audiodelays_flanger_obj_set_feedback(mp_obj_t self_in, mp_obj_t feedback_in) {
    audiodelays_flanger_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_flanger_set_feedback(self, feedback_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodelays_flanger_set_feedback_obj, audiodelays_flanger_obj_set_feedback);

MP_PROPERTY_GETSET(audiodelays_flanger_feedback_obj,
    (mp_obj_t)&audiodelays_flanger_get_feedback_obj,
    (mp_obj_t)&audiodelays_flanger_set_feedback_obj);

static mp_obj_t audiodelays_flanger_obj_get_mix(mp_obj_t self_in) {
    return common_hal_audiodelays_flanger_get_mix(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_flanger_get_mix_obj, audiodelays_flanger_obj_get_mix);

static mp_obj_t audiodelays_flanger_obj_set_mix(mp_obj_t self_in, mp_obj_t mix_in) {
    audiodelays_flanger_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_flanger_set_mix(self, mix_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodelays_flanger_set_mix_obj, audiodelays_flanger_obj_set_mix);

MP_PROPERTY_GETSET(audiodelays_flanger_mix_obj,
    (mp_obj_t)&audiodelays_flanger_get_mix_obj,
    (mp_obj_t)&audiodelays_flanger_set_mix_obj);

static mp_obj_t audiodelays_flanger_obj_get_invert(mp_obj_t self_in) {
    audiodelays_flanger_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(common_hal_audiodelays_flanger_get_invert(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_flanger_get_invert_obj, audiodelays_flanger_obj_get_invert);

static mp_obj_t audiodelays_flanger_obj_set_invert(mp_obj_t self_in, mp_obj_t invert_in) {
    audiodelays_flanger_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_flanger_set_invert(self, mp_obj_is_true(invert_in));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodelays_flanger_set_invert_obj, audiodelays_flanger_obj_set_invert);

MP_PROPERTY_GETSET(audiodelays_flanger_invert_obj,
    (mp_obj_t)&audiodelays_flanger_get_invert_obj,
    (mp_obj_t)&audiodelays_flanger_set_invert_obj);

static mp_obj_t audiodelays_flanger_obj_get_lfo_value(mp_obj_t self_in) {
    audiodelays_flanger_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_float(common_hal_audiodelays_flanger_get_lfo_value(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_flanger_get_lfo_value_obj, audiodelays_flanger_obj_get_lfo_value);

MP_PROPERTY_GETTER(audiodelays_flanger_lfo_value_obj,
    (mp_obj_t)&audiodelays_flanger_get_lfo_value_obj);

static mp_obj_t audiodelays_flanger_obj_get_playing(mp_obj_t self_in) {
    audiodelays_flanger_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_bool(common_hal_audiodelays_flanger_get_playing(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_flanger_get_playing_obj, audiodelays_flanger_obj_get_playing);

MP_PROPERTY_GETTER(audiodelays_flanger_playing_obj,
    (mp_obj_t)&audiodelays_flanger_get_playing_obj);

static mp_obj_t audiodelays_flanger_obj_play(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_sample, ARG_loop };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sample,    MP_ARG_OBJ | MP_ARG_REQUIRED, {} },
        { MP_QSTR_loop,      MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
    };
    audiodelays_flanger_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    check_for_deinit(self);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_obj_t sample = args[ARG_sample].u_obj;
    common_hal_audiodelays_flanger_play(self, sample, args[ARG_loop].u_bool);

    return MP_OBJ_FROM_PTR(self);
}
MP_DEFINE_CONST_FUN_OBJ_KW(audiodelays_flanger_play_obj, 1, audiodelays_flanger_obj_play);

static mp_obj_t audiodelays_flanger_obj_stop(mp_obj_t self_in) {
    audiodelays_flanger_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_flanger_stop(self);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_flanger_stop_obj, audiodelays_flanger_obj_stop);

static const mp_rom_map_elem_t audiodelays_flanger_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiodelays_flanger_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&audiodelays_flanger___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiodelays_flanger_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audiodelays_flanger_stop_obj) },

    { MP_ROM_QSTR(MP_QSTR_playing), MP_ROM_PTR(&audiodelays_flanger_playing_obj) },
    { MP_ROM_QSTR(MP_QSTR_min_delay_ms), MP_ROM_PTR(&audiodelays_flanger_min_delay_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_rate), MP_ROM_PTR(&audiodelays_flanger_rate_obj) },
    { MP_ROM_QSTR(MP_QSTR_depth), MP_ROM_PTR(&audiodelays_flanger_depth_obj) },
    { MP_ROM_QSTR(MP_QSTR_feedback), MP_ROM_PTR(&audiodelays_flanger_feedback_obj) },
    { MP_ROM_QSTR(MP_QSTR_mix), MP_ROM_PTR(&audiodelays_flanger_mix_obj) },
    { MP_ROM_QSTR(MP_QSTR_invert), MP_ROM_PTR(&audiodelays_flanger_invert_obj) },
    { MP_ROM_QSTR(MP_QSTR_lfo_value), MP_ROM_PTR(&audiodelays_flanger_lfo_value_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiodelays_flanger_locals_dict, audiodelays_flanger_locals_dict_table);

// What is behind this node, for audiosample_find_type()'s walk.
// audiodsp#112: the pump refuses a file-backed source at handover,
// and the protocol had no way to look past the tail.
static mp_obj_t audiodelays_flanger_sources(mp_obj_t self_in, uint8_t index) {
    return index == 0 ? ((audiodelays_flanger_obj_t *)MP_OBJ_TO_PTR(self_in))->sample
                      : MP_OBJ_NULL;
}

static const audiosample_p_t audiodelays_flanger_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audiodelays_flanger_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audiodelays_flanger_get_buffer,
    .sources = audiodelays_flanger_sources,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiodelays_flanger_type,
    MP_QSTR_Flanger,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiodelays_flanger_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiodelays_flanger_locals_dict,
    protocol, &audiodelays_flanger_proto
    );
