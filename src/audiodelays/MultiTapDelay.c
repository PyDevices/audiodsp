// Ported from CircuitPython's shared-bindings+shared-module/audiodelays/
// MultiTapDelay.{h,c} (upstream repo: https://github.com/adafruit/circuitpython,
// MIT). No m_malloc fix needed here -- upstream already uses m_malloc_maybe
// (mainline has an equivalent, unlike m_malloc_without_collect used by the
// other effects). `attr, cp_compat_attr` added for delay_ms/decay/mix/
// taps/playing.
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Cooper Dalrymple
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include "audiodelays/MultiTapDelay.h"

#include <math.h>
#include <string.h>

#include "cp_compat/argcheck.h"
#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/objproperty.h"

#include "py/objtuple.h"
#include "py/runtime.h"
#include "shared/audiodsp_multitap.h"
#include "shared/audiodsp_pump_lock.h"

// --- shared-module (DSP engine) -------------------------------------------

void common_hal_audiodelays_multi_tap_delay_construct(audiodelays_multi_tap_delay_obj_t *self, uint32_t max_delay_ms,
    mp_obj_t delay_ms, mp_obj_t decay, mp_obj_t mix, mp_obj_t taps,
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
        common_hal_audiodelays_multi_tap_delay_deinit(self);
        m_malloc_fail(self->buffer_len);
    }
    memset(self->buffer[0], 0, self->buffer_len);

    self->buffer[1] = m_malloc_maybe(self->buffer_len);
    if (self->buffer[1] == NULL) {
        common_hal_audiodelays_multi_tap_delay_deinit(self);
        m_malloc_fail(self->buffer_len);
    }
    memset(self->buffer[1], 0, self->buffer_len);

    self->last_buf_idx = 1;

    self->sample = NULL;
    self->sample_remaining_buffer = NULL;
    self->sample_buffer_length = 0;
    self->loop = false;
    self->more_data = false;

    if (decay == MP_OBJ_NULL) {
        decay = mp_obj_new_float(MICROPY_FLOAT_CONST(0.7));
    }
    synthio_block_assign_slot(decay, &self->decay, MP_QSTR_decay);

    if (mix == MP_OBJ_NULL) {
        mix = mp_obj_new_float(MICROPY_FLOAT_CONST(0.25));
    }
    synthio_block_assign_slot(mix, &self->mix, MP_QSTR_mix);

    // Allocate the delay buffer for the max possible delay, delay is always 16-bit
    self->max_delay_ms = max_delay_ms;
    self->max_delay_buffer_len = (uint32_t)(self->base.sample_rate / MICROPY_FLOAT_CONST(1000.0) * max_delay_ms) * (self->base.channel_count * sizeof(uint16_t)); // bytes
    self->delay_buffer = m_malloc_maybe(self->max_delay_buffer_len);
    if (self->delay_buffer == NULL) {
        common_hal_audiodelays_multi_tap_delay_deinit(self);
        m_malloc_fail(self->max_delay_buffer_len);
    }
    memset(self->delay_buffer, 0, self->max_delay_buffer_len);

    self->sample_ms = MICROPY_FLOAT_CONST(1000.0) / self->base.sample_rate;

    common_hal_audiodelays_multi_tap_delay_set_delay_ms(self, delay_ms);
    self->delay_buffer_pos = 0;
    self->delay_buffer_right_pos = 0;

    self->tap_positions = NULL;
    self->tap_levels = NULL;
    self->tap_offsets = NULL;
    self->tap_len = 0;
    common_hal_audiodelays_multi_tap_delay_set_taps(self, taps);
}

void common_hal_audiodelays_multi_tap_delay_deinit(audiodelays_multi_tap_delay_obj_t *self) {
    // The whole body. mark_deinit is not the damage; the pointer
    // nulling AFTER it is -- the funnel's guard has already let a
    // pull in by then, and the pull writes into a buffer that has
    // just become NULL. Detach under the lock, free afterwards.
    audiodsp_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    self->delay_buffer = NULL;
    self->buffer[0] = NULL;
    self->buffer[1] = NULL;

    self->tap_positions = NULL;
    self->tap_levels = NULL;
    self->tap_offsets = NULL;
    audiodsp_pump_lock_release();
}

