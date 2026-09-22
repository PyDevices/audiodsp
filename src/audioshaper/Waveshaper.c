// audioshaper.Waveshaper. See Waveshaper.h for provenance.
// SPDX-License-Identifier: MIT

#include "audioshaper/Waveshaper.h"

#include <string.h>

#include "cp_compat/context_manager_helpers.h"
#include "py/runtime.h"
#include "shared/audiodsp_pump_lock.h"

// The options `Waveshaper(...)` and `set(...)` accept, paired with the shared
// DSP's enum. `sample_rate`, `channel_count`, `oversample` and `curve` are
// deliberately absent: the first three are fixed at construction and the
// fourth is not a float, so all four are handled ahead of this table and
// keyword order stays irrelevant.
typedef struct {
    qstr name;
    audiodsp_shaper_option_t option;
} waveshaper_option_name_t;

static const waveshaper_option_name_t waveshaper_option_names[] = {
    { MP_QSTR_pre_gain, AUDIODSP_SHAPER_OPT_PRE_GAIN },
    { MP_QSTR_bias, AUDIODSP_SHAPER_OPT_BIAS },
    { MP_QSTR_post_gain, AUDIODSP_SHAPER_OPT_POST_GAIN },
    { MP_QSTR_mix, AUDIODSP_SHAPER_OPT_MIX },
    { MP_QSTR_hysteresis, AUDIODSP_SHAPER_OPT_HYSTERESIS },
    { MP_QSTR_hysteresis_width, AUDIODSP_SHAPER_OPT_HYSTERESIS_WIDTH },
    { MP_QSTR_hysteresis_bias, AUDIODSP_SHAPER_OPT_HYSTERESIS_BIAS },
};

// Copies the table out of whatever buffer the caller passed. int16, Q15, at
// least two points, spanning -1..+1 of input. It is copied rather than
// borrowed because a class computes it once at import and has no reason to
// keep the array alive afterwards.
static void waveshaper_load_curve(audioshaper_waveshaper_obj_t *self,
    mp_obj_t curve_in) {
    mp_buffer_info_t info;
    mp_get_buffer_raise(curve_in, &info, MP_BUFFER_READ);
    if (info.len < 4 || (info.len % 2) != 0) {
        mp_raise_ValueError(
            MP_ERROR_TEXT("curve must be at least two whole int16 points"));
    }
    int16_t *copy = m_malloc(info.len);
    memcpy(copy, info.buf, info.len);
    self->curve = copy;
    audiodsp_shaper_set_curve(&self->config, copy,
        (uint32_t)(info.len / 2));
}

static void waveshaper_apply_kwargs(audioshaper_waveshaper_obj_t *self,
    const mp_map_t *kw) {
    for (size_t i = 0; i < kw->alloc; ++i) {
        if (!mp_map_slot_is_filled(kw, i)) {
            continue;
        }
        qstr name = mp_obj_str_get_qstr(kw->table[i].key);
        if (name == MP_QSTR_sample_rate || name == MP_QSTR_channel_count ||
            name == MP_QSTR_oversample) {
            continue;
        }
        if (name == MP_QSTR_curve) {
            waveshaper_load_curve(self, kw->table[i].value);
            continue;
        }
        float value = (float)mp_obj_get_float(kw->table[i].value);
        bool known = false;
        for (size_t option = 0;
             option < MP_ARRAY_SIZE(waveshaper_option_names); ++option) {
            if (waveshaper_option_names[option].name == name) {
                audiodsp_shaper_configure(&self->config,
                    waveshaper_option_names[option].option, value);
                known = true;
                break;
            }
        }
        if (!known) {
            mp_raise_msg_varg(&mp_type_TypeError,
                MP_ERROR_TEXT("unknown Waveshaper option '%q'"), name);
        }
    }
    audiodsp_shaper_config_finish(&self->config);
}

