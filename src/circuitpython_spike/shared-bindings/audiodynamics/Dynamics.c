// audiodynamics.Dynamics bindings for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "shared-bindings/audiodynamics/Dynamics.h"
#include "shared-bindings/audiocore/__init__.h"

#include "py/objproperty.h"
#include "shared/runtime/context_manager_helpers.h"
#include "py/runtime.h"

//| class Dynamics:
//|     """An envelope-follower gain computer: compressor, limiter, downward
//|     expander, gate, and transient shaper.
//|
//|     Processes signed 16-bit stereo, and hands out 256 frames at a time. It
//|     sits in an audiosample chain like any other effect, and never reports
//|     itself finished - a starved chain gets silence."""
//|
//|     def __init__(
//|         self,
//|         mode: int = DYN_COMPRESS,
//|         *,
//|         sample_rate: int = 48000,
//|         threshold_db: float = -24.0,
//|         ratio: float = 4.0,
//|         knee_db: float = 6.0,
//|         makeup_db: float = 0.0,
//|         attack_ms: float = 10.0,
//|         release_ms: float = 120.0,
//|         attack_gain_db: float = 0.0,
//|         sustain_gain_db: float = 0.0,
//|         sidechain_hz: float = 0.0,
//|         lookahead_ms: float = 0.0,
//|         true_peak: int = 0,
//|         detector: str = "peak",
//|         rms_ms: float = 10.0,
//|         feedback_detector: bool = False,
//|         relative_threshold: bool = False,
//|         program_attack: bool = False,
//|         sidechain_lp_hz: float = 0.0,
//|         sidechain_poles: int = 1,
//|         key_listen: bool = False,
//|         depth_db: float = 1.0,
//|         hold_ms: float = 0.0,
//|         hysteresis_db: float = 0.0,
//|         transient_fast_attack_ms: float = 1.0,
//|         transient_fast_release_ms: float = 50.0,
//|         transient_slow_attack_ms: float = 25.0,
//|         transient_slow_release_ms: float = 300.0,
//|         slow_hold_ms: float = 0.0,
//|         transient_dual: bool = False,
//|         sustain_fast_attack_ms: float = 1.0,
//|         sustain_fast_release_ms: float = 200.0,
//|         sustain_slow_attack_ms: float = 25.0,
//|         sustain_slow_release_ms: float = 1200.0,
//|     ) -> None:
//|         """Create a dynamics processor in one of the ``DYN_*`` modes.
//|
//|         ``sidechain_hz`` high-passes the detector without touching the
//|         audio, which is how a de-esser is built. ``attack_gain_db`` and
//|         ``sustain_gain_db`` apply to ``DYN_TRANSIENT`` only; the threshold,
//|         ratio and knee apply to the others.
//|
//|         ``lookahead_ms`` holds the audio back while the detector reads
//|         ahead of it, capped at 50 ms. ``true_peak`` is a level: 1 adds a
//|         four-point half-band estimate of the peak between samples, 2 a 4x
//|         polyphase reconstruction of it.
//|
//|         The rest are the effects program's additions and are all
//|         default-off. ``detector="rms"`` averages a mean square over
//|         ``rms_ms`` instead of rectifying a peak. ``feedback_detector``
//|         reads the frame the node last put out rather than this frame's
//|         input, and ``key()`` reads a different stream entirely;
//|         ``key_listen`` puts the detector's signal on the output.
//|         ``sidechain_lp_hz`` closes the top of the key band and
//|         ``sidechain_poles=2`` cascades a second pole through both ends.
//|         ``depth_db`` replaces the expander's -60 dB and the gate's -80 dB
//|         floors - positive means unset, because a depth is an attenuation.
//|         ``hold_ms`` and ``hysteresis_db`` swap the gate's memoryless gain
//|         computer for a closed/attack/hold/decay machine.
//|         ``relative_threshold`` drives the gain computer with the
//|         side-chained level minus the full-band one, and ``program_attack``
//|         scales the attack coefficient by the overshoot. The four
//|         ``transient_*_ms`` times are the shaper's detector constants;
//|         ``slow_hold_ms`` makes the slow one a peak-hold and
//|         ``transient_dual`` runs the four ``sustain_*_ms`` times as a second
//|         pair so both differences apply at once."""
//|         ...

// The options __init__ and set() accept, paired with the shared DSP's enum.
// `sample_rate` is deliberately absent: the millisecond conversions read it,
// so it is applied ahead of this table rather than from it, and keyword order
// stays irrelevant.
typedef struct {
    qstr name;
    audiodsp_dynamics_option_t option;
} dynamics_option_name_t;