mp_float_t common_hal_audiodelays_multi_tap_delay_get_delay_ms(audiodelays_multi_tap_delay_obj_t *self) {
    return self->delay_ms;
}

void common_hal_audiodelays_multi_tap_delay_set_delay_ms(audiodelays_multi_tap_delay_obj_t *self, mp_obj_t delay_ms) {
    self->delay_ms = mp_obj_get_float(delay_ms);

    // Require that delay is at least 1 sample long
    self->delay_ms = MAX(self->delay_ms, self->sample_ms);

    self->delay_buffer_len = (uint32_t)(self->base.sample_rate / MICROPY_FLOAT_CONST(1000.0) * self->delay_ms) * (self->base.channel_count * sizeof(uint16_t));

    if (self->delay_buffer_len > self->max_delay_buffer_len) {
        self->delay_buffer_len = self->max_delay_buffer_len;
    } else if (self->delay_buffer_len < self->buffer_len) {
        // If the delay buffer is smaller than our audio buffer, weird things happen
        self->delay_buffer_len = self->buffer_len;
    }

    // Clear the now unused part of the buffer or some weird artifacts appear
    memset(self->delay_buffer + self->delay_buffer_len, 0, self->max_delay_buffer_len - self->delay_buffer_len);

    recalculate_tap_offsets(self);
}

mp_obj_t common_hal_audiodelays_multi_tap_delay_get_decay(audiodelays_multi_tap_delay_obj_t *self) {
    return self->decay.obj;
}

void common_hal_audiodelays_multi_tap_delay_set_decay(audiodelays_multi_tap_delay_obj_t *self, mp_obj_t decay) {
    synthio_block_assign_slot(decay, &self->decay, MP_QSTR_decay);
}

mp_obj_t common_hal_audiodelays_multi_tap_delay_get_mix(audiodelays_multi_tap_delay_obj_t *self) {
    return self->mix.obj;
}

void common_hal_audiodelays_multi_tap_delay_set_mix(audiodelays_multi_tap_delay_obj_t *self, mp_obj_t mix) {
    synthio_block_assign_slot(mix, &self->mix, MP_QSTR_mix);
}

mp_obj_t common_hal_audiodelays_multi_tap_delay_get_taps(audiodelays_multi_tap_delay_obj_t *self) {
    if (!self->tap_len) {
        return mp_const_none;
    } else {
        mp_obj_tuple_t *taps = MP_OBJ_TO_PTR(mp_obj_new_tuple(self->tap_len, NULL));
        for (size_t i = 0; i < self->tap_len; i++) {
            mp_obj_tuple_t *pair = MP_OBJ_TO_PTR(mp_obj_new_tuple(2, NULL));
            pair->items[0] = mp_obj_new_float(self->tap_positions[i]);
            pair->items[1] = mp_obj_new_float((mp_float_t)self->tap_levels[i]);
            taps->items[i] = MP_OBJ_FROM_PTR(pair);
        }
        return MP_OBJ_FROM_PTR(taps);
    }
}

void validate_tap_value(mp_obj_t item, qstr arg_name) {
    if (mp_obj_is_small_int(item)) {
        mp_arg_validate_int_range(mp_obj_get_int(item), 0, 1, arg_name);
    } else {
        mp_arg_validate_obj_float_range(item, 0, 1, arg_name);
    }
}

double get_tap_value(mp_obj_t item) {
    double value;
    if (mp_obj_is_small_int(item)) {
        value = (double)mp_obj_get_int(item);
    } else {
        value = (double)mp_obj_float_get(item);
    }
    return value;
}

