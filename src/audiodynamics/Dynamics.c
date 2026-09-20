// audiodynamics.Dynamics. See Dynamics.h for provenance.
// SPDX-License-Identifier: MIT

#include "audiodynamics/Dynamics.h"

#include <string.h>

#include "cp_compat/context_manager_helpers.h"
#include "py/runtime.h"
#include "shared/audioif_pump_lock.h"

// The options `Dynamics(...)` and `set(...)` accept, paired with the shared
// DSP's enum. `sample_rate` is deliberately absent: the millisecond
// conversions read it, so it is applied ahead of this table rather than from
// it, and keyword order stays irrelevant.
typedef struct {
    qstr name;
    audioif_dynamics_option_t option;
} dynamics_option_name_t;

static const dynamics_option_name_t dynamics_option_names[] = {
    { MP_QSTR_threshold_db, AUDIOIF_DYNAMICS_OPT_THRESHOLD_DB },
    { MP_QSTR_ratio, AUDIOIF_DYNAMICS_OPT_RATIO },
    { MP_QSTR_knee_db, AUDIOIF_DYNAMICS_OPT_KNEE_DB },
    { MP_QSTR_makeup_db, AUDIOIF_DYNAMICS_OPT_MAKEUP_DB },
    { MP_QSTR_attack_ms, AUDIOIF_DYNAMICS_OPT_ATTACK_MS },
    { MP_QSTR_release_ms, AUDIOIF_DYNAMICS_OPT_RELEASE_MS },
    { MP_QSTR_attack_gain_db, AUDIOIF_DYNAMICS_OPT_ATTACK_GAIN_DB },
    { MP_QSTR_sustain_gain_db, AUDIOIF_DYNAMICS_OPT_SUSTAIN_GAIN_DB },
    { MP_QSTR_sidechain_hz, AUDIOIF_DYNAMICS_OPT_SIDECHAIN_HZ },
    { MP_QSTR_lookahead_ms, AUDIOIF_DYNAMICS_OPT_LOOKAHEAD_MS },
    { MP_QSTR_true_peak, AUDIOIF_DYNAMICS_OPT_TRUE_PEAK },
    // The effects program's additions, every one of them default-off.
    { MP_QSTR_transient_fast_attack_ms,
      AUDIOIF_DYNAMICS_OPT_TRANSIENT_FAST_ATTACK_MS },
    { MP_QSTR_transient_fast_release_ms,
      AUDIOIF_DYNAMICS_OPT_TRANSIENT_FAST_RELEASE_MS },
    { MP_QSTR_transient_slow_attack_ms,
      AUDIOIF_DYNAMICS_OPT_TRANSIENT_SLOW_ATTACK_MS },
    { MP_QSTR_transient_slow_release_ms,
      AUDIOIF_DYNAMICS_OPT_TRANSIENT_SLOW_RELEASE_MS },
    { MP_QSTR_detector, AUDIOIF_DYNAMICS_OPT_DETECTOR },
    { MP_QSTR_rms_ms, AUDIOIF_DYNAMICS_OPT_RMS_MS },
    { MP_QSTR_feedback_detector, AUDIOIF_DYNAMICS_OPT_FEEDBACK_DETECTOR },
    { MP_QSTR_sidechain_lp_hz, AUDIOIF_DYNAMICS_OPT_SIDECHAIN_LP_HZ },
    { MP_QSTR_sidechain_poles, AUDIOIF_DYNAMICS_OPT_SIDECHAIN_POLES },
    { MP_QSTR_key_listen, AUDIOIF_DYNAMICS_OPT_KEY_LISTEN },
    { MP_QSTR_depth_db, AUDIOIF_DYNAMICS_OPT_DEPTH_DB },
    { MP_QSTR_hold_ms, AUDIOIF_DYNAMICS_OPT_HOLD_MS },
    { MP_QSTR_hysteresis_db, AUDIOIF_DYNAMICS_OPT_HYSTERESIS_DB },
    { MP_QSTR_relative_threshold, AUDIOIF_DYNAMICS_OPT_RELATIVE_THRESHOLD },
    { MP_QSTR_program_attack, AUDIOIF_DYNAMICS_OPT_PROGRAM_ATTACK },
    { MP_QSTR_transient_dual, AUDIOIF_DYNAMICS_OPT_TRANSIENT_DUAL },
    { MP_QSTR_sustain_fast_attack_ms,
      AUDIOIF_DYNAMICS_OPT_SUSTAIN_FAST_ATTACK_MS },
    { MP_QSTR_sustain_fast_release_ms,
      AUDIOIF_DYNAMICS_OPT_SUSTAIN_FAST_RELEASE_MS },
    { MP_QSTR_sustain_slow_attack_ms,
      AUDIOIF_DYNAMICS_OPT_SUSTAIN_SLOW_ATTACK_MS },
    { MP_QSTR_sustain_slow_release_ms,
      AUDIOIF_DYNAMICS_OPT_SUSTAIN_SLOW_RELEASE_MS },
    { MP_QSTR_slow_hold_ms, AUDIOIF_DYNAMICS_OPT_SLOW_HOLD_MS },
    { MP_QSTR_gain_smooth_ms, AUDIOIF_DYNAMICS_OPT_GAIN_SMOOTH_MS },
    { MP_QSTR_feedback_gain_corrected,
      AUDIOIF_DYNAMICS_OPT_FEEDBACK_GAIN_CORRECTED },
};

