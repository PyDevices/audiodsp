// Ported from CircuitPython's shared-bindings+shared-module/audiodelays/
// Echo.{h,c} (upstream repo: https://github.com/adafruit/circuitpython,
// MIT). Deviation: m_malloc_without_collect -> m_malloc (no mainline
// equivalent, see docs/upstream-diff.md). `attr, cp_compat_attr` added for
// delay_ms/decay/mix/freq_shift/playing. Kept verbatim: set_freq_shift
// truncates delay_ms to a uint32_t before passing it to
// recalculate_delay(mp_float_t) -- an upstream precision-losing round trip,
// not a port bug.
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Mark Komus, Cooper Dalrymple
// SPDX-FileCopyrightText: Copyright (c) 2024 Mark Komus
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include "audiodelays/Echo.h"

#include <math.h>
#include <string.h>

#include "cp_compat/argcheck.h"
#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/objproperty.h"
#include "shared/audioif_echo.h"

#include "py/runtime.h"
#include "shared/audioif_pump_lock.h"

// --- shared-module (DSP engine) -------------------------------------------

void common_hal_audiodelays_echo_construct(audiodelays_echo_obj_t *self, uint32_t max_delay_ms,
    mp_obj_t delay_ms, mp_obj_t decay, mp_obj_t filter, mp_obj_t mix,
    uint32_t buffer_size, uint8_t bits_per_sample,
    bool samples_signed, uint8_t channel_count, uint32_t sample_rate, bool freq_shift) {

    self->freq_shift = freq_shift;

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

    common_hal_audiodelays_echo_set_filter(self, filter);

    if (decay == MP_OBJ_NULL) {
        decay = mp_obj_new_float(MICROPY_FLOAT_CONST(0.7));
    }
    synthio_block_assign_slot(decay, &self->decay, MP_QSTR_decay);

    if (delay_ms == MP_OBJ_NULL) {
        delay_ms = mp_obj_new_float(MICROPY_FLOAT_CONST(250.0));
    }
    synthio_block_assign_slot(delay_ms, &self->delay_ms, MP_QSTR_delay_ms);

    if (mix == MP_OBJ_NULL) {
        mix = mp_obj_new_float(MICROPY_FLOAT_CONST(0.25));
    }
    synthio_block_assign_slot(mix, &self->mix, MP_QSTR_mix);

    // Allocate the echo buffer for the max possible delay, echo is always 16-bit
    self->max_delay_ms = max_delay_ms;
    self->max_echo_buffer_len = (uint32_t)(self->base.sample_rate / MICROPY_FLOAT_CONST(1000.0) * max_delay_ms) * (self->base.channel_count * sizeof(uint16_t)); // bytes
    self->echo_buffer = m_malloc(self->max_echo_buffer_len);
    memset(self->echo_buffer, 0, self->max_echo_buffer_len);

    self->sample_ms = MICROPY_FLOAT_CONST(1000.0) / self->base.sample_rate;

    mp_float_t f_delay_ms = synthio_block_slot_get(&self->delay_ms);
    recalculate_delay(self, f_delay_ms);

    // read is where we read previous echo from delay_ms ago to play back now
    // write is where the store the latest playing sample to echo back later
    self->echo_buffer_left_pos = 0;
    self->echo_buffer_right_pos = 0;
}

void common_hal_audiodelays_echo_deinit(audiodelays_echo_obj_t *self) {
    audiosample_mark_deinit(&self->base);
    audiofilters_deinit_filter_chain(&self->filter);
    self->echo_buffer = NULL;
    self->buffer[0] = NULL;
    self->buffer[1] = NULL;
}

mp_obj_t common_hal_audiodelays_echo_get_delay_ms(audiodelays_echo_obj_t *self) {
    return self->delay_ms.obj;
}

void common_hal_audiodelays_echo_set_delay_ms(audiodelays_echo_obj_t *self, mp_obj_t delay_ms) {
    synthio_block_assign_slot(delay_ms, &self->delay_ms, MP_QSTR_delay_ms);

    mp_float_t f_delay_ms = synthio_block_slot_get(&self->delay_ms);

    recalculate_delay(self, f_delay_ms);
}