void common_hal_audiodelays_multi_tap_delay_set_taps(audiodelays_multi_tap_delay_obj_t *self, mp_obj_t taps_in) {
    if (taps_in != mp_const_none && !MP_OBJ_TYPE_HAS_SLOT(mp_obj_get_type(taps_in), iter)) {
        mp_raise_TypeError_varg(
            MP_ERROR_TEXT("%q must be of type %q, not %q"),
            MP_QSTR_taps, MP_QSTR_iterable, mp_obj_get_type(taps_in)->name);
    }

    size_t len, i;
    mp_obj_t *items;

    if (taps_in == mp_const_none) {
        len = 0;
        items = NULL;
    } else {
        // convert object to tuple if it wasn't before
        taps_in = MP_OBJ_TYPE_GET_SLOT(&mp_type_tuple, make_new)(
            &mp_type_tuple, 1, 0, &taps_in);

        mp_obj_tuple_get(taps_in, &len, &items);
        mp_arg_validate_length_min(len, 1, MP_QSTR_items);

        for (i = 0; i < len; i++) {
            mp_obj_t item = items[i];
            if (mp_obj_is_tuple_compatible(item)) {
                size_t len1;
                mp_obj_t *items1;
                mp_obj_tuple_get(item, &len1, &items1);
                mp_arg_validate_length(len1, 2, MP_QSTR_items);

                for (size_t j = 0; j < len1; j++) {
                    validate_tap_value(items1[j], j ? MP_QSTR_level : MP_QSTR_position);
                }
            } else if (mp_obj_is_float(item) || mp_obj_is_small_int(item)) {
                validate_tap_value(item, MP_QSTR_position);
            } else {
                mp_raise_TypeError_varg(
                    MP_ERROR_TEXT("%q in %q must be of type %q or %q, not %q"),
                    MP_QSTR_object,
                    MP_QSTR_taps,
                    MP_QSTR_iterable,
                    MP_QSTR_float,
                    mp_obj_get_type(item)->name);
            }
        }
    }

    self->tap_positions = m_renew(mp_float_t,
        self->tap_positions,
        self->tap_len,
        len);
    self->tap_levels = m_renew(double,
        self->tap_levels,
        self->tap_len,
        len);
    self->tap_offsets = m_renew(uint32_t,
        self->tap_offsets,
        self->tap_len,
        len);
    self->tap_len = len;

    for (i = 0; i < len; i++) {
        mp_obj_t item = items[i];
        if (mp_obj_is_tuple_compatible(item)) {
            size_t len1;
            mp_obj_t *items1;
            mp_obj_tuple_get(item, &len1, &items1);

            self->tap_positions[i] = (mp_float_t)get_tap_value(items1[0]);
            self->tap_levels[i] = get_tap_value(items1[1]);
        } else {
            self->tap_positions[i] = (mp_float_t)get_tap_value(item);
            self->tap_levels[i] = MICROPY_FLOAT_CONST(1.0);
        }
    }

    recalculate_tap_offsets(self);
}

void recalculate_tap_offsets(audiodelays_multi_tap_delay_obj_t *self) {
    if (!self->tap_len) {
        return;
    }

    uint32_t delay_buffer_len = self->delay_buffer_len / self->base.channel_count / sizeof(uint16_t);
    for (size_t i = 0; i < self->tap_len; i++) {
        self->tap_offsets[i] = (uint32_t)(delay_buffer_len * self->tap_positions[i]);
    }
}

void audiodelays_multi_tap_delay_reset_buffer(audiodelays_multi_tap_delay_obj_t *self,
    bool single_channel_output,
    uint8_t channel) {
    (void)single_channel_output;
    (void)channel;

    memset(self->buffer[0], 0, self->buffer_len);
    memset(self->buffer[1], 0, self->buffer_len);
    memset(self->delay_buffer, 0, self->max_delay_buffer_len);
}

bool common_hal_audiodelays_multi_tap_delay_get_playing(audiodelays_multi_tap_delay_obj_t *self) {
    return self->sample != NULL;
}