static const dynamics_option_name_t dynamics_option_names[] = {
    { MP_QSTR_threshold_db, AUDIODSP_DYNAMICS_OPT_THRESHOLD_DB },
    { MP_QSTR_ratio, AUDIODSP_DYNAMICS_OPT_RATIO },
    { MP_QSTR_knee_db, AUDIODSP_DYNAMICS_OPT_KNEE_DB },
    { MP_QSTR_makeup_db, AUDIODSP_DYNAMICS_OPT_MAKEUP_DB },
    { MP_QSTR_attack_ms, AUDIODSP_DYNAMICS_OPT_ATTACK_MS },
    { MP_QSTR_release_ms, AUDIODSP_DYNAMICS_OPT_RELEASE_MS },
    { MP_QSTR_attack_gain_db, AUDIODSP_DYNAMICS_OPT_ATTACK_GAIN_DB },
    { MP_QSTR_sustain_gain_db, AUDIODSP_DYNAMICS_OPT_SUSTAIN_GAIN_DB },
    { MP_QSTR_sidechain_hz, AUDIODSP_DYNAMICS_OPT_SIDECHAIN_HZ },
    { MP_QSTR_lookahead_ms, AUDIODSP_DYNAMICS_OPT_LOOKAHEAD_MS },
    { MP_QSTR_true_peak, AUDIODSP_DYNAMICS_OPT_TRUE_PEAK },
    // The effects program's additions, every one of them default-off.
    { MP_QSTR_transient_fast_attack_ms,
      AUDIODSP_DYNAMICS_OPT_TRANSIENT_FAST_ATTACK_MS },
    { MP_QSTR_transient_fast_release_ms,
      AUDIODSP_DYNAMICS_OPT_TRANSIENT_FAST_RELEASE_MS },
    { MP_QSTR_transient_slow_attack_ms,
      AUDIODSP_DYNAMICS_OPT_TRANSIENT_SLOW_ATTACK_MS },
    { MP_QSTR_transient_slow_release_ms,
      AUDIODSP_DYNAMICS_OPT_TRANSIENT_SLOW_RELEASE_MS },
    { MP_QSTR_detector, AUDIODSP_DYNAMICS_OPT_DETECTOR },
    { MP_QSTR_rms_ms, AUDIODSP_DYNAMICS_OPT_RMS_MS },
    { MP_QSTR_feedback_detector, AUDIODSP_DYNAMICS_OPT_FEEDBACK_DETECTOR },
    { MP_QSTR_sidechain_lp_hz, AUDIODSP_DYNAMICS_OPT_SIDECHAIN_LP_HZ },
    { MP_QSTR_sidechain_poles, AUDIODSP_DYNAMICS_OPT_SIDECHAIN_POLES },
    { MP_QSTR_key_listen, AUDIODSP_DYNAMICS_OPT_KEY_LISTEN },
    { MP_QSTR_depth_db, AUDIODSP_DYNAMICS_OPT_DEPTH_DB },
    { MP_QSTR_hold_ms, AUDIODSP_DYNAMICS_OPT_HOLD_MS },
    { MP_QSTR_hysteresis_db, AUDIODSP_DYNAMICS_OPT_HYSTERESIS_DB },
    { MP_QSTR_relative_threshold, AUDIODSP_DYNAMICS_OPT_RELATIVE_THRESHOLD },
    { MP_QSTR_program_attack, AUDIODSP_DYNAMICS_OPT_PROGRAM_ATTACK },
    { MP_QSTR_transient_dual, AUDIODSP_DYNAMICS_OPT_TRANSIENT_DUAL },
    { MP_QSTR_sustain_fast_attack_ms,
      AUDIODSP_DYNAMICS_OPT_SUSTAIN_FAST_ATTACK_MS },
    { MP_QSTR_sustain_fast_release_ms,
      AUDIODSP_DYNAMICS_OPT_SUSTAIN_FAST_RELEASE_MS },
    { MP_QSTR_sustain_slow_attack_ms,
      AUDIODSP_DYNAMICS_OPT_SUSTAIN_SLOW_ATTACK_MS },
    { MP_QSTR_sustain_slow_release_ms,
      AUDIODSP_DYNAMICS_OPT_SUSTAIN_SLOW_RELEASE_MS },
    { MP_QSTR_slow_hold_ms, AUDIODSP_DYNAMICS_OPT_SLOW_HOLD_MS },
    { MP_QSTR_gain_smooth_ms, AUDIODSP_DYNAMICS_OPT_GAIN_SMOOTH_MS },
    { MP_QSTR_feedback_gain_corrected,
      AUDIODSP_DYNAMICS_OPT_FEEDBACK_GAIN_CORRECTED },
};