void recalculate_delay(audiodelays_echo_obj_t *self, mp_float_t f_delay_ms) {
    // Require that delay is at least 1 sample long
    f_delay_ms = MAX(f_delay_ms, self->sample_ms);

    uint32_t max_echo_buffer_len = self->max_echo_buffer_len >> (self->base.channel_count - 1);

    if (self->freq_shift) {
        // Calculate the rate of iteration over the echo buffer with 8 sub-bits
        self->echo_buffer_rate = (uint32_t)MAX(self->max_delay_ms / f_delay_ms * MICROPY_FLOAT_CONST(256.0), MICROPY_FLOAT_CONST(1.0));
        // Only use half of the buffer per channel if stereo
        self->echo_buffer_len = max_echo_buffer_len;
    } else {
        uint32_t new_echo_buffer_len = (uint32_t)(self->base.sample_rate / MICROPY_FLOAT_CONST(1000.0) * f_delay_ms) * sizeof(uint16_t);

        if (new_echo_buffer_len > max_echo_buffer_len) {
            new_echo_buffer_len = max_echo_buffer_len;
        } else if (new_echo_buffer_len < self->buffer_len) {
            // If the echo buffer is smaller than our audio buffer, weird things happen
            new_echo_buffer_len = self->buffer_len;
        }

        self->echo_buffer_len = new_echo_buffer_len;

        // Clear the now unused part of the buffer or some weird artifacts appear
        for (uint32_t i = 0; i < self->base.channel_count; i++) {
            memset(self->echo_buffer + (i * max_echo_buffer_len) + self->echo_buffer_len, 0, max_echo_buffer_len - self->echo_buffer_len);
        }
    }

    self->current_delay_ms = f_delay_ms;
}

mp_obj_t common_hal_audiodelays_echo_get_decay(audiodelays_echo_obj_t *self) {
    return self->decay.obj;
}

void common_hal_audiodelays_echo_set_decay(audiodelays_echo_obj_t *self, mp_obj_t decay) {
    synthio_block_assign_slot(decay, &self->decay, MP_QSTR_decay);
}

mp_obj_t common_hal_audiodelays_echo_get_filter(audiodelays_echo_obj_t *self) {
    return self->filter.obj;
}

void common_hal_audiodelays_echo_set_filter(audiodelays_echo_obj_t *self, mp_obj_t filter_in) {
    audiofilters_assign_filter_chain(&self->filter, filter_in, self->base.channel_count);
}

mp_obj_t common_hal_audiodelays_echo_get_mix(audiodelays_echo_obj_t *self) {
    return self->mix.obj;
}

void common_hal_audiodelays_echo_set_mix(audiodelays_echo_obj_t *self, mp_obj_t arg) {
    synthio_block_assign_slot(arg, &self->mix, MP_QSTR_mix);
}

bool common_hal_audiodelays_echo_get_freq_shift(audiodelays_echo_obj_t *self) {
    return self->freq_shift;
}

void common_hal_audiodelays_echo_set_freq_shift(audiodelays_echo_obj_t *self, bool freq_shift) {
    // Clear the echo buffer and reset buffer position if changing freq_shift modes
    if (self->freq_shift != freq_shift) {
        memset(self->echo_buffer, 0, self->max_echo_buffer_len);
        self->echo_buffer_left_pos = 0;
        self->echo_buffer_right_pos = 0;
    }
    self->freq_shift = freq_shift;
    uint32_t delay_ms = (uint32_t)synthio_block_slot_get(&self->delay_ms);
    recalculate_delay(self, delay_ms);
}

void audiodelays_echo_reset_buffer(audiodelays_echo_obj_t *self,
    bool single_channel_output,
    uint8_t channel) {
    (void)single_channel_output;
    (void)channel;

    memset(self->buffer[0], 0, self->buffer_len);
    memset(self->buffer[1], 0, self->buffer_len);
    memset(self->echo_buffer, 0, self->max_echo_buffer_len);

    audiofilters_reset_filter_chain(&self->filter, self->base.channel_count);
}

bool common_hal_audiodelays_echo_get_playing(audiodelays_echo_obj_t *self) {
    return self->sample != NULL;
}

void common_hal_audiodelays_echo_play(audiodelays_echo_obj_t *self, mp_obj_t sample, bool loop) {
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

    audioif_pump_lock_acquire();
    self->sample = sample;
    self->loop = loop;
    self->sample_remaining_buffer = (void *)primed;
    self->sample_buffer_length = primed_length;
    self->more_data = result == GET_BUFFER_MORE_DATA;
    audioif_pump_lock_release();
}