void common_hal_audiodelays_multi_tap_delay_play(audiodelays_multi_tap_delay_obj_t *self, mp_obj_t sample, bool loop) {
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

void common_hal_audiodelays_multi_tap_delay_stop(audiodelays_multi_tap_delay_obj_t *self) {
    self->sample = NULL;
}

audioio_get_buffer_result_t audiodelays_multi_tap_delay_get_buffer(audiodelays_multi_tap_delay_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {

    if (!single_channel_output) {
        channel = 0;
    }

    self->last_buf_idx = !self->last_buf_idx;

    int16_t *word_buffer = (int16_t *)self->buffer[self->last_buf_idx];
    int8_t *hword_buffer = self->buffer[self->last_buf_idx];
    uint32_t length = self->buffer_len / (self->base.bits_per_sample / 8);

    int16_t *delay_buffer = (int16_t *)self->delay_buffer;
    uint32_t delay_buffer_len = self->delay_buffer_len / self->base.channel_count / sizeof(uint16_t);

    uint32_t delay_buffer_pos = self->delay_buffer_pos;
    if (single_channel_output && channel == 1) {
        delay_buffer_pos = self->delay_buffer_right_pos;
    }

    int32_t mix_down_scale = SYNTHIO_MIX_DOWN_SCALE(self->tap_len);

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
        mp_float_t mix = synthio_block_slot_get_limited(&self->mix, MICROPY_FLOAT_CONST(0.0), MICROPY_FLOAT_CONST(1.0)) * MICROPY_FLOAT_CONST(2.0);
        mp_float_t decay = synthio_block_slot_get_limited(&self->decay, MICROPY_FLOAT_CONST(0.0), MICROPY_FLOAT_CONST(1.0));

        int16_t *sample_src = NULL;
        int8_t *sample_hsrc = NULL;
        if (self->sample != NULL) {
            sample_src = (int16_t *)self->sample_remaining_buffer;
            sample_hsrc = (int8_t *)self->sample_remaining_buffer;
        }

        if (self->base.bits_per_sample == 16 && self->base.samples_signed &&
            !single_channel_output) {
            int16_t silence[SYNTHIO_MAX_DUR * 2] = {0};
            const int16_t *input = self->sample != NULL ? sample_src : silence;
            delay_buffer_pos = audiodsp_multitap_process_s16(
                word_buffer, input, n, delay_buffer, delay_buffer_pos,
                delay_buffer_len, self->base.channel_count,
                self->tap_offsets, self->tap_levels, self->tap_len,
                decay, mix);
            goto multitap_samples_done;
        }

        for (uint32_t i = 0; i < n; i++) {
            uint32_t delay_buffer_offset = delay_buffer_len * ((single_channel_output && channel == 1) || (!single_channel_output && (i % self->base.channel_count) == 1));

            int32_t sample_word = 0;
            if (self->sample != NULL) {
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
            }

            // Pull words from delay buffer at tap positions, apply level and mix down
            int32_t word = 0;
            int32_t delay_word;
            if (self->tap_len) {
                size_t tap_pos;
                for (size_t j = 0; j < self->tap_len; j++) {
                    tap_pos = (delay_buffer_pos + delay_buffer_len - self->tap_offsets[j]) % delay_buffer_len;
                    delay_word = delay_buffer[tap_pos + delay_buffer_offset];
                    word += (int32_t)(delay_word * self->tap_levels[j]);
                }

                if (self->tap_len > 1) {
                    word = synthio_mix_down_sample(word, mix_down_scale);
                }
            }

            delay_word = delay_buffer[delay_buffer_pos + delay_buffer_offset];

            // If no taps are provided, use as standard delay
            if (!self->tap_len) {
                word = delay_word;
            }

            delay_word = (int32_t)(delay_word * decay) + sample_word;

            if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                delay_word = synthio_mix_down_sample(delay_word, SYNTHIO_MIX_DOWN_SCALE(2));
                delay_buffer[delay_buffer_pos + delay_buffer_offset] = (int16_t)delay_word;
            } else {
                // Do not have mix_down for 8 bit so just hard cap samples into 1 byte
                delay_word = MIN(MAX(delay_word, -128), 127);
                delay_buffer[delay_buffer_pos + delay_buffer_offset] = (int8_t)delay_word;
            }

            word = (int32_t)((sample_word * MIN(MICROPY_FLOAT_CONST(2.0) - mix, MICROPY_FLOAT_CONST(1.0)))
                + (word * MIN(mix, MICROPY_FLOAT_CONST(1.0))));
            word = synthio_mix_down_sample(word, SYNTHIO_MIX_DOWN_SCALE(2));

            if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                word_buffer[i] = (int16_t)word;
                if (!self->base.samples_signed) {
                    word_buffer[i] ^= 0x8000;
                }
            } else {
                int8_t mixed = (int16_t)word;
                if (self->base.samples_signed) {
                    hword_buffer[i] = mixed;
                } else {
                    hword_buffer[i] = (uint8_t)mixed ^ 0x80;
                }
            }

            if ((self->base.channel_count == 1 || single_channel_output || (!single_channel_output && (i % self->base.channel_count) == 1))
                && ++delay_buffer_pos >= delay_buffer_len) {
                delay_buffer_pos = 0;
            }
        }

        multitap_samples_done:

        length -= n;
        word_buffer += n;
        hword_buffer += n;
        if (self->sample != NULL) {
            self->sample_remaining_buffer += (n * (self->base.bits_per_sample / 8));
            self->sample_buffer_length -= n;
        }
    }

    if (single_channel_output && channel == 1) {
        self->delay_buffer_right_pos = delay_buffer_pos;
    } else {
        self->delay_buffer_pos = delay_buffer_pos;
    }

    *buffer = (uint8_t *)self->buffer[self->last_buf_idx];
    *buffer_length = self->buffer_len;

    return GET_BUFFER_MORE_DATA;
}

