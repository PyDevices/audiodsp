// audioecho.FeedbackDelay bindings for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <string.h>

#include "shared-bindings/audioecho/FeedbackDelay.h"
#include "shared-bindings/audiocore/__init__.h"

#include "py/objproperty.h"
#include "shared/runtime/context_manager_helpers.h"
#include "py/runtime.h"

//| class FeedbackDelay:
//|     """A delay line whose feedback path carries a low-pass, a high-pass, a
//|     soft-clip, per-sample delay modulation and a cross-feed between the
//|     channels.
//|
//|     Processes signed 16-bit stereo, and hands out 256 frames at a time. It
//|     sits in an audiosample chain like any other effect, and never reports
//|     itself finished - a starved chain gets silence. The line only advances
//|     with frames that arrive, so the repeats stop with the source rather
//|     than ringing on, the same way `audiodelays.Echo` behaves."""
//|
//|     def __init__(
//|         self,
//|         *,
//|         sample_rate: int = 48000,
//|         max_delay_ms: float = 250.0,
//|         delay_ms: float = ...,
//|         feedback: float = 0.4,
//|         mix: float = 0.3,
//|         damping_hz: float = 0.0,
//|         cut_hz: float = 0.0,
//|         wow_hz: float = 0.0,
//|         wow_depth_ms: float = 0.0,
//|         cross_feed: float = 0.0,
//|         loop_drive: float = 0.0,
//|         input_pan: float = 0.0,
//|         wow_shape: circuitpython_typing.ReadableBuffer | None = None,
//|         delay_slew: float = 0.0,
//|         wow_am_depth: float = 0.0,
//|         loop_semitones: float = 0.0,
//|         loop_window_ms: float = 25.0,
//|     ) -> None:
//|         """Create a feedback delay. ``max_delay_ms`` sizes the line and
//|         cannot change afterwards; ``delay_ms`` defaults to half of it.
//|
//|         ``damping_hz`` and ``cut_hz`` are one-pole filters *inside* the
//|         loop, so every repeat loses a little more top and bottom than the
//|         last. ``loop_drive`` softens each pass. ``wow_hz`` and
//|         ``wow_depth_ms`` modulate the delay per sample, which is where the
//|         doppler in a tape machine's wow comes from. ``cross_feed`` sends
//|         each channel's repeats into the other channel's line, and
//|         ``input_pan`` steers the input into one line only: hard over with
//|         full cross-feed is a real ping-pong.
//|
//|         The last five are off at their defaults, and a delay built without
//|         them renders exactly what it rendered before they existed.
//|
//|         ``wow_shape`` replaces the built-in sine with one period of your
//|         own, as ``int16`` Q15 samples whose count is a power of two from 2
//|         to 4096; ``None`` puts the sine back. A bucket brigade's delay is
//|         its line length over its clock, so a triangle on the clock is a
//|         *reciprocal* on the delay and no sine can be it. The table is
//|         borrowed, not copied - keep a reference to it.
//|
//|         ``delay_slew`` walks the read head to a new ``delay_ms`` instead of
//|         jumping to it, in delay-seconds per second (the same number at any
//|         sample rate). 0 is the jump. 1.0 is the read head standing still
//|         while the write head runs - an octave up for exactly as long as the
//|         move lasts, with no discontinuity at either end.
//|
//|         ``wow_am_depth`` (0..1) puts the wow oscillator on the wet gain as
//|         well as on the delay: it dips to ``1 - depth`` and returns to
//|         unity, never boosts, and never touches the feedback path.
//|
//|         ``loop_semitones`` (-24..+24) pitch-shifts the line read *inside*
//|         the loop, so every pass rises again - which is what a shimmer is.
//|         ``loop_window_ms`` is the crossfade window it reads through. A
//|         shifted loop repeats half a window later than an unshifted one
//|         (the two taps' gains sum to one, so the mean read is ``delay_ms +
//|         loop_window_ms / 2`` throughout), so switching it on or off
//|         mid-stream steps the read by that much and nothing smooths it.
//|         Set it when you build the node, or between takes."""
//|         ...

// The options __init__ and set() accept, paired with the shared DSP's enum.
// `sample_rate` and `max_delay_ms` are deliberately absent: they size the
// line, so they are applied ahead of this table rather than from it, and
// keyword order stays irrelevant.
typedef struct {
    qstr name;
    audiodsp_feedback_delay_option_t option;
} feedback_delay_option_name_t;