void common_hal_audiodelays_echo_stop(audiodelays_echo_obj_t *self) {
    self->sample = NULL;
}

audioio_get_buffer_result_t audiodelays_echo_get_buffer(audiodelays_echo_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {

    if (!single_channel_output) {
        channel = 0;
    }

    self->last_buf_idx = !self->last_buf_idx;

    int16_t *word_buffer = (int16_t *)self->buffer[self->last_buf_idx];
    int8_t *hword_buffer = self->buffer[self->last_buf_idx];
    uint32_t length = self->buffer_len / (self->base.bits_per_sample / 8);

    int16_t *echo_buffer = (int16_t *)self->echo_buffer;

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

        mp_float_t f_delay_ms = synthio_block_slot_get(&self->delay_ms);
        if (MICROPY_FLOAT_C_FUN(fabs)(self->current_delay_ms - f_delay_ms) >= self->sample_ms) {
            recalculate_delay(self, f_delay_ms);
        }

        audiofilters_tick_filter_chain(&self->filter);

        uint32_t echo_buf_len = self->echo_buffer_len / sizeof(uint16_t);
        uint32_t max_echo_buf_len = (self->max_echo_buffer_len >> (self->base.channel_count - 1)) / sizeof(uint16_t);

        if (self->sample == NULL) {
            if (mix <= MICROPY_FLOAT_CONST(0.01)) { // Mix of 0 is pure sample sound. We have no sample so no sound
                if (self->base.samples_signed) {
                    memset(word_buffer, 0, length * (self->base.bits_per_sample / 8));
                } else {
                    // For unsigned samples set to the middle which is "quiet"
                    if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                        uint16_t *uword_buffer = (uint16_t *)word_buffer;
                        while (length--) {
                            *uword_buffer++ = 32768;
                        }
                    } else {
                        memset(hword_buffer, 128, length * (self->base.bits_per_sample / 8));
                    }
                }
            } else {
                // Since we have no sample we can just iterate over the our entire remaining buffer and finish
                for (uint32_t i = 0; i < length; i++) {
                    int16_t echo, word = 0;
                    uint32_t next_buffer_pos = 0;

                    uint32_t echo_buffer_offset = max_echo_buf_len * ((single_channel_output && channel == 1) || (!single_channel_output && (i % self->base.channel_count) == 1));
                    uint32_t echo_buffer_pos = echo_buffer_offset ? self->echo_buffer_right_pos : self->echo_buffer_left_pos;

                    if (self->freq_shift) {
                        echo = echo_buffer[(echo_buffer_pos >> 8) + echo_buffer_offset];
                        next_buffer_pos = echo_buffer_pos + self->echo_buffer_rate;

                        for (uint32_t j = echo_buffer_pos >> 8; j < next_buffer_pos >> 8; j++) {
                            word = (int16_t)(echo_buffer[(j % echo_buf_len) + echo_buffer_offset] * decay);
                            echo_buffer[(j % echo_buf_len) + echo_buffer_offset] = (int16_t)audiofilters_process_filter_chain(
                                &self->filter, self->base.channel_count, !!echo_buffer_offset, word);
                        }
                    } else {
                        echo = echo_buffer[echo_buffer_pos + echo_buffer_offset];
                        word = (int16_t)(echo * decay);
                        echo_buffer[echo_buffer_pos++ + echo_buffer_offset] = (int16_t)audiofilters_process_filter_chain(
                            &self->filter, self->base.channel_count, !!echo_buffer_offset, word);
                    }

                    word = (int16_t)(echo * MIN(mix, MICROPY_FLOAT_CONST(1.0)));

                    if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                        word_buffer[i] = word;
                        if (!self->base.samples_signed) {
                            word_buffer[i] ^= 0x8000;
                        }
                    } else {
                        hword_buffer[i] = (int8_t)word;
                        if (!self->base.samples_signed) {
                            hword_buffer[i] ^= 0x80;
                        }
                    }

                    if (self->freq_shift) {
                        echo_buffer_pos = next_buffer_pos % (echo_buf_len << 8);
                    } else if (!self->freq_shift && echo_buffer_pos >= echo_buf_len) {
                        echo_buffer_pos = 0;
                    }

                    if (echo_buffer_offset) {
                        self->echo_buffer_right_pos = echo_buffer_pos;
                    } else {
                        self->echo_buffer_left_pos = echo_buffer_pos;
                    }
                }
            }

            length = 0;
        } else {
            int16_t *sample_src = (int16_t *)self->sample_remaining_buffer;
            int8_t *sample_hsrc = (int8_t *)self->sample_remaining_buffer;

            if (mix <= MICROPY_FLOAT_CONST(0.01)) {
                for (uint32_t i = 0; i < n; i++) {
                    if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                        word_buffer[i] = sample_src[i];
                    } else {
                        hword_buffer[i] = sample_hsrc[i];
                    }
                }
            } else {
                if (self->base.bits_per_sample == 16 && self->base.samples_signed &&
                    !single_channel_output && self->filter.objs_len == 0) {
                    audioif_echo_positions_t positions = {
                        self->echo_buffer_left_pos, self->echo_buffer_right_pos};
                    audioif_echo_process_s16(word_buffer, sample_src, n,
                        echo_buffer, echo_buf_len, max_echo_buf_len,
                        self->echo_buffer_rate, decay, mix, self->freq_shift,
                        self->base.channel_count, &positions);
                    self->echo_buffer_left_pos = positions.left_position;
                    self->echo_buffer_right_pos = positions.right_position;
                    goto echo_samples_done;
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

                    int32_t echo, word = 0;
                    uint32_t next_buffer_pos = 0;

                    uint32_t echo_buffer_offset = max_echo_buf_len * ((single_channel_output && channel == 1) || (!single_channel_output && (i % self->base.channel_count) == 1));
                    uint32_t echo_buffer_pos = echo_buffer_offset ? self->echo_buffer_right_pos : self->echo_buffer_left_pos;

                    if (self->freq_shift) {
                        echo = echo_buffer[(echo_buffer_pos >> 8) + echo_buffer_offset];
                        next_buffer_pos = echo_buffer_pos + self->echo_buffer_rate;
                    } else {
                        echo = echo_buffer[echo_buffer_pos + echo_buffer_offset];
                        word = (int32_t)(echo * decay + sample_word);
                    }

                    if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                        if (self->freq_shift) {
                            for (uint32_t j = echo_buffer_pos >> 8; j < next_buffer_pos >> 8; j++) {
                                word = (int32_t)(echo_buffer[(j % echo_buf_len) + echo_buffer_offset] * decay + sample_word);
                                word = synthio_mix_down_sample(word, SYNTHIO_MIX_DOWN_SCALE(2));
                                echo_buffer[(j % echo_buf_len) + echo_buffer_offset] = (int16_t)audiofilters_process_filter_chain(
                                    &self->filter, self->base.channel_count, !!echo_buffer_offset, word);
                            }
                        } else {
                            word = synthio_mix_down_sample(word, SYNTHIO_MIX_DOWN_SCALE(2));
                            echo_buffer[echo_buffer_pos++ + echo_buffer_offset] = (int16_t)audiofilters_process_filter_chain(
                                &self->filter, self->base.channel_count, !!echo_buffer_offset, word);
                        }
                    } else {
                        if (self->freq_shift) {
                            for (uint32_t j = echo_buffer_pos >> 8; j < next_buffer_pos >> 8; j++) {
                                word = (int32_t)(echo_buffer[(j % echo_buf_len) + echo_buffer_offset] * decay + sample_word);
                                // Do not have mix_down for 8 bit so just hard cap samples into 1 byte
                                word = MIN(MAX(word, -128), 127);
                                echo_buffer[(j % echo_buf_len) + echo_buffer_offset] = (int8_t)audiofilters_process_filter_chain(
                                    &self->filter, self->base.channel_count, !!echo_buffer_offset, word);
                            }
                        } else {
                            word = MIN(MAX(word, -128), 127);
                            echo_buffer[echo_buffer_pos++ + echo_buffer_offset] = (int8_t)audiofilters_process_filter_chain(
                                &self->filter, self->base.channel_count, !!echo_buffer_offset, word);
                        }
                    }

                    word = (int32_t)((sample_word * MIN(MICROPY_FLOAT_CONST(2.0) - mix, MICROPY_FLOAT_CONST(1.0)))
                        + (echo * MIN(mix, MICROPY_FLOAT_CONST(1.0))));
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

                    if (self->freq_shift) {
                        echo_buffer_pos = next_buffer_pos % (echo_buf_len << 8);
                    } else if (!self->freq_shift && echo_buffer_pos >= echo_buf_len) {
                        echo_buffer_pos = 0;
                    }

                    if (echo_buffer_offset) {
                        self->echo_buffer_right_pos = echo_buffer_pos;
                    } else {
                        self->echo_buffer_left_pos = echo_buffer_pos;
                    }
                }
            }