// --- Python bindings (from shared-bindings/audiodelays/MultiTapDelay.c) --

static mp_obj_t audiodelays_multi_tap_delay_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_max_delay_ms, ARG_delay_ms, ARG_decay, ARG_mix, ARG_taps, ARG_buffer_size, ARG_sample_rate, ARG_bits_per_sample, ARG_samples_signed, ARG_channel_count, };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_max_delay_ms, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 500 } },
        { MP_QSTR_delay_ms, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(250) } },
        { MP_QSTR_decay, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_mix, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_taps, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = mp_const_none} },
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

    audiodelays_multi_tap_delay_obj_t *self = mp_obj_malloc(audiodelays_multi_tap_delay_obj_t, &audiodelays_multi_tap_delay_type);
    common_hal_audiodelays_multi_tap_delay_construct(self, max_delay_ms, args[ARG_delay_ms].u_obj, args[ARG_decay].u_obj, args[ARG_mix].u_obj, args[ARG_taps].u_obj, args[ARG_buffer_size].u_int, bits_per_sample, args[ARG_samples_signed].u_bool, channel_count, args[ARG_sample_rate].u_int);

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audiodelays_multi_tap_delay_deinit(mp_obj_t self_in) {
    audiodelays_multi_tap_delay_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_multi_tap_delay_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_multi_tap_delay_deinit_obj, audiodelays_multi_tap_delay_deinit);

