// Ported from CircuitPython's shared-bindings+shared-module/audiofilters/
// Filter.{h,c} (upstream repo: https://github.com/adafruit/circuitpython,
// MIT). Deviation: m_malloc_without_collect -> m_malloc (no mainline
// equivalent, same fix as every other tier -- see docs/upstream-diff.md).
// Type registration adds `attr, cp_compat_attr` for filter/mix/playing
// (see docs/upstream-diff.md, "Property invocation needs an explicit attr
// slot").
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Cooper Dalrymple
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include "audiofilters/Filter.h"

#include <string.h>

#include "cp_compat/argcheck.h"
#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/objproperty.h"

#include "py/objtuple.h"
#include "py/runtime.h"
#include "shared/audioif_pump_lock.h"

// --- shared-module (DSP engine) -------------------------------------------

void common_hal_audiofilters_filter_construct(audiofilters_filter_obj_t *self,
    mp_obj_t filter, mp_obj_t mix,
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

    self->filter_buffer = m_malloc(SYNTHIO_MAX_DUR * sizeof(int32_t));
    memset(self->filter_buffer, 0, SYNTHIO_MAX_DUR * sizeof(int32_t));

    self->sample = NULL;
    self->sample_remaining_buffer = NULL;
    self->sample_buffer_length = 0;
    self->loop = false;
    self->more_data = false;

    common_hal_audiofilters_filter_set_filter(self, filter);

    synthio_block_assign_slot(mix, &self->mix, MP_QSTR_mix);
}

void common_hal_audiofilters_filter_deinit(audiofilters_filter_obj_t *self) {
    // The whole body. mark_deinit is not the damage; the pointer
    // nulling AFTER it is -- the funnel's guard has already let a
    // pull in by then, and the pull writes into a buffer that has
    // just become NULL. Detach under the lock, free afterwards.
    audioif_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    self->buffer[0] = NULL;
    self->buffer[1] = NULL;
    audiofilters_deinit_filter_chain(&self->filter);
    self->filter_buffer = NULL;
    audioif_pump_lock_release();
}

void common_hal_audiofilters_filter_set_filter(audiofilters_filter_obj_t *self, mp_obj_t filter_in) {
    audiofilters_assign_filter_chain(&self->filter, filter_in, self->base.channel_count);
}

mp_obj_t common_hal_audiofilters_filter_get_filter(audiofilters_filter_obj_t *self) {
    return self->filter.obj;
}

mp_obj_t common_hal_audiofilters_filter_get_mix(audiofilters_filter_obj_t *self) {
    return self->mix.obj;
}

void common_hal_audiofilters_filter_set_mix(audiofilters_filter_obj_t *self, mp_obj_t arg) {
    synthio_block_assign_slot(arg, &self->mix, MP_QSTR_mix);
}

void audiofilters_filter_reset_buffer(audiofilters_filter_obj_t *self,
    bool single_channel_output,
    uint8_t channel) {
    (void)single_channel_output;
    (void)channel;

    memset(self->buffer[0], 0, self->buffer_len);
    memset(self->buffer[1], 0, self->buffer_len);
    memset(self->filter_buffer, 0, SYNTHIO_MAX_DUR * sizeof(int32_t));

    audiofilters_reset_filter_chain(&self->filter, self->base.channel_count);
}

bool common_hal_audiofilters_filter_get_playing(audiofilters_filter_obj_t *self) {
    return self->sample != NULL;
}

