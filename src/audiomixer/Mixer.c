// Ported from CircuitPython's shared-bindings/audiomixer/Mixer.c and
// shared-module/audiomixer/Mixer.c (upstream repo:
// https://github.com/adafruit/circuitpython, MIT), merged into one file
// (this port doesn't keep CP's shared-bindings/shared-module split).
//
// Deviations from upstream, both documented in docs/upstream-diff.md:
//   - mix_down_one_voice's `#if CIRCUITPY_SYNTHIO` branch is unconditional
//     here (this port always has synthio, tier 2); the plain-uint16 level/
//     panning fallback is dropped.
//   - The ARM Cortex-M4/M7 CMSIS DSP intrinsics (__QADD16/__UADD8/__UADD16,
//     the `#include "cmsis_compiler.h"`) are dropped in favor of the
//     portable C fallback unconditionally -- numerically identical, and
//     this port doesn't vendor CMSIS.
//   - m_malloc_without_collect -> m_malloc (no mainline equivalent, same
//     fix as tier 1/2).
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include "audiomixer/Mixer.h"
#include "shared/audiodsp_synth_dsp.h"
#include "audiomixer/MixerVoice.h"
#include "cp_compat/argcheck.h"
#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/objproperty.h"

#include <limits.h>
#include <stdint.h>

#include "py/binary.h"
#include "py/runtime.h"
#include "shared/audiodsp_pump_lock.h"

static mp_obj_t audiomixer_mixer_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_voice_count, ARG_buffer_size, ARG_channel_count, ARG_bits_per_sample, ARG_samples_signed, ARG_sample_rate };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_voice_count, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 2} },
        { MP_QSTR_buffer_size, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1024} },
        { MP_QSTR_channel_count, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 2} },
        { MP_QSTR_bits_per_sample, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 16} },
        { MP_QSTR_samples_signed, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = true} },
        { MP_QSTR_sample_rate, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 8000} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t voice_count = mp_arg_validate_int_range(args[ARG_voice_count].u_int, 1, 255, MP_QSTR_voice_count);
    mp_int_t channel_count = mp_arg_validate_int_range(args[ARG_channel_count].u_int, 1, 2, MP_QSTR_channel_count);
    mp_int_t sample_rate = mp_arg_validate_int_min(args[ARG_sample_rate].u_int, 1, MP_QSTR_sample_rate);
    mp_int_t bits_per_sample = args[ARG_bits_per_sample].u_int;
    if (bits_per_sample != 8 && bits_per_sample != 16) {
        mp_raise_ValueError(MP_ERROR_TEXT("bits_per_sample must be 8 or 16"));
    }
    audiomixer_mixer_obj_t *self =
        mp_obj_malloc_var(audiomixer_mixer_obj_t, voice, mp_obj_t, voice_count, &audiomixer_mixer_type);
    common_hal_audiomixer_mixer_construct(self, voice_count, args[ARG_buffer_size].u_int, bits_per_sample, args[ARG_samples_signed].u_bool, channel_count, sample_rate);

    for (int v = 0; v < voice_count; v++) {
        self->voice[v] = MP_OBJ_TYPE_GET_SLOT(&audiomixer_mixervoice_type, make_new)(&audiomixer_mixervoice_type, 0, 0, NULL);
        common_hal_audiomixer_mixervoice_set_parent(self->voice[v], self);
    }
    self->voice_tuple = mp_obj_new_tuple(self->voice_count, self->voice);

    return MP_OBJ_FROM_PTR(self);
}

void common_hal_audiomixer_mixer_construct(audiomixer_mixer_obj_t *self,
    uint8_t voice_count,
    uint32_t buffer_size,
    uint8_t bits_per_sample,
    bool samples_signed,
    uint8_t channel_count,
    uint32_t sample_rate) {
    self->len = buffer_size / 2 / sizeof(uint32_t) * sizeof(uint32_t);

    self->first_buffer = m_malloc(self->len);
    if (self->first_buffer == NULL) {
        common_hal_audiomixer_mixer_deinit(self);
        m_malloc_fail(self->len);
    }

    self->second_buffer = m_malloc(self->len);
    if (self->second_buffer == NULL) {
        common_hal_audiomixer_mixer_deinit(self);
        m_malloc_fail(self->len);
    }

    self->base.bits_per_sample = bits_per_sample;
    self->base.samples_signed = samples_signed;
    self->base.channel_count = channel_count;
    self->base.sample_rate = sample_rate;
    self->base.single_buffer = false;
    self->voice_count = voice_count;
    self->base.max_buffer_length = buffer_size;
}