static void check_for_deinit(audiodelays_multi_tap_delay_obj_t *self) {
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

static mp_obj_t audiodelays_multi_tap_delay_obj_get_delay_ms(mp_obj_t self_in) {
    audiodelays_multi_tap_delay_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_float(common_hal_audiodelays_multi_tap_delay_get_delay_ms(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_multi_tap_delay_get_delay_ms_obj, audiodelays_multi_tap_delay_obj_get_delay_ms);

static mp_obj_t audiodelays_multi_tap_delay_obj_set_delay_ms(mp_obj_t self_in, mp_obj_t delay_ms_in) {
    audiodelays_multi_tap_delay_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_multi_tap_delay_set_delay_ms(self, delay_ms_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodelays_multi_tap_delay_set_delay_ms_obj, audiodelays_multi_tap_delay_obj_set_delay_ms);

MP_PROPERTY_GETSET(audiodelays_multi_tap_delay_delay_ms_obj,
    (mp_obj_t)&audiodelays_multi_tap_delay_get_delay_ms_obj,
    (mp_obj_t)&audiodelays_multi_tap_delay_set_delay_ms_obj);

static mp_obj_t audiodelays_multi_tap_delay_obj_get_decay(mp_obj_t self_in) {
    return common_hal_audiodelays_multi_tap_delay_get_decay(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_multi_tap_delay_get_decay_obj, audiodelays_multi_tap_delay_obj_get_decay);

static mp_obj_t audiodelays_multi_tap_delay_obj_set_decay(mp_obj_t self_in, mp_obj_t decay_in) {
    audiodelays_multi_tap_delay_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_multi_tap_delay_set_decay(self, decay_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodelays_multi_tap_delay_set_decay_obj, audiodelays_multi_tap_delay_obj_set_decay);

MP_PROPERTY_GETSET(audiodelays_multi_tap_delay_decay_obj,
    (mp_obj_t)&audiodelays_multi_tap_delay_get_decay_obj,
    (mp_obj_t)&audiodelays_multi_tap_delay_set_decay_obj);

static mp_obj_t audiodelays_multi_tap_delay_obj_get_mix(mp_obj_t self_in) {
    return common_hal_audiodelays_multi_tap_delay_get_mix(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_multi_tap_delay_get_mix_obj, audiodelays_multi_tap_delay_obj_get_mix);

static mp_obj_t audiodelays_multi_tap_delay_obj_set_mix(mp_obj_t self_in, mp_obj_t mix_in) {
    audiodelays_multi_tap_delay_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_multi_tap_delay_set_mix(self, mix_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodelays_multi_tap_delay_set_mix_obj, audiodelays_multi_tap_delay_obj_set_mix);

MP_PROPERTY_GETSET(audiodelays_multi_tap_delay_mix_obj,
    (mp_obj_t)&audiodelays_multi_tap_delay_get_mix_obj,
    (mp_obj_t)&audiodelays_multi_tap_delay_set_mix_obj);

static mp_obj_t audiodelays_multi_tap_delay_obj_get_taps(mp_obj_t self_in) {
    return common_hal_audiodelays_multi_tap_delay_get_taps(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_multi_tap_delay_get_taps_obj, audiodelays_multi_tap_delay_obj_get_taps);

static mp_obj_t audiodelays_multi_tap_delay_obj_set_taps(mp_obj_t self_in, mp_obj_t taps_in) {
    audiodelays_multi_tap_delay_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_multi_tap_delay_set_taps(self, taps_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodelays_multi_tap_delay_set_taps_obj, audiodelays_multi_tap_delay_obj_set_taps);

MP_PROPERTY_GETSET(audiodelays_multi_tap_delay_taps_obj,
    (mp_obj_t)&audiodelays_multi_tap_delay_get_taps_obj,
    (mp_obj_t)&audiodelays_multi_tap_delay_set_taps_obj);

static mp_obj_t audiodelays_multi_tap_delay_obj_get_playing(mp_obj_t self_in) {
    audiodelays_multi_tap_delay_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_bool(common_hal_audiodelays_multi_tap_delay_get_playing(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_multi_tap_delay_get_playing_obj, audiodelays_multi_tap_delay_obj_get_playing);

MP_PROPERTY_GETTER(audiodelays_multi_tap_delay_playing_obj,
    (mp_obj_t)&audiodelays_multi_tap_delay_get_playing_obj);

static mp_obj_t audiodelays_multi_tap_delay_obj_play(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_sample, ARG_loop };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sample,    MP_ARG_OBJ | MP_ARG_REQUIRED, {} },
        { MP_QSTR_loop,      MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
    };
    audiodelays_multi_tap_delay_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    check_for_deinit(self);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_obj_t sample = args[ARG_sample].u_obj;
    common_hal_audiodelays_multi_tap_delay_play(self, sample, args[ARG_loop].u_bool);

    return MP_OBJ_FROM_PTR(self);
}
MP_DEFINE_CONST_FUN_OBJ_KW(audiodelays_multi_tap_delay_play_obj, 1, audiodelays_multi_tap_delay_obj_play);

static mp_obj_t audiodelays_multi_tap_delay_obj_stop(mp_obj_t self_in) {
    audiodelays_multi_tap_delay_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_multi_tap_delay_stop(self);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_multi_tap_delay_stop_obj, audiodelays_multi_tap_delay_obj_stop);

static const mp_rom_map_elem_t audiodelays_multi_tap_delay_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiodelays_multi_tap_delay_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiodelays_multi_tap_delay_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audiodelays_multi_tap_delay_stop_obj) },

    { MP_ROM_QSTR(MP_QSTR_playing), MP_ROM_PTR(&audiodelays_multi_tap_delay_playing_obj) },
    { MP_ROM_QSTR(MP_QSTR_delay_ms), MP_ROM_PTR(&audiodelays_multi_tap_delay_delay_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_decay), MP_ROM_PTR(&audiodelays_multi_tap_delay_decay_obj) },
    { MP_ROM_QSTR(MP_QSTR_mix), MP_ROM_PTR(&audiodelays_multi_tap_delay_mix_obj) },
    { MP_ROM_QSTR(MP_QSTR_taps), MP_ROM_PTR(&audiodelays_multi_tap_delay_taps_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiodelays_multi_tap_delay_locals_dict, audiodelays_multi_tap_delay_locals_dict_table);

static const audiosample_p_t audiodelays_multi_tap_delay_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audiodelays_multi_tap_delay_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audiodelays_multi_tap_delay_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiodelays_multi_tap_delay_type,
    MP_QSTR_MultiTapDelay,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiodelays_multi_tap_delay_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiodelays_multi_tap_delay_locals_dict,
    protocol, &audiodelays_multi_tap_delay_proto
    );