void common_hal_audiofilters_filter_play(audiofilters_filter_obj_t *self, mp_obj_t sample, bool loop) {
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

void common_hal_audiofilters_filter_stop(audiofilters_filter_obj_t *self) {
    self->sample = NULL;
}

audioio_get_buffer_result_t audiofilters_filter_get_buffer(audiofilters_filter_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    (void)channel;

    if (!single_channel_output) {
        channel = 0;
    }

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
            // tick all block inputs
            shared_bindings_synthio_lfo_tick(self->base.sample_rate, length / self->base.channel_count);
            (void)synthio_block_slot_get(&self->mix);

            audiofilters_tick_filter_chain(&self->filter);
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

            length = 0;
        } else {
            uint32_t n = MIN(MIN(self->sample_buffer_length, length), (uint32_t)(SYNTHIO_MAX_DUR * self->base.channel_count));

            int16_t *sample_src = (int16_t *)self->sample_remaining_buffer;
            int8_t *sample_hsrc = (int8_t *)self->sample_remaining_buffer;

            shared_bindings_synthio_lfo_tick(self->base.sample_rate, n / self->base.channel_count);
            mp_float_t mix = synthio_block_slot_get_limited(&self->mix, MICROPY_FLOAT_CONST(0.0), MICROPY_FLOAT_CONST(1.0));

            if (mix <= MICROPY_FLOAT_CONST(0.01) || !self->filter.objs_len) {
                for (uint32_t i = 0; i < n; i++) {
                    if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                        word_buffer[i] = sample_src[i];
                    } else {
                        hword_buffer[i] = sample_hsrc[i];
                    }
                }
            } else {
                // Deinterleave a chunk of frames, filter each channel through
                // the cascade with that channel's own states, and write it
                // back. Chunking in frames (not samples) keeps every channel
                // advancing in lockstep across chunk boundaries.
                const uint8_t channels = self->base.channel_count;
                audiofilters_tick_filter_chain(&self->filter);
                uint32_t i = 0;
                while (i < n) {
                    uint32_t frames = MIN((uint32_t)SYNTHIO_MAX_DUR, (n - i) / channels);
                    if (frames == 0) {
                        // A partial frame can only appear if the source handed
                        // us a fragment; copy it through rather than spin.
                        for (uint32_t j = i; j < n; j++) {
                            if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                                word_buffer[j] = sample_src[j];
                            } else {
                                hword_buffer[j] = sample_hsrc[j];
                            }
                        }
                        break;
                    }

                    for (uint8_t c = 0; c < channels; c++) {
                        for (uint32_t k = 0; k < frames; k++) {
                            uint32_t idx = i + k * channels + c;
                            if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                                self->filter_buffer[k] = sample_src[idx];
                            } else {
                                if (self->base.samples_signed) {
                                    self->filter_buffer[k] = sample_hsrc[idx];
                                } else {
                                    // Careful: 8-bit unsigned -> 32-bit signed
                                    self->filter_buffer[k] = (int8_t)(((uint8_t)sample_hsrc[idx]) ^ 0x80);
                                }
                            }
                        }

                        for (uint32_t k = 0; k < frames; k++) {
                            self->filter_buffer[k] = audiofilters_process_filter_chain(
                                &self->filter, channels, c, self->filter_buffer[k]);
                        }

                        for (uint32_t k = 0; k < frames; k++) {
                            uint32_t idx = i + k * channels + c;
                            if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                                word_buffer[idx] = synthio_mix_down_sample((int32_t)((sample_src[idx] * (MICROPY_FLOAT_CONST(1.0) - mix)) + (self->filter_buffer[k] * mix)), SYNTHIO_MIX_DOWN_SCALE(2));
                                if (!self->base.samples_signed) {
                                    word_buffer[idx] ^= 0x8000;
                                }
                            } else {
                                if (self->base.samples_signed) {
                                    hword_buffer[idx] = (int8_t)((sample_hsrc[idx] * (MICROPY_FLOAT_CONST(1.0) - mix)) + (self->filter_buffer[k] * mix));
                                } else {
                                    hword_buffer[idx] = (uint8_t)(((int8_t)(((uint8_t)sample_hsrc[idx]) ^ 0x80) * (MICROPY_FLOAT_CONST(1.0) - mix)) + (self->filter_buffer[k] * mix)) ^ 0x80;
                                }
                            }
                        }
                    }

                    i += frames * channels;
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

// --- Python bindings (from shared-bindings/audiofilters/Filter.c) --------

static mp_obj_t audiofilters_filter_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_filter, ARG_mix, ARG_buffer_size, ARG_sample_rate, ARG_bits_per_sample, ARG_samples_signed, ARG_channel_count, };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_filter, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_NONE } },
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

    audiofilters_filter_obj_t *self = mp_obj_malloc(audiofilters_filter_obj_t, &audiofilters_filter_type);
    common_hal_audiofilters_filter_construct(self,
        args[ARG_filter].u_obj,
        args[ARG_mix].u_obj,
        args[ARG_buffer_size].u_int,
        bits_per_sample,
        args[ARG_samples_signed].u_bool,
        channel_count,
        args[ARG_sample_rate].u_int);

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audiofilters_filter_deinit(mp_obj_t self_in) {
    audiofilters_filter_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiofilters_filter_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofilters_filter_deinit_obj, audiofilters_filter_deinit);