void common_hal_audiomixer_mixer_deinit(audiomixer_mixer_obj_t *self) {
    // The whole body. mark_deinit is not the damage; the pointer
    // nulling AFTER it is -- the funnel's guard has already let a
    // pull in by then, and the pull writes into a buffer that has
    // just become NULL. Detach under the lock, free afterwards.
    audiodsp_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    self->first_buffer = NULL;
    self->second_buffer = NULL;
    audiodsp_pump_lock_release();
}

static mp_obj_t audiomixer_mixer_deinit(mp_obj_t self_in) {
    audiomixer_mixer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiomixer_mixer_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiomixer_mixer_deinit_obj, audiomixer_mixer_deinit);

static void check_for_deinit(audiomixer_mixer_obj_t *self) {
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

bool common_hal_audiomixer_mixer_get_playing(audiomixer_mixer_obj_t *self) {
    for (uint8_t v = 0; v < self->voice_count; v++) {
        if (common_hal_audiomixer_mixervoice_get_playing(MP_OBJ_TO_PTR(self->voice[v]))) {
            return true;
        }
    }
    return false;
}

static mp_obj_t audiomixer_mixer_obj_get_playing(mp_obj_t self_in) {
    audiomixer_mixer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_bool(common_hal_audiomixer_mixer_get_playing(self));
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiomixer_mixer_get_playing_obj, audiomixer_mixer_obj_get_playing);

MP_PROPERTY_GETTER(audiomixer_mixer_playing_obj,
    (mp_obj_t)&audiomixer_mixer_get_playing_obj);

static mp_obj_t audiomixer_mixer_obj_get_voice(mp_obj_t self_in) {
    audiomixer_mixer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return self->voice_tuple;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiomixer_mixer_get_voice_obj, audiomixer_mixer_obj_get_voice);

MP_PROPERTY_GETTER(audiomixer_mixer_voice_obj,
    (mp_obj_t)&audiomixer_mixer_get_voice_obj);

static mp_obj_t audiomixer_mixer_obj_play(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_sample, ARG_voice, ARG_loop };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sample,    MP_ARG_OBJ | MP_ARG_REQUIRED, {} },
        { MP_QSTR_voice,     MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_loop,      MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
    };
    audiomixer_mixer_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    check_for_deinit(self);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    uint8_t v = args[ARG_voice].u_int;
    if (v > (self->voice_count - 1)) {
        mp_arg_error_invalid(MP_QSTR_voice);
    }
    audiomixer_mixervoice_obj_t *voice = MP_OBJ_TO_PTR(self->voice[v]);
    mp_obj_t sample = args[ARG_sample].u_obj;
    common_hal_audiomixer_mixervoice_play(voice, sample, args[ARG_loop].u_bool);

    return MP_OBJ_FROM_PTR(self);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audiomixer_mixer_play_obj, 1, audiomixer_mixer_obj_play);

static mp_obj_t audiomixer_mixer_obj_stop_voice(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_voice };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_voice, MP_ARG_INT, {.u_int = 0} },
    };
    audiomixer_mixer_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    check_for_deinit(self);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    uint8_t v = args[ARG_voice].u_int;
    if (v > (self->voice_count - 1)) {
        mp_arg_error_invalid(MP_QSTR_voice);
    }
    audiomixer_mixervoice_obj_t *voice = MP_OBJ_TO_PTR(self->voice[v]);
    common_hal_audiomixer_mixervoice_stop(voice);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audiomixer_mixer_stop_voice_obj, 1, audiomixer_mixer_obj_stop_voice);