echo_samples_done:
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

// --- Python bindings (from shared-bindings/audiodelays/Echo.c) -----------

static mp_obj_t audiodelays_echo_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_max_delay_ms, ARG_delay_ms, ARG_decay, ARG_filter, ARG_mix, ARG_buffer_size, ARG_sample_rate, ARG_bits_per_sample, ARG_samples_signed, ARG_channel_count, ARG_freq_shift, };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_max_delay_ms, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 500 } },
        { MP_QSTR_delay_ms, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_decay, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_filter, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_NONE } },
        { MP_QSTR_mix, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_buffer_size, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 512} },
        { MP_QSTR_sample_rate, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 8000} },
        { MP_QSTR_bits_per_sample, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 16} },
        { MP_QSTR_samples_signed, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = true} },
        { MP_QSTR_channel_count, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1 } },
        { MP_QSTR_freq_shift, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = true } },
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

    audiodelays_echo_obj_t *self = mp_obj_malloc(audiodelays_echo_obj_t, &audiodelays_echo_type);
    common_hal_audiodelays_echo_construct(self, max_delay_ms, args[ARG_delay_ms].u_obj, args[ARG_decay].u_obj, args[ARG_filter].u_obj, args[ARG_mix].u_obj, args[ARG_buffer_size].u_int, bits_per_sample, args[ARG_samples_signed].u_bool, channel_count, args[ARG_sample_rate].u_int, args[ARG_freq_shift].u_bool);

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audiodelays_echo_deinit(mp_obj_t self_in) {
    audiodelays_echo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_echo_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_echo_deinit_obj, audiodelays_echo_deinit);