static const feedback_delay_option_name_t feedback_delay_option_names[] = {
    { MP_QSTR_delay_ms, AUDIODSP_FEEDBACK_DELAY_OPT_DELAY_MS },
    { MP_QSTR_feedback, AUDIODSP_FEEDBACK_DELAY_OPT_FEEDBACK },
    { MP_QSTR_mix, AUDIODSP_FEEDBACK_DELAY_OPT_MIX },
    { MP_QSTR_damping_hz, AUDIODSP_FEEDBACK_DELAY_OPT_DAMPING_HZ },
    { MP_QSTR_cut_hz, AUDIODSP_FEEDBACK_DELAY_OPT_CUT_HZ },
    { MP_QSTR_wow_hz, AUDIODSP_FEEDBACK_DELAY_OPT_WOW_HZ },
    { MP_QSTR_wow_depth_ms, AUDIODSP_FEEDBACK_DELAY_OPT_WOW_DEPTH_MS },
    { MP_QSTR_cross_feed, AUDIODSP_FEEDBACK_DELAY_OPT_CROSS_FEED },
    { MP_QSTR_loop_drive, AUDIODSP_FEEDBACK_DELAY_OPT_LOOP_DRIVE },
    { MP_QSTR_input_pan, AUDIODSP_FEEDBACK_DELAY_OPT_INPUT_PAN },
    { MP_QSTR_delay_slew, AUDIODSP_FEEDBACK_DELAY_OPT_DELAY_SLEW },
    { MP_QSTR_wow_am_depth, AUDIODSP_FEEDBACK_DELAY_OPT_WOW_AM_DEPTH },
    { MP_QSTR_loop_semitones, AUDIODSP_FEEDBACK_DELAY_OPT_LOOP_SEMITONES },
    { MP_QSTR_loop_window_ms, AUDIODSP_FEEDBACK_DELAY_OPT_LOOP_WINDOW_MS },
};

// `wow_shape` is a buffer, not a number, so it is handled beside
// `sample_rate` and `max_delay_ms` rather than from the table above. The
// object is kept on the instance because the config only *borrows* the
// samples: a table the collector took would be read as whatever landed on it
// next.
static void feedback_delay_set_shape(audioecho_feedback_delay_obj_t *self,
    mp_obj_t value) {
    if (value == mp_const_none) {
        audiodsp_feedback_delay_set_wow_shape(&self->config, NULL, 0);
        self->wow_shape = MP_OBJ_NULL;
        return;
    }
    mp_buffer_info_t info;
    mp_get_buffer_raise(value, &info, MP_BUFFER_READ);
    if (info.len % sizeof(int16_t) != 0 ||
        !audiodsp_feedback_delay_set_wow_shape(&self->config,
            (const int16_t *)info.buf,
            (uint32_t)(info.len / sizeof(int16_t)))) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "wow_shape must be 2 to 4096 int16 samples, a power of two"));
    }
    self->wow_shape = value;
}

static void feedback_delay_apply_kwargs(audioecho_feedback_delay_obj_t *self,
    const mp_map_t *kw) {
    for (size_t i = 0; i < kw->alloc; ++i) {
        if (!mp_map_slot_is_filled(kw, i)) {
            continue;
        }
        qstr name = mp_obj_str_get_qstr(kw->table[i].key);
        if (name == MP_QSTR_sample_rate || name == MP_QSTR_max_delay_ms ||
            name == MP_QSTR_channel_count) {
            continue;
        }
        if (name == MP_QSTR_wow_shape) {
            feedback_delay_set_shape(self, kw->table[i].value);
            continue;
        }
        float value = (float)mp_obj_get_float(kw->table[i].value);
        bool known = false;
        for (size_t option = 0;
             option < MP_ARRAY_SIZE(feedback_delay_option_names); ++option) {
            if (feedback_delay_option_names[option].name == name) {
                audiodsp_feedback_delay_configure(&self->config,
                    feedback_delay_option_names[option].option, value);
                known = true;
                break;
            }
        }
        if (!known) {
            mp_raise_msg_varg(&mp_type_TypeError,
                MP_ERROR_TEXT("unknown FeedbackDelay option '%q'"), name);
        }
    }
}