static const mp_rom_map_elem_t audiomixer_mixer_locals_dict_table[] = {
    // Methods
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiomixer_mixer_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiomixer_mixer_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop_voice), MP_ROM_PTR(&audiomixer_mixer_stop_voice_obj) },

    // Properties
    { MP_ROM_QSTR(MP_QSTR_playing), MP_ROM_PTR(&audiomixer_mixer_playing_obj) },
    { MP_ROM_QSTR(MP_QSTR_voice), MP_ROM_PTR(&audiomixer_mixer_voice_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiomixer_mixer_locals_dict, audiomixer_mixer_locals_dict_table);

// --- mixdown engine (shared-module) -------------------------------------

// Deviation from upstream, which stops every voice here instead of rewinding
// them -- see docs/upstream-diff.md, "Resetting a Mixer silenced it". Anything
// that pulls from a Mixer resets it first (every effect's play() does), so
// upstream's version makes a Mixer feeding an effect render silence forever.
void audiomixer_mixer_reset_buffer(audiomixer_mixer_obj_t *self,
    bool single_channel_output,
    uint8_t channel) {
    for (uint8_t i = 0; i < self->voice_count; i++) {
        common_hal_audiomixer_mixervoice_reset(self->voice[i]);
    }
}

static inline uint32_t add16signed(uint32_t a, uint32_t b) {
    uint32_t result = 0;
    for (int8_t i = 0; i < 2; i++) {
        int16_t ai = a >> (sizeof(int16_t) * 8 * i);
        int16_t bi = b >> (sizeof(int16_t) * 8 * i);
        int32_t intermediate = (int32_t)ai + bi;
        if (intermediate > SHRT_MAX) {
            intermediate = SHRT_MAX;
        } else if (intermediate < SHRT_MIN) {
            intermediate = SHRT_MIN;
        }
        result |= (((uint32_t)intermediate) & 0xffff) << (sizeof(int16_t) * 8 * i);
    }
    return result;
}

// Deviation from upstream: a multiplier of 1<<15 is unity, and passes the
// sample through. Upstream maps a voice level of 1.0 to Q15 32768 and then
// scales by `level / 32767.0f`, so unity is really 1.0000305 and every
// sample at |value| >= 32736 comes out one LSB larger. See
// docs/upstream-diff.md, "A voice at level 1.0 is a wire". Only the unity
// multiplier takes this branch; every other level is upstream's arithmetic,
// bit for bit.
static inline uint32_t mult16signed(uint32_t val, int32_t lomul, int32_t himul) {
    uint32_t result = 0;
    for (int8_t i = 0; i < 2; i++) {
        int32_t mul = i ? himul : lomul;
        int16_t ai = (val >> (sizeof(uint16_t) * 8 * i));
        if (mul == (1 << 15)) {
            result |= (((uint32_t)ai) & 0xffff) << (sizeof(int16_t) * 8 * i);
            continue;
        }
        float mod_mul = (float)mul / (float)((1 << 15) - 1);
        int32_t intermediate = (int32_t)(ai * mod_mul);
        if (intermediate > SHRT_MAX) {
            intermediate = SHRT_MAX;
        } else if (intermediate < SHRT_MIN) {
            intermediate = SHRT_MIN;
        }
        intermediate &= 0x0000FFFF;
        result |= (((uint32_t)intermediate)) << (sizeof(int16_t) * 8 * i);
    }
    return result;
}

static inline uint32_t tounsigned8(uint32_t val) {
    return val ^ 0x80808080;
}

static inline uint32_t tounsigned16(uint32_t val) {
    return val ^ 0x80008000;
}

static inline uint32_t tosigned16(uint32_t val) {
    return val ^ 0x80008000;
}

static inline uint32_t unpack8(uint16_t val) {
    return ((val & 0xff00) << 16) | ((val & 0x00ff) << 8);
}

static inline uint32_t pack8(uint32_t val) {
    return ((val & 0xff000000) >> 16) | ((val & 0xff00) >> 8);
}

static inline uint32_t copy16lsb(uint32_t val) {
    val &= 0x0000ffff;
    return val | (val << 16);
}

static inline uint32_t copy16msb(uint32_t val) {
    val &= 0xffff0000;
    return val | (val >> 16);
}

// Deviation from upstream: `__attribute__((unused))` added to both. Genuine
// dead code carried over faithfully from CP's own Mixer.c (confirmed by
// reading it directly: neither is called there either, only
// copy16lsb/copy16msb are) -- not a port artifact. unix/windows never
// warned on it; emscripten's `-Wall -Werror` (phase 8d) does.
static inline uint32_t copy8lsb(uint32_t val) __attribute__((unused));
static inline uint32_t copy8lsb(uint32_t val) {
    val &= 0x00ff;
    return val | (val << 8);
}

static inline uint32_t copy8msb(uint32_t val) __attribute__((unused));
static inline uint32_t copy8msb(uint32_t val) {
    val &= 0xff00;
    return val | (val >> 8);
}

#define ALMOST_ONE (MICROPY_FLOAT_CONST(32767.) / 32768)

static void mix_down_one_voice(audiomixer_mixer_obj_t *self,
    audiomixer_mixervoice_obj_t *voice, bool voices_active,
    uint32_t *word_buffer, uint32_t length) {
    audiosample_base_t *sample = MP_OBJ_TO_PTR(voice->sample);
    // A fetch that yields less than one packed word leaves buffer_length at
    // zero, so the pass below would consume nothing and `length` would not
    // move. One is tolerated -- a source is allowed a beat to produce -- and a
    // second consecutive one means it never will, so the voice stops and its
    // remainder is zero-filled like any other short voice.
    //
    // Nothing above catches that. The not-more-data branch either rewinds (if
    // looping) or stops, and the fetch that follows is not examined at all, so
    // a source that keeps handing back half a word spins here forever. That is
    // the hang in audiodsp#85. The CPython twin has had two exits for this
    // since audiodsp#24 -- a stop when a fetch yields nothing and claims no
    // more, and this counter for a source that yields nothing while claiming
    // MORE_DATA -- and each was measured to be sufficient on its own; removing
    // both is what makes the twin hang. This side had neither.
    //
    // play() refuses the sample that reaches it by the documented route, with
    // a ValueError the programmer can act on. This is the backstop under it,
    // for the routes play() cannot see: `voice.loop = True` set afterwards,
    // and a source whose content play()'s one fetch did not reveal.
    uint8_t empty_fetches = 0;
    while (length != 0) {
        if (voice->buffer_length == 0) {
            if (!voice->more_data) {
                if (voice->loop) {
                    audiosample_reset_buffer(voice->sample, false, 0);
                } else {
                    voice->sample = NULL;
                    break;
                }
            }
            if (voice->sample) {
                // Load another buffer
                audioio_get_buffer_result_t result = audiosample_get_buffer(voice->sample, false, 0, (uint8_t **)&voice->remaining_buffer, &voice->buffer_length);
                // A deinited sample answers GET_BUFFER_ERROR with a NULL
                // buffer (audiocore's funnel guard). Drop the voice rather
                // than mixing from it.
                if (result == GET_BUFFER_ERROR) {
                    voice->sample = NULL;
                    voice->buffer_length = 0;
                    voice->more_data = false;
                    break;
                }
                // Track length in terms of words.
                voice->buffer_length /= sizeof(uint32_t);
                voice->more_data = result == GET_BUFFER_MORE_DATA;
                if (voice->buffer_length == 0) {
                    if (++empty_fetches > 1) {
                        voice->sample = NULL;
                        break;
                    }
                    continue;
                }
                empty_fetches = 0;
            }
        }

        uint32_t *src = voice->remaining_buffer;

        uint32_t n;
        if (MP_LIKELY(self->base.channel_count == sample->channel_count)) {
            n = MIN(MIN(voice->buffer_length, length), SYNTHIO_MAX_DUR * self->base.channel_count);
        } else {
            n = MIN(MIN(voice->buffer_length << 1, length), SYNTHIO_MAX_DUR * self->base.channel_count);
        }

        // Get the current level from the BlockInput. These may change at run time so you need to do bounds checking if required.
        shared_bindings_synthio_lfo_tick(self->base.sample_rate, n / self->base.channel_count);
        uint16_t level = (uint16_t)(synthio_block_slot_get_limited(&voice->level, MICROPY_FLOAT_CONST(0.0), MICROPY_FLOAT_CONST(1.0)) * (1 << 15));
        int16_t panning = synthio_block_slot_get_scaled(&voice->panning, -ALMOST_ONE, ALMOST_ONE);

        uint16_t left_panning_scaled = 32768, right_panning_scaled = 32768;
        if (MP_LIKELY(self->base.channel_count == 2)) {
            // panning > 0 attenuates the LEFT channel. CircuitPython 10.3.0
            // flipped this and matched synthio.Note to it.
            if (panning >= 0) {
                left_panning_scaled = 32767 - panning;
            } else {
                right_panning_scaled = 32767 + panning;
            }
        }

        int32_t pending_lo_level = level;
        int32_t pending_hi_level = level;
        if (MP_LIKELY(self->base.channel_count == 2)) {
            pending_lo_level = (left_panning_scaled * pending_lo_level) >> 15;
            pending_hi_level = (right_panning_scaled * pending_hi_level) >> 15;
        }

        // A level or pan move waits for a zero crossing so it cannot click.
        int32_t active_lo_level = voice->active_lo_level;
        int32_t active_hi_level = voice->active_hi_level;
        uint32_t last_word = 0;

        // First active voice gets copied over verbatim.
        if (!voices_active) {
            if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                if (MP_LIKELY(self->base.samples_signed)) {
                    if (MP_LIKELY(self->base.channel_count == sample->channel_count)) {
                        for (uint32_t i = 0; i < n; i++) {
                            uint32_t word = src[i];
                            audiodsp_assign_packed_level(word, &last_word, &active_lo_level, &active_hi_level, pending_lo_level, pending_hi_level);
                            word_buffer[i] = mult16signed(word, active_lo_level, active_hi_level);
                        }
                    } else {
                        for (uint32_t i = 0; i < n; i += 2) {
                            uint32_t word = src[i >> 1];
                            uint32_t word_lsb = copy16lsb(word);
                            audiodsp_assign_packed_level(word_lsb, &last_word, &active_lo_level, &active_hi_level, pending_lo_level, pending_hi_level);
                            word_buffer[i] = mult16signed(word_lsb, active_lo_level, active_hi_level);
                            word = copy16msb(word);
                            audiodsp_assign_packed_level(word, &last_word, &active_lo_level, &active_hi_level, pending_lo_level, pending_hi_level);
                            word_buffer[i + 1] = mult16signed(word, active_lo_level, active_hi_level);
                        }
                    }
                } else {
                    if (MP_LIKELY(self->base.channel_count == sample->channel_count)) {
                        for (uint32_t i = 0; i < n; i++) {
                            uint32_t word = src[i];
                            word = tosigned16(word);
                            audiodsp_assign_packed_level(word, &last_word, &active_lo_level, &active_hi_level, pending_lo_level, pending_hi_level);
                            word_buffer[i] = mult16signed(word, active_lo_level, active_hi_level);
                        }
                    } else {
                        for (uint32_t i = 0; i + 1 < n; i += 2) {
                            uint32_t word = src[i >> 1];
                            word = tosigned16(word);
                            uint32_t word_lsb = copy16lsb(word);
                            audiodsp_assign_packed_level(word_lsb, &last_word, &active_lo_level, &active_hi_level, pending_lo_level, pending_hi_level);
                            word_buffer[i] = mult16signed(word_lsb, active_lo_level, active_hi_level);
                            word = copy16msb(word);
                            audiodsp_assign_packed_level(word, &last_word, &active_lo_level, &active_hi_level, pending_lo_level, pending_hi_level);
                            word_buffer[i + 1] = mult16signed(word, active_lo_level, active_hi_level);
                        }
                    }
                }
            } else {
                uint16_t *hword_buffer = (uint16_t *)word_buffer;
                uint16_t *hsrc = (uint16_t *)src;
                if (MP_LIKELY(self->base.channel_count == sample->channel_count)) {
                    for (uint32_t i = 0; i < n * 2; i++) {
                        uint32_t word = unpack8(hsrc[i]);
                        if (MP_LIKELY(!self->base.samples_signed)) {
                            word = tosigned16(word);
                        }
                        audiodsp_assign_packed_level(word, &last_word, &active_lo_level, &active_hi_level, pending_lo_level, pending_hi_level);
                        word = mult16signed(word, active_lo_level, active_hi_level);
                        hword_buffer[i] = pack8(word);
                    }
                } else {
                    for (uint32_t i = 0; i + 1 < n * 2; i += 2) {
                        uint32_t word = unpack8(hsrc[i >> 1]);
                        if (MP_LIKELY(!self->base.samples_signed)) {
                            word = tosigned16(word);
                        }
                        uint32_t word_lsb = copy16lsb(word);
                        audiodsp_assign_packed_level(word_lsb, &last_word, &active_lo_level, &active_hi_level, pending_lo_level, pending_hi_level);
                        hword_buffer[i] = pack8(mult16signed(word_lsb, active_lo_level, active_hi_level));
                        word = copy16msb(word);
                        audiodsp_assign_packed_level(word, &last_word, &active_lo_level, &active_hi_level, pending_lo_level, pending_hi_level);
                        hword_buffer[i + 1] = pack8(mult16signed(word, active_lo_level, active_hi_level));
                    }
                }
            }
        } else {
            if (MP_LIKELY(self->base.bits_per_sample == 16)) {
                if (MP_LIKELY(self->base.samples_signed)) {
                    if (MP_LIKELY(self->base.channel_count == sample->channel_count)) {
                        for (uint32_t i = 0; i < n; i++) {
                            uint32_t word = src[i];
                            audiodsp_assign_packed_level(word, &last_word, &active_lo_level, &active_hi_level, pending_lo_level, pending_hi_level);
                            word_buffer[i] = add16signed(mult16signed(word, active_lo_level, active_hi_level), word_buffer[i]);
                        }
                    } else {
                        for (uint32_t i = 0; i + 1 < n; i += 2) {
                            uint32_t word = src[i >> 1];
                            uint32_t word_lsb = copy16lsb(word);
                            audiodsp_assign_packed_level(word_lsb, &last_word, &active_lo_level, &active_hi_level, pending_lo_level, pending_hi_level);
                            word_buffer[i] = add16signed(mult16signed(word_lsb, active_lo_level, active_hi_level), word_buffer[i]);
                            word = copy16msb(word);
                            audiodsp_assign_packed_level(word, &last_word, &active_lo_level, &active_hi_level, pending_lo_level, pending_hi_level);
                            word_buffer[i + 1] = add16signed(mult16signed(word, active_lo_level, active_hi_level), word_buffer[i + 1]);
                        }
                    }
                } else {
                    if (MP_LIKELY(self->base.channel_count == sample->channel_count)) {
                        for (uint32_t i = 0; i < n; i++) {
                            uint32_t word = src[i];
                            word = tosigned16(word);
                            audiodsp_assign_packed_level(word, &last_word, &active_lo_level, &active_hi_level, pending_lo_level, pending_hi_level);
                            word_buffer[i] = add16signed(mult16signed(word, active_lo_level, active_hi_level), word_buffer[i]);
                        }
                    } else {
                        for (uint32_t i = 0; i + 1 < n; i += 2) {
                            uint32_t word = src[i >> 1];
                            word = tosigned16(word);
                            uint32_t word_lsb = copy16lsb(word);
                            audiodsp_assign_packed_level(word_lsb, &last_word, &active_lo_level, &active_hi_level, pending_lo_level, pending_hi_level);
                            word_buffer[i] = add16signed(mult16signed(word_lsb, active_lo_level, active_hi_level), word_buffer[i]);
                            word = copy16msb(word);
                            audiodsp_assign_packed_level(word, &last_word, &active_lo_level, &active_hi_level, pending_lo_level, pending_hi_level);
                            word_buffer[i + 1] = add16signed(mult16signed(word, active_lo_level, active_hi_level), word_buffer[i + 1]);
                        }
                    }
                }
            } else {
                uint16_t *hword_buffer = (uint16_t *)word_buffer;
                uint16_t *hsrc = (uint16_t *)src;
                if (MP_LIKELY(self->base.channel_count == sample->channel_count)) {
                    for (uint32_t i = 0; i < n * 2; i++) {
                        uint32_t word = unpack8(hsrc[i]);
                        if (MP_LIKELY(!self->base.samples_signed)) {
                            word = tosigned16(word);
                        }
                        audiodsp_assign_packed_level(word, &last_word, &active_lo_level, &active_hi_level, pending_lo_level, pending_hi_level);
                        word = mult16signed(word, active_lo_level, active_hi_level);
                        word = add16signed(word, unpack8(hword_buffer[i]));
                        hword_buffer[i] = pack8(word);
                    }
                } else {
                    for (uint32_t i = 0; i + 1 < n * 2; i += 2) {
                        uint32_t word = unpack8(hsrc[i >> 1]);
                        if (MP_LIKELY(!self->base.samples_signed)) {
                            word = tosigned16(word);
                        }
                        uint32_t word_lsb = copy16lsb(word);
                        audiodsp_assign_packed_level(word_lsb, &last_word, &active_lo_level, &active_hi_level, pending_lo_level, pending_hi_level);
                        hword_buffer[i] = pack8(add16signed(mult16signed(word_lsb, active_lo_level, active_hi_level), unpack8(hword_buffer[i])));
                        word = copy16msb(word);
                        audiodsp_assign_packed_level(word, &last_word, &active_lo_level, &active_hi_level, pending_lo_level, pending_hi_level);
                        hword_buffer[i + 1] = pack8(add16signed(mult16signed(word, active_lo_level, active_hi_level), unpack8(hword_buffer[i + 1])));
                    }
                }
            }
        }
        length -= n;
        word_buffer += n;
        if (MP_LIKELY(self->base.channel_count == sample->channel_count)) {
            voice->remaining_buffer += n;
            voice->buffer_length -= n;
        } else {
            voice->remaining_buffer += n >> 1;
            voice->buffer_length -= n >> 1;
        }

        // Force the active level to match the pending level in the case that the conditions of a
        // zero crossing weren't met within the last `SYNTHIO_MAX_DUR` frames. Will ensure minimal
        // delay in level or panning changes at the potential expensive of an audible pop.
        voice->active_lo_level = pending_lo_level;
        voice->active_hi_level = pending_hi_level;
    }

    if (length && !voices_active) {
        for (uint32_t i = 0; i < length; i++) {
            word_buffer[i] = 0;
        }
    }
}