// The lookahead buffer is allocated only once someone asks for one, and only
// ever grows: `set(lookahead_ms=...)` mid-stream is a live gesture, and
// shrinking would mean freeing memory the DSP is reading out of.
static void dynamics_ensure_lookahead(audiodynamics_dynamics_obj_t *self) {
    const uint32_t wanted = audioif_dynamics_lookahead_frames(&self->config);
    if (wanted == 0 || wanted <= self->state.lookahead_capacity) {
        return;
    }
    int16_t *buffer = m_malloc((size_t)wanted * self->base.channel_count *
        sizeof(int16_t));
    audioif_dynamics_set_lookahead(&self->state, buffer, wanted);
}

static void dynamics_apply_kwargs(audiodynamics_dynamics_obj_t *self,
    const mp_map_t *kw) {
    for (size_t i = 0; i < kw->alloc; ++i) {
        if (mp_map_slot_is_filled(kw, i) &&
            mp_obj_str_get_qstr(kw->table[i].key) == MP_QSTR_sample_rate) {
            self->base.sample_rate =
                (uint32_t)mp_obj_get_int(kw->table[i].value);
            self->config.sample_rate = self->base.sample_rate;
        }
    }
    for (size_t i = 0; i < kw->alloc; ++i) {
        if (mp_map_slot_is_filled(kw, i) &&
            mp_obj_str_get_qstr(kw->table[i].key) == MP_QSTR_channel_count) {
            mp_int_t channels = mp_obj_get_int(kw->table[i].value);
            if (channels < 1 || channels > 2) {
                mp_raise_ValueError(MP_ERROR_TEXT(
                    "channel_count must be 1 or 2"));
            }
            self->base.channel_count = (uint8_t)channels;
            audioif_dynamics_set_channel_count(&self->config, &self->state,
                (uint32_t)channels);
        }
    }
    for (size_t i = 0; i < kw->alloc; ++i) {
        if (!mp_map_slot_is_filled(kw, i)) {
            continue;
        }
        qstr name = mp_obj_str_get_qstr(kw->table[i].key);
        if (name == MP_QSTR_sample_rate || name == MP_QSTR_channel_count) {
            continue;
        }
        // `detector=` reads better as a word than as a number. Every option
        // the DSP takes is a float, so the word is mapped to one here rather
        // than teaching the kernel about strings.
        float value;
        if (name == MP_QSTR_detector &&
            mp_obj_is_str(kw->table[i].value)) {
            const qstr word = mp_obj_str_get_qstr(kw->table[i].value);
            if (word == MP_QSTR_rms) {
                value = (float)AUDIOIF_DYNAMICS_DETECT_RMS;
            } else if (word == MP_QSTR_peak) {
                value = (float)AUDIOIF_DYNAMICS_DETECT_PEAK;
            } else {
                mp_raise_ValueError(MP_ERROR_TEXT(
                    "detector must be 'peak' or 'rms'"));
            }
        } else {
            value = (float)mp_obj_get_float(kw->table[i].value);
        }
        bool known = false;
        for (size_t option = 0; option < MP_ARRAY_SIZE(dynamics_option_names);
             ++option) {
            if (dynamics_option_names[option].name == name) {
                audioif_dynamics_configure(&self->config,
                    dynamics_option_names[option].option, value);
                known = true;
                break;
            }
        }
        if (!known) {
            mp_raise_msg_varg(&mp_type_TypeError,
                MP_ERROR_TEXT("unknown Dynamics option '%q'"), name);
        }
    }
    dynamics_ensure_lookahead(self);
}