static mp_obj_t audioecho_feedback_delay_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, true);
    mp_map_t kw_map;
    mp_map_init_fixed_table(&kw_map, n_kw, all_args + n_args);

    uint32_t sample_rate = 48000;
    uint32_t channel_count = 2;
    mp_float_t max_delay_ms = 250;
    for (size_t i = 0; i < kw_map.alloc; ++i) {
        if (!mp_map_slot_is_filled(&kw_map, i)) {
            continue;
        }
        qstr name = mp_obj_str_get_qstr(kw_map.table[i].key);
        if (name == MP_QSTR_sample_rate) {
            sample_rate = (uint32_t)mp_obj_get_int(kw_map.table[i].value);
        } else if (name == MP_QSTR_max_delay_ms) {
            max_delay_ms = mp_obj_get_float(kw_map.table[i].value);
        } else if (name == MP_QSTR_channel_count) {
            channel_count = (uint32_t)mp_obj_get_int(kw_map.table[i].value);
        }
    }
    if (max_delay_ms <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("max_delay_ms must be positive"));
    }
    if (channel_count < 1u || channel_count > 2u) {
        mp_raise_ValueError(MP_ERROR_TEXT("channel_count must be 1 or 2"));
    }

    audioecho_feedback_delay_obj_t *self =
        mp_obj_malloc(audioecho_feedback_delay_obj_t, type);
    self->base.sample_rate = sample_rate;
    self->base.max_buffer_length = sizeof(self->buffer);
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)channel_count;
    self->base.samples_signed = 1;
    self->base.single_buffer = false;
    self->source = MP_OBJ_NULL;
    self->wow_shape = MP_OBJ_NULL;
    self->pending = NULL;
    self->pending_frames = 0;

    uint32_t line_frames =
        (uint32_t)((mp_float_t)sample_rate * max_delay_ms / 1000);
    if (line_frames < 2) {
        line_frames = 2;
    }
    audiodsp_feedback_delay_config_init(&self->config, sample_rate,
        line_frames);
    audiodsp_feedback_delay_set_channel_count(&self->config, channel_count);
    int16_t *line = m_malloc((size_t)line_frames * 2u * sizeof(int16_t));
    memset(line, 0, (size_t)line_frames * 2u * sizeof(int16_t));
    audiodsp_feedback_delay_state_init(&self->state, line);
    // The default delay is half the line rather than all of it, so a caller
    // who sizes the line and says nothing else still hears repeats.
    audiodsp_feedback_delay_configure(&self->config,
        AUDIODSP_FEEDBACK_DELAY_OPT_DELAY_MS, (float)max_delay_ms * 0.5f);

    feedback_delay_apply_kwargs(self, &kw_map);
    audiodsp_feedback_delay_config_finish(&self->config);
    return MP_OBJ_FROM_PTR(self);
}

//|     def play(self, sample: circuitpython_typing.AudioSample) -> None:
//|         """Set the source the delay reads from."""
//|         ...
static mp_obj_t audioecho_feedback_delay_play(mp_obj_t self_in,
    mp_obj_t sample) {
    audioecho_feedback_delay_obj_t *self = MP_OBJ_TO_PTR(self_in);
    (void)audiosample_check(sample);
    self->source = sample;
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audioecho_feedback_delay_play_obj,
    audioecho_feedback_delay_play);

//|     def set(self, **options: float) -> None:
//|         """Change settings mid-stream. The line and the filters keep their
//|         contents; only what the loop does to them changes."""
//|         ...
static mp_obj_t audioecho_feedback_delay_set(size_t n_args,
    const mp_obj_t *args, mp_map_t *kw_args) {
    audioecho_feedback_delay_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    (void)n_args;
    feedback_delay_apply_kwargs(self, kw_args);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(audioecho_feedback_delay_set_obj, 1,
    audioecho_feedback_delay_set);

//|     def clear(self) -> None:
//|         """Empty the line and the loop filters."""
//|         ...
//|
//|
static mp_obj_t audioecho_feedback_delay_clear(mp_obj_t self_in) {
    audioecho_feedback_delay_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_feedback_delay_reset(&self->state, &self->config);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audioecho_feedback_delay_clear_obj,
    audioecho_feedback_delay_clear);

// `deinit()` releases what this binding holds and marks the node
// deinitialised, so the guarded getters raise afterwards. The MicroPython
// binding of this same type gained it on 2026-09-09 (audiodsp#58, #60, #63) and
// this copy did not, which is audiodsp#75: the two bindings are hand-written and
// nothing held them to each other. Same fields, same order, deliberately.
static mp_obj_t audioecho_feedback_delay_deinit(mp_obj_t self_in) {
    audioecho_feedback_delay_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    self->source = mp_const_none;
    self->wow_shape = mp_const_none;
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audioecho_feedback_delay_deinit_obj, audioecho_feedback_delay_deinit);

static const mp_rom_map_elem_t
audioecho_feedback_delay_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audioecho_feedback_delay_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play),
      MP_ROM_PTR(&audioecho_feedback_delay_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_set),
      MP_ROM_PTR(&audioecho_feedback_delay_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear),
      MP_ROM_PTR(&audioecho_feedback_delay_clear_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audioecho_feedback_delay_locals_dict,
    audioecho_feedback_delay_locals_dict_table);

static const audiosample_p_t audioecho_feedback_delay_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)
        audioecho_feedback_delay_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)
        audioecho_feedback_delay_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audioecho_feedback_delay_type,
    MP_QSTR_FeedbackDelay,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audioecho_feedback_delay_make_new,
    locals_dict, &audioecho_feedback_delay_locals_dict,
    protocol, &audioecho_feedback_delay_proto
    );