audioio_get_buffer_result_t audiomixer_mixer_get_buffer(audiomixer_mixer_obj_t *self,
    bool single_channel_output,
    uint8_t channel,
    uint8_t **buffer,
    uint32_t *buffer_length) {
    if (!single_channel_output) {
        channel = 0;
    }

    uint32_t channel_read_count = self->left_read_count;
    if (channel == 1) {
        channel_read_count = self->right_read_count;
    }
    *buffer_length = self->len;

    bool need_more_data = self->read_count == channel_read_count;
    if (need_more_data) {
        uint32_t *word_buffer;
        if (self->use_first_buffer) {
            *buffer = (uint8_t *)self->first_buffer;
            word_buffer = self->first_buffer;
        } else {
            *buffer = (uint8_t *)self->second_buffer;
            word_buffer = self->second_buffer;
        }
        self->use_first_buffer = !self->use_first_buffer;
        bool voices_active = false;
        uint32_t length = self->len / sizeof(uint32_t);

        for (int32_t v = 0; v < self->voice_count; v++) {
            audiomixer_mixervoice_obj_t *voice = MP_OBJ_TO_PTR(self->voice[v]);
            if (voice->sample) {
                mix_down_one_voice(self, voice, voices_active, word_buffer, length);
                voices_active = true;
            }
        }

        if (!voices_active) {
            for (uint32_t i = 0; i < length; i++) {
                word_buffer[i] = 0;
            }
        }

        if (!self->base.samples_signed) {
            if (self->base.bits_per_sample == 16) {
                for (uint32_t i = 0; i < length; i++) {
                    word_buffer[i] = tounsigned16(word_buffer[i]);
                }
            } else {
                for (uint32_t i = 0; i < length; i++) {
                    word_buffer[i] = tounsigned8(word_buffer[i]);
                }
            }
        }

        self->read_count += 1;
    } else if (!self->use_first_buffer) {
        *buffer = (uint8_t *)self->first_buffer;
    } else {
        *buffer = (uint8_t *)self->second_buffer;
    }


    if (channel == 0) {
        self->left_read_count += 1;
    } else if (channel == 1) {
        self->right_read_count += 1;
        *buffer = *buffer + self->base.bits_per_sample / 8;
    }
    return GET_BUFFER_MORE_DATA;
}