static mp_obj_t audiodynamics_dynamics_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 0, 1, true);
    audiodynamics_dynamics_obj_t *self =
        mp_obj_malloc(audiodynamics_dynamics_obj_t, type);
    self->base.sample_rate = 48000;
    self->base.max_buffer_length = sizeof(self->buffer);
    self->base.bits_per_sample = 16;
    self->base.channel_count = 2;
    self->base.samples_signed = 1;
    self->base.single_buffer = false;
    self->source = MP_OBJ_NULL;
    self->pending = NULL;
    self->pending_frames = 0;
    self->key_source = MP_OBJ_NULL;
    self->key_pending = NULL;
    self->key_pending_frames = 0;

    const int mode = n_args >= 1 ? (int)mp_obj_get_int(all_args[0])
                                 : AUDIOIF_DYNAMICS_COMPRESS;
    audioif_dynamics_config_init(&self->config, mode, self->base.sample_rate);
    audioif_dynamics_state_init(&self->state);

    mp_map_t kw_map;
    mp_map_init_fixed_table(&kw_map, n_kw, all_args + n_args);
    dynamics_apply_kwargs(self, &kw_map);
    audioif_dynamics_config_finish(&self->config);
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audiodynamics_dynamics_play(mp_obj_t self_in, mp_obj_t sample) {
    audiodynamics_dynamics_obj_t *self = MP_OBJ_TO_PTR(self_in);
    (void)audiosample_check(sample);
    audioif_pump_lock_acquire();
    self->source = sample;
    self->pending = NULL;
    self->pending_frames = 0;
    audioif_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiodynamics_dynamics_play_obj,
    audiodynamics_dynamics_play);

// Feed the detector from a second stream, while the gain still lands on
// whatever play() is playing. None goes back to reading the audio.
static mp_obj_t audiodynamics_dynamics_key(mp_obj_t self_in, mp_obj_t sample) {
    audiodynamics_dynamics_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (sample == mp_const_none) {
        self->key_source = MP_OBJ_NULL;
    } else {
        (void)audiosample_check(sample);
        self->key_source = sample;
    }
    audioif_pump_lock_acquire();
    self->key_pending = NULL;
    self->key_pending_frames = 0;
    audioif_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiodynamics_dynamics_key_obj,
    audiodynamics_dynamics_key);

static mp_obj_t audiodynamics_dynamics_set(size_t n_args,
    const mp_obj_t *args, mp_map_t *kw_args) {
    audiodynamics_dynamics_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    (void)n_args;
    dynamics_apply_kwargs(self, kw_args);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audiodynamics_dynamics_set_obj, 1,
    audiodynamics_dynamics_set);

static mp_obj_t audiodynamics_dynamics_gain_reduction_db(mp_obj_t self_in) {
    audiodynamics_dynamics_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_float((mp_float_t)self->state.gain_reduction_db);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiodynamics_dynamics_gain_reduction_db_obj,
    audiodynamics_dynamics_gain_reduction_db);