static mp_obj_t audioshaper_waveshaper_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, true);
    mp_map_t kw_map;
    mp_map_init_fixed_table(&kw_map, n_kw, all_args + n_args);

    uint32_t sample_rate = 48000;
    uint32_t channel_count = 2;
    uint32_t oversample = 4;
    mp_obj_t curve = MP_OBJ_NULL;
    for (size_t i = 0; i < kw_map.alloc; ++i) {
        if (!mp_map_slot_is_filled(&kw_map, i)) {
            continue;
        }
        qstr name = mp_obj_str_get_qstr(kw_map.table[i].key);
        if (name == MP_QSTR_sample_rate) {
            sample_rate = (uint32_t)mp_obj_get_int(kw_map.table[i].value);
        } else if (name == MP_QSTR_channel_count) {
            channel_count = (uint32_t)mp_obj_get_int(kw_map.table[i].value);
        } else if (name == MP_QSTR_oversample) {
            oversample = (uint32_t)mp_obj_get_int(kw_map.table[i].value);
        } else if (name == MP_QSTR_curve) {
            curve = kw_map.table[i].value;
        }
    }
    if (channel_count < 1u || channel_count > 2u) {
        mp_raise_ValueError(MP_ERROR_TEXT("channel_count must be 1 or 2"));
    }
    // Powers of two only, and no higher than the state carries: rounding a
    // stray 3 down to 2 would be a different effect than the caller asked
    // for, quietly. The ceiling follows AUDIODSP_SHAPER_MAX_STAGES rather
    // than a list written out here, so a port that lowers it gets a check
    // that agrees with its own state.
    if (oversample < 1u || oversample > AUDIODSP_SHAPER_MAX_OVERSAMPLE ||
        (oversample & (oversample - 1u)) != 0u) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("oversample must be a power of two, 1 to %d"),
            (int)AUDIODSP_SHAPER_MAX_OVERSAMPLE);
    }
    if (curve == MP_OBJ_NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("curve is required"));
    }

    audioshaper_waveshaper_obj_t *self =
        mp_obj_malloc(audioshaper_waveshaper_obj_t, type);
    self->base.sample_rate = sample_rate;
    self->base.max_buffer_length = sizeof(self->buffer);
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)channel_count;
    self->base.samples_signed = 1;
    self->base.single_buffer = false;
    self->source = MP_OBJ_NULL;
    self->curve = NULL;
    self->pending = NULL;
    self->pending_frames = 0;

    audiodsp_shaper_config_init(&self->config, sample_rate, oversample);
    audiodsp_shaper_set_channel_count(&self->config, channel_count);
    audiodsp_shaper_state_init(&self->state);
    waveshaper_apply_kwargs(self, &kw_map);
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audioshaper_waveshaper_play(mp_obj_t self_in,
    mp_obj_t sample) {
    audioshaper_waveshaper_obj_t *self = MP_OBJ_TO_PTR(self_in);
    (void)audiosample_check(sample);
    audiodsp_pump_lock_acquire();
    self->source = sample;
    self->pending = NULL;
    self->pending_frames = 0;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audioshaper_waveshaper_play_obj,
    audioshaper_waveshaper_play);

static mp_obj_t audioshaper_waveshaper_set(size_t n_args,
    const mp_obj_t *args, mp_map_t *kw_args) {
    audioshaper_waveshaper_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    (void)n_args;
    waveshaper_apply_kwargs(self, kw_args);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audioshaper_waveshaper_set_obj, 1,
    audioshaper_waveshaper_set);

static mp_obj_t audioshaper_waveshaper_clear(mp_obj_t self_in) {
    audioshaper_waveshaper_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_acquire();
    audiodsp_shaper_reset(&self->state);
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioshaper_waveshaper_clear_obj,
    audioshaper_waveshaper_clear);