// What is behind this node, for audiosample_find_type()'s walk. A Mixer has
// one slot per voice; a voice with nothing playing is an EMPTY slot rather
// than the end of the list, or a silent voice 0 would hide the rest.
// audiodsp#112.
static mp_obj_t audiomixer_mixer_sources(mp_obj_t self_in, uint8_t index) {
    audiomixer_mixer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (index >= self->voice_count) {
        return MP_OBJ_NULL;
    }
    audiomixer_mixervoice_obj_t *voice = MP_OBJ_TO_PTR(self->voice[index]);
    return voice->sample == MP_OBJ_NULL ? mp_const_none : voice->sample;
}

static const audiosample_p_t audiomixer_mixer_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audiomixer_mixer_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audiomixer_mixer_get_buffer,
    .sources = audiomixer_mixer_sources,
};

// Deviation from upstream: adds `attr, cp_compat_attr` -- see
// cp_compat/objproperty.c for why a ported type's MP_PROPERTY_GETTER/GETSET
// locals_dict entries need this to actually be invoked on this port.
MP_DEFINE_CONST_OBJ_TYPE(
    audiomixer_mixer_type,
    MP_QSTR_Mixer,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiomixer_mixer_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiomixer_mixer_locals_dict,
    protocol, &audiomixer_mixer_proto
    );