static void check_for_deinit(audiofilters_filter_obj_t *self) {
    // The whole body. mark_deinit is not the damage; the pointer
    // nulling AFTER it is -- the funnel's guard has already let a
    // pull in by then, and the pull writes into a buffer that has
    // just become NULL. Detach under the lock, free afterwards.
    audioif_pump_lock_acquire();
    audiosample_check_for_deinit(&self->base);
    audioif_pump_lock_release();
}

static mp_obj_t audiofilters_filter_obj_get_filter(mp_obj_t self_in) {
    audiofilters_filter_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return common_hal_audiofilters_filter_get_filter(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiofilters_filter_get_filter_obj, audiofilters_filter_obj_get_filter);

static mp_obj_t audiofilters_filter_obj_set_filter(mp_obj_t self_in, mp_obj_t filter_in) {
    audiofilters_filter_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiofilters_filter_set_filter(self, filter_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiofilters_filter_set_filter_obj, audiofilters_filter_obj_set_filter);

MP_PROPERTY_GETSET(audiofilters_filter_filter_obj,
    (mp_obj_t)&audiofilters_filter_get_filter_obj,
    (mp_obj_t)&audiofilters_filter_set_filter_obj);

static mp_obj_t audiofilters_filter_obj_get_mix(mp_obj_t self_in) {
    return common_hal_audiofilters_filter_get_mix(self_in);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiofilters_filter_get_mix_obj, audiofilters_filter_obj_get_mix);

static mp_obj_t audiofilters_filter_obj_set_mix(mp_obj_t self_in, mp_obj_t mix_in) {
    audiofilters_filter_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiofilters_filter_set_mix(self, mix_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiofilters_filter_set_mix_obj, audiofilters_filter_obj_set_mix);

MP_PROPERTY_GETSET(audiofilters_filter_mix_obj,
    (mp_obj_t)&audiofilters_filter_get_mix_obj,
    (mp_obj_t)&audiofilters_filter_set_mix_obj);

static mp_obj_t audiofilters_filter_obj_get_playing(mp_obj_t self_in) {
    audiofilters_filter_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_bool(common_hal_audiofilters_filter_get_playing(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiofilters_filter_get_playing_obj, audiofilters_filter_obj_get_playing);

MP_PROPERTY_GETTER(audiofilters_filter_playing_obj,
    (mp_obj_t)&audiofilters_filter_get_playing_obj);

static mp_obj_t audiofilters_filter_obj_play(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_sample, ARG_loop };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sample,    MP_ARG_OBJ | MP_ARG_REQUIRED, {} },
        { MP_QSTR_loop,      MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
    };
    audiofilters_filter_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    check_for_deinit(self);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_obj_t sample = args[ARG_sample].u_obj;
    common_hal_audiofilters_filter_play(self, sample, args[ARG_loop].u_bool);

    return MP_OBJ_FROM_PTR(self);
}
MP_DEFINE_CONST_FUN_OBJ_KW(audiofilters_filter_play_obj, 1, audiofilters_filter_obj_play);

static mp_obj_t audiofilters_filter_obj_stop(mp_obj_t self_in) {
    audiofilters_filter_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiofilters_filter_stop(self);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiofilters_filter_stop_obj, audiofilters_filter_obj_stop);

static const mp_rom_map_elem_t audiofilters_filter_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiofilters_filter_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiofilters_filter_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audiofilters_filter_stop_obj) },

    { MP_ROM_QSTR(MP_QSTR_playing), MP_ROM_PTR(&audiofilters_filter_playing_obj) },
    { MP_ROM_QSTR(MP_QSTR_filter), MP_ROM_PTR(&audiofilters_filter_filter_obj) },
    { MP_ROM_QSTR(MP_QSTR_mix), MP_ROM_PTR(&audiofilters_filter_mix_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiofilters_filter_locals_dict, audiofilters_filter_locals_dict_table);

static const audiosample_p_t audiofilters_filter_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audiofilters_filter_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audiofilters_filter_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiofilters_filter_type,
    MP_QSTR_Filter,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiofilters_filter_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiofilters_filter_locals_dict,
    protocol, &audiofilters_filter_proto
    );