static audioio_get_buffer_result_t audiodynamics_dynamics_get_buffer(
    mp_obj_t self_in, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    audiodynamics_dynamics_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint32_t produced = 0;
    while (produced < AUDIOIF_DYNAMICS_FRAMES) {
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
        uint32_t run = AUDIOIF_DYNAMICS_FRAMES - produced;
        if (run > self->pending_frames) {
            run = self->pending_frames;
        }
        const int16_t *key = NULL;
        if (self->key_source != MP_OBJ_NULL) {
            if (self->key_pending_frames == 0) {
                uint8_t *raw = NULL;
                uint32_t raw_bytes = 0;
                audioio_get_buffer_result_t result = audiosample_get_buffer(
                    self->key_source, false, 0, &raw, &raw_bytes);
                const uint32_t width = 2u * self->base.channel_count;
                if (result == GET_BUFFER_ERROR || raw == NULL ||
                    raw_bytes < width) {
                    // A key that has run dry starves the node exactly the way
                    // an absent source does, rather than silently reverting
                    // the detector to the audio.
                    break;
                }
                self->key_pending = (const int16_t *)raw;
                self->key_pending_frames = raw_bytes / width;
            }
            if (run > self->key_pending_frames) {
                run = self->key_pending_frames;
            }
            key = self->key_pending;
            self->key_pending += run * self->base.channel_count;
            self->key_pending_frames -= run;
        }
        audioif_dynamics_process_s16_key(&self->config, &self->state,
            &self->buffer[produced * 2], self->pending, key, run);
        self->pending += run * self->base.channel_count;
        self->pending_frames -= run;
        produced += run;
    }
    // A starved chain gets silence rather than a short block: this node sits
    // in the middle of a live graph and never reports itself finished.
    if (produced == 0) {
        memset(self->buffer, 0, sizeof(self->buffer));
        produced = AUDIOIF_DYNAMICS_FRAMES;
    }
    *buffer = (uint8_t *)self->buffer;
    *buffer_length = produced * 2u * self->base.channel_count;
    return GET_BUFFER_MORE_DATA;
}

static void audiodynamics_dynamics_reset_buffer(mp_obj_t self_in,
    bool single_channel_output, uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    audiodynamics_dynamics_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->pending = NULL;
    self->pending_frames = 0;
    self->key_pending = NULL;
    self->key_pending_frames = 0;
    audioif_pump_lock_acquire();
    audioif_dynamics_reset(&self->state);
    audioif_pump_lock_release();
}

// `deinit()` releases what this binding holds and marks the node
// deinitialised, which is what makes every guarded entry point raise
// afterwards -- `audiosample_get_buffer` and `audiosample_reset_buffer` in
// audiocore for the audio path, and the three shared properties. The node
// types audioif ported from CircuitPython have had this since they were
// ported; the ones audioif wrote itself did not, so no class built on them
// could release one and Tier 1's "deinit() releases every node the class
// built" was unmeasurable on a board (audioif#58, #60, #63).
//
// The inline buffers go with the object. What is cleared here is what the
// object holds a *reference* to: the upstream source, so releasing the tail
// of a chain lets the GC reclaim the rest of it, and every borrowed pointer
// into a source's buffer, so nothing dangles.
static mp_obj_t audiodynamics_dynamics_deinit(mp_obj_t self_in) {
    audiodynamics_dynamics_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    audioif_pump_lock_acquire();
    self->source = mp_const_none;
    self->pending = NULL;
    self->pending_frames = 0;
    self->key_source = mp_const_none;
    self->key_pending = NULL;
    self->key_pending_frames = 0;
    audioif_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiodynamics_dynamics_deinit_obj, audiodynamics_dynamics_deinit);

static const mp_rom_map_elem_t audiodynamics_dynamics_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiodynamics_dynamics_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiodynamics_dynamics_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_key), MP_ROM_PTR(&audiodynamics_dynamics_key_obj) },
    { MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&audiodynamics_dynamics_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_gain_reduction_db),
      MP_ROM_PTR(&audiodynamics_dynamics_gain_reduction_db_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiodynamics_dynamics_locals,
    audiodynamics_dynamics_locals_table);

static const audiosample_p_t audiodynamics_dynamics_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = audiodynamics_dynamics_reset_buffer,
    .get_buffer = audiodynamics_dynamics_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiodynamics_dynamics_type,
    MP_QSTR_Dynamics,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiodynamics_dynamics_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiodynamics_dynamics_locals,
    protocol, &audiodynamics_dynamics_proto
    );