// The lookahead buffer is allocated only once someone asks for one, and only
// ever grows: `set(lookahead_ms=...)` mid-stream is a live gesture, and
// shrinking would mean freeing memory the DSP is reading out of.
static void dynamics_ensure_lookahead(audiodynamics_dynamics_obj_t *self) {
    const uint32_t wanted = audiodsp_dynamics_lookahead_frames(&self->config);
    if (wanted == 0 || wanted <= self->state.lookahead_capacity) {
        return;
    }
    int16_t *buffer = m_malloc((size_t)wanted * self->base.channel_count *
        sizeof(int16_t));
    audiodsp_dynamics_set_lookahead(&self->state, buffer, wanted);
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
            audiodsp_dynamics_set_channel_count(&self->config, &self->state,
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
                value = (float)AUDIODSP_DYNAMICS_DETECT_RMS;
            } else if (word == MP_QSTR_peak) {
                value = (float)AUDIODSP_DYNAMICS_DETECT_PEAK;
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
                audiodsp_dynamics_configure(&self->config,
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
                                 : AUDIODSP_DYNAMICS_COMPRESS;
    audiodsp_dynamics_config_init(&self->config, mode, self->base.sample_rate);
    audiodsp_dynamics_state_init(&self->state);

    mp_map_t kw_map;
    mp_map_init_fixed_table(&kw_map, n_kw, all_args + n_args);
    dynamics_apply_kwargs(self, &kw_map);
    audiodsp_dynamics_config_finish(&self->config);
    return MP_OBJ_FROM_PTR(self);
}

//|     def play(self, sample: circuitpython_typing.AudioSample) -> None:
//|         """Take audio from ``sample``. Signed 16-bit stereo only."""
//|         ...
static mp_obj_t audiodynamics_dynamics_play(mp_obj_t self_in,
    mp_obj_t sample) {
    audiodynamics_dynamics_obj_t *self = MP_OBJ_TO_PTR(self_in);
    (void)audiosample_check(sample);
    self->source = sample;
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodynamics_dynamics_play_obj,
    audiodynamics_dynamics_play);

//|     def key(self, sample: circuitpython_typing.AudioSample | None) -> None:
//|         """Feed the detector from ``sample`` instead of from the audio.
//|
//|         The gain still lands on whatever ``play()`` is playing. ``None``
//|         goes back to reading the audio; a key that runs dry starves the
//|         node the same way an absent source does."""
//|         ...
static mp_obj_t audiodynamics_dynamics_key(mp_obj_t self_in,
    mp_obj_t sample) {
    audiodynamics_dynamics_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (sample == mp_const_none) {
        self->key_source = MP_OBJ_NULL;
    } else {
        (void)audiosample_check(sample);
        self->key_source = sample;
    }
    self->key_pending = NULL;
    self->key_pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiodynamics_dynamics_key_obj,
    audiodynamics_dynamics_key);

//|     def set(self, **options: float) -> None:
//|         """Change any of the constructor's options mid-stream. The detector
//|         keeps its memory."""
//|         ...
static mp_obj_t audiodynamics_dynamics_set(size_t n_args,
    const mp_obj_t *args, mp_map_t *kw_args) {
    audiodynamics_dynamics_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    (void)n_args;
    dynamics_apply_kwargs(self, kw_args);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(audiodynamics_dynamics_set_obj, 1,
    audiodynamics_dynamics_set);

//|     def gain_reduction_db(self) -> float:
//|         """Gain applied to the most recent frame, in dB (negative = cut)."""
//|         ...
//|
//|
static mp_obj_t audiodynamics_dynamics_gain_reduction_db(mp_obj_t self_in) {
    audiodynamics_dynamics_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_float((mp_float_t)self->state.gain_reduction_db);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodynamics_dynamics_gain_reduction_db_obj,
    audiodynamics_dynamics_gain_reduction_db);

// `deinit()` releases what this binding holds and marks the node
// deinitialised, so the guarded getters raise afterwards. The MicroPython
// binding of this same type gained it on 2026-09-09 (audiodsp#58, #60, #63) and
// this copy did not, which is audiodsp#75: the two bindings are hand-written and
// nothing held them to each other. Same fields, same order, deliberately.
static mp_obj_t audiodynamics_dynamics_deinit(mp_obj_t self_in) {
    audiodynamics_dynamics_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    self->source = mp_const_none;
    self->pending = NULL;
    self->pending_frames = 0;
    self->key_source = mp_const_none;
    self->key_pending = NULL;
    self->key_pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiodynamics_dynamics_deinit_obj, audiodynamics_dynamics_deinit);

static const mp_rom_map_elem_t audiodynamics_dynamics_locals_dict_table[] = {
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
static MP_DEFINE_CONST_DICT(audiodynamics_dynamics_locals_dict,
    audiodynamics_dynamics_locals_dict_table);

static const audiosample_p_t audiodynamics_dynamics_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)
        audiodynamics_dynamics_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)
        audiodynamics_dynamics_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiodynamics_dynamics_type,
    MP_QSTR_Dynamics,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiodynamics_dynamics_make_new,
    locals_dict, &audiodynamics_dynamics_locals_dict,
    protocol, &audiodynamics_dynamics_proto
    );