static audioio_get_buffer_result_t audioshaper_waveshaper_get_buffer(
    mp_obj_t self_in, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    audioshaper_waveshaper_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint32_t produced = 0;
    while (produced < AUDIODSP_SHAPER_FRAMES) {
        if (self->pending_frames == 0) {
            if (self->source == MP_OBJ_NULL) {
                break;
            }
            uint8_t *raw = NULL;
            uint32_t raw_bytes = 0;
            audioio_get_buffer_result_t result = audiosample_get_buffer(
                self->source, false, 0, &raw, &raw_bytes);
            const uint32_t width = 2u * self->base.channel_count;
            if (result == GET_BUFFER_ERROR || raw == NULL || raw_bytes < width) {
                break;
            }
            self->pending = (const int16_t *)raw;
            self->pending_frames = raw_bytes / width;
        }
        uint32_t run = AUDIODSP_SHAPER_FRAMES - produced;
        if (run > self->pending_frames) {
            run = self->pending_frames;
        }
        audiodsp_shaper_process_s16(&self->config, &self->state,
            &self->buffer[produced * self->base.channel_count], self->pending,
            run);
        self->pending += run * self->base.channel_count;
        self->pending_frames -= run;
        produced += run;
    }
    // A starved chain gets silence rather than a short block: this node sits
    // in the middle of a live graph and never reports itself finished. It has
    // no tail of its own -- no delay line, no reverberation -- so silence in
    // really is silence out, once the half-bands have rung down.
    if (produced == 0) {
        memset(self->buffer, 0, sizeof(self->buffer));
        produced = AUDIODSP_SHAPER_FRAMES;
    }
    *buffer = (uint8_t *)self->buffer;
    *buffer_length = produced * 2u * self->base.channel_count;
    return GET_BUFFER_MORE_DATA;
}

static void audioshaper_waveshaper_reset_buffer(mp_obj_t self_in,
    bool single_channel_output, uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    audioshaper_waveshaper_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->pending = NULL;
    self->pending_frames = 0;
    audiodsp_shaper_reset(&self->state);
}

// `deinit()` releases what this binding holds and marks the node
// deinitialised, which is what makes every guarded entry point raise
// afterwards -- `audiosample_get_buffer` and `audiosample_reset_buffer` in
// audiocore for the audio path, and the three shared properties. The node
// types audiodsp ported from CircuitPython have had this since they were
// ported; the ones audiodsp wrote itself did not, so no class built on them
// could release one and Tier 1's "deinit() releases every node the class
// built" was unmeasurable on a board (audiodsp#58, #60, #63).
//
// The inline buffers go with the object. What is cleared here is what the
// object holds a *reference* to: the upstream source, so releasing the tail
// of a chain lets the GC reclaim the rest of it, and every borrowed pointer
// into a source's buffer, so nothing dangles.
static mp_obj_t audioshaper_waveshaper_deinit(mp_obj_t self_in) {
    audioshaper_waveshaper_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    self->source = mp_const_none;
    self->pending = NULL;
    self->pending_frames = 0;
    self->curve = NULL;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioshaper_waveshaper_deinit_obj, audioshaper_waveshaper_deinit);

static const mp_rom_map_elem_t audioshaper_waveshaper_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audioshaper_waveshaper_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play),
      MP_ROM_PTR(&audioshaper_waveshaper_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_set),
      MP_ROM_PTR(&audioshaper_waveshaper_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear),
      MP_ROM_PTR(&audioshaper_waveshaper_clear_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audioshaper_waveshaper_locals,
    audioshaper_waveshaper_locals_table);

static const audiosample_p_t audioshaper_waveshaper_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = audioshaper_waveshaper_reset_buffer,
    .get_buffer = audioshaper_waveshaper_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audioshaper_waveshaper_type,
    MP_QSTR_Waveshaper,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audioshaper_waveshaper_make_new,
    attr, cp_compat_attr,
    locals_dict, &audioshaper_waveshaper_locals,
    protocol, &audioshaper_waveshaper_proto
    );