static void check_for_deinit(audiodelays_echo_obj_t *self) {
    audiosample_check_for_deinit(&self->base);
}

static mp_obj_t audiodelays_echo_obj_get_delay_ms(mp_obj_t self_in) {
    return common_hal_audiodelays_echo_get_delay_ms(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_echo_get_delay_ms_obj, audiodelays_echo_obj_get_delay_ms);

static mp_obj_t audiodelays_echo_obj_set_delay_ms(mp_obj_t self_in, mp_obj_t delay_ms_in) {
    audiodelays_echo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_echo_set_delay_ms(self, delay_ms_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodelays_echo_set_delay_ms_obj, audiodelays_echo_obj_set_delay_ms);

MP_PROPERTY_GETSET(audiodelays_echo_delay_ms_obj,
    (mp_obj_t)&audiodelays_echo_get_delay_ms_obj,
    (mp_obj_t)&audiodelays_echo_set_delay_ms_obj);

static mp_obj_t audiodelays_echo_obj_get_decay(mp_obj_t self_in) {
    return common_hal_audiodelays_echo_get_decay(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_echo_get_decay_obj, audiodelays_echo_obj_get_decay);

static mp_obj_t audiodelays_echo_obj_set_decay(mp_obj_t self_in, mp_obj_t decay_in) {
    audiodelays_echo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_echo_set_decay(self, decay_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodelays_echo_set_decay_obj, audiodelays_echo_obj_set_decay);

MP_PROPERTY_GETSET(audiodelays_echo_decay_obj,
    (mp_obj_t)&audiodelays_echo_get_decay_obj,
    (mp_obj_t)&audiodelays_echo_set_decay_obj);

static mp_obj_t audiodelays_echo_obj_get_filter(mp_obj_t self_in) {
    return common_hal_audiodelays_echo_get_filter(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_echo_get_filter_obj, audiodelays_echo_obj_get_filter);

static mp_obj_t audiodelays_echo_obj_set_filter(mp_obj_t self_in, mp_obj_t filter_in) {
    audiodelays_echo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_echo_set_filter(self, filter_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodelays_echo_set_filter_obj, audiodelays_echo_obj_set_filter);

MP_PROPERTY_GETSET(audiodelays_echo_filter_obj,
    (mp_obj_t)&audiodelays_echo_get_filter_obj,
    (mp_obj_t)&audiodelays_echo_set_filter_obj);

static mp_obj_t audiodelays_echo_obj_get_mix(mp_obj_t self_in) {
    return common_hal_audiodelays_echo_get_mix(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_echo_get_mix_obj, audiodelays_echo_obj_get_mix);

static mp_obj_t audiodelays_echo_obj_set_mix(mp_obj_t self_in, mp_obj_t mix_in) {
    audiodelays_echo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_echo_set_mix(self, mix_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodelays_echo_set_mix_obj, audiodelays_echo_obj_set_mix);

MP_PROPERTY_GETSET(audiodelays_echo_mix_obj,
    (mp_obj_t)&audiodelays_echo_get_mix_obj,
    (mp_obj_t)&audiodelays_echo_set_mix_obj);

static mp_obj_t audiodelays_echo_obj_get_freq_shift(mp_obj_t self_in) {
    return mp_obj_new_bool(common_hal_audiodelays_echo_get_freq_shift(self_in));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_echo_get_freq_shift_obj, audiodelays_echo_obj_get_freq_shift);

static mp_obj_t audiodelays_echo_obj_set_freq_shift(mp_obj_t self_in, mp_obj_t freq_shift_in) {
    audiodelays_echo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_echo_set_freq_shift(self, mp_obj_is_true(freq_shift_in));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodelays_echo_set_freq_shift_obj, audiodelays_echo_obj_set_freq_shift);

MP_PROPERTY_GETSET(audiodelays_echo_freq_shift_obj,
    (mp_obj_t)&audiodelays_echo_get_freq_shift_obj,
    (mp_obj_t)&audiodelays_echo_set_freq_shift_obj);

static mp_obj_t audiodelays_echo_obj_get_playing(mp_obj_t self_in) {
    audiodelays_echo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_bool(common_hal_audiodelays_echo_get_playing(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_echo_get_playing_obj, audiodelays_echo_obj_get_playing);

MP_PROPERTY_GETTER(audiodelays_echo_playing_obj,
    (mp_obj_t)&audiodelays_echo_get_playing_obj);

static mp_obj_t audiodelays_echo_obj_play(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_sample, ARG_loop };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sample,    MP_ARG_OBJ | MP_ARG_REQUIRED, {} },
        { MP_QSTR_loop,      MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
    };
    audiodelays_echo_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    check_for_deinit(self);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_obj_t sample = args[ARG_sample].u_obj;
    common_hal_audiodelays_echo_play(self, sample, args[ARG_loop].u_bool);

    return MP_OBJ_FROM_PTR(self);
}
MP_DEFINE_CONST_FUN_OBJ_KW(audiodelays_echo_play_obj, 1, audiodelays_echo_obj_play);

static mp_obj_t audiodelays_echo_obj_stop(mp_obj_t self_in) {
    audiodelays_echo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiodelays_echo_stop(self);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodelays_echo_stop_obj, audiodelays_echo_obj_stop);

static const mp_rom_map_elem_t audiodelays_echo_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiodelays_echo_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiodelays_echo_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audiodelays_echo_stop_obj) },

    { MP_ROM_QSTR(MP_QSTR_playing), MP_ROM_PTR(&audiodelays_echo_playing_obj) },
    { MP_ROM_QSTR(MP_QSTR_delay_ms), MP_ROM_PTR(&audiodelays_echo_delay_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_decay), MP_ROM_PTR(&audiodelays_echo_decay_obj) },
    { MP_ROM_QSTR(MP_QSTR_filter), MP_ROM_PTR(&audiodelays_echo_filter_obj) },
    { MP_ROM_QSTR(MP_QSTR_mix), MP_ROM_PTR(&audiodelays_echo_mix_obj) },
    { MP_ROM_QSTR(MP_QSTR_freq_shift), MP_ROM_PTR(&audiodelays_echo_freq_shift_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiodelays_echo_locals_dict, audiodelays_echo_locals_dict_table);

static const audiosample_p_t audiodelays_echo_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audiodelays_echo_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audiodelays_echo_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiodelays_echo_type,
    MP_QSTR_Echo,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiodelays_echo_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiodelays_echo_locals_dict,
    protocol, &audiodelays_echo_proto
    );
