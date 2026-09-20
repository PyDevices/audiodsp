// audioecho.FeedbackDelay. See FeedbackDelay.h for provenance.
// SPDX-License-Identifier: MIT

#include "audioecho/FeedbackDelay.h"

#include <string.h>

#include "cp_compat/context_manager_helpers.h"
#include "py/runtime.h"
#include "shared/audioif_pump_lock.h"

// The options `FeedbackDelay(...)` and `set(...)` accept, paired with the
// shared DSP's enum. `sample_rate` and `max_delay_ms` are deliberately
// absent: they size the line, so they are applied ahead of this table rather
// than from it, and keyword order stays irrelevant.
typedef struct {
    qstr name;
    audioif_feedback_delay_option_t option;
} feedback_delay_option_name_t;

static const feedback_delay_option_name_t feedback_delay_option_names[] = {
    { MP_QSTR_delay_ms, AUDIOIF_FEEDBACK_DELAY_OPT_DELAY_MS },
    { MP_QSTR_feedback, AUDIOIF_FEEDBACK_DELAY_OPT_FEEDBACK },
    { MP_QSTR_mix, AUDIOIF_FEEDBACK_DELAY_OPT_MIX },
    { MP_QSTR_damping_hz, AUDIOIF_FEEDBACK_DELAY_OPT_DAMPING_HZ },
    { MP_QSTR_cut_hz, AUDIOIF_FEEDBACK_DELAY_OPT_CUT_HZ },
    { MP_QSTR_wow_hz, AUDIOIF_FEEDBACK_DELAY_OPT_WOW_HZ },
    { MP_QSTR_wow_depth_ms, AUDIOIF_FEEDBACK_DELAY_OPT_WOW_DEPTH_MS },
    { MP_QSTR_cross_feed, AUDIOIF_FEEDBACK_DELAY_OPT_CROSS_FEED },
    { MP_QSTR_loop_drive, AUDIOIF_FEEDBACK_DELAY_OPT_LOOP_DRIVE },
    { MP_QSTR_input_pan, AUDIOIF_FEEDBACK_DELAY_OPT_INPUT_PAN },
    { MP_QSTR_delay_slew, AUDIOIF_FEEDBACK_DELAY_OPT_DELAY_SLEW },
    { MP_QSTR_wow_am_depth, AUDIOIF_FEEDBACK_DELAY_OPT_WOW_AM_DEPTH },
    { MP_QSTR_loop_semitones, AUDIOIF_FEEDBACK_DELAY_OPT_LOOP_SEMITONES },
    { MP_QSTR_loop_window_ms, AUDIOIF_FEEDBACK_DELAY_OPT_LOOP_WINDOW_MS },
};

// `wow_shape` is a buffer, not a number, so it is handled beside
// `sample_rate` and `max_delay_ms` rather than from the table above. The
// object is kept on the instance because the config only *borrows* the
// samples: a table the collector took would be read as whatever landed on it
// next.
static void feedback_delay_set_shape(audioecho_feedback_delay_obj_t *self,
    mp_obj_t value) {
    if (value == mp_const_none) {
        audioif_feedback_delay_set_wow_shape(&self->config, NULL, 0);
        self->wow_shape = MP_OBJ_NULL;
        return;
    }
    mp_buffer_info_t info;
    mp_get_buffer_raise(value, &info, MP_BUFFER_READ);
    if (info.len % sizeof(int16_t) != 0 ||
        !audioif_feedback_delay_set_wow_shape(&self->config,
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
                audioif_feedback_delay_configure(&self->config,
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
    audioif_feedback_delay_config_init(&self->config, sample_rate,
        line_frames);
    audioif_feedback_delay_set_channel_count(&self->config, channel_count);
    int16_t *line = m_malloc((size_t)line_frames * 2u * sizeof(int16_t));
    memset(line, 0, (size_t)line_frames * 2u * sizeof(int16_t));
    audioif_feedback_delay_state_init(&self->state, line);
    // The default delay is half the line rather than all of it, so a caller
    // who sizes the line and says nothing else still hears repeats.
    audioif_feedback_delay_configure(&self->config,
        AUDIOIF_FEEDBACK_DELAY_OPT_DELAY_MS, (float)max_delay_ms * 0.5f);

    feedback_delay_apply_kwargs(self, &kw_map);
    audioif_feedback_delay_config_finish(&self->config);
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audioecho_feedback_delay_play(mp_obj_t self_in,
    mp_obj_t sample) {
    audioecho_feedback_delay_obj_t *self = MP_OBJ_TO_PTR(self_in);
    (void)audiosample_check(sample);
    audioif_pump_lock_acquire();
    self->source = sample;
    self->pending = NULL;
    self->pending_frames = 0;
    audioif_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audioecho_feedback_delay_play_obj,
    audioecho_feedback_delay_play);

static mp_obj_t audioecho_feedback_delay_set(size_t n_args,
    const mp_obj_t *args, mp_map_t *kw_args) {
    audioecho_feedback_delay_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    (void)n_args;
    feedback_delay_apply_kwargs(self, kw_args);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audioecho_feedback_delay_set_obj, 1,
    audioecho_feedback_delay_set);

static mp_obj_t audioecho_feedback_delay_clear(mp_obj_t self_in) {
    audioecho_feedback_delay_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audioif_pump_lock_acquire();
    audioif_feedback_delay_reset(&self->state, &self->config);
    audioif_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioecho_feedback_delay_clear_obj,
    audioecho_feedback_delay_clear);

static audioio_get_buffer_result_t audioecho_feedback_delay_get_buffer(
    mp_obj_t self_in, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    audioecho_feedback_delay_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint32_t produced = 0;
    while (produced < AUDIOIF_FEEDBACK_DELAY_FRAMES) {
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
        uint32_t run = AUDIOIF_FEEDBACK_DELAY_FRAMES - produced;
        if (run > self->pending_frames) {
            run = self->pending_frames;
        }
        // `* channel_count`, not `* 2`: the DSP writes `channel_count`
        // samples per frame, so a mono node advanced this destination by
        // twice what it had written and interleaved its own output with the
        // gap it left (audioif#54). Harmless on stereo by coincidence, and
        // harmless on mono while a source hands out at least
        // AUDIOIF_FEEDBACK_DELAY_FRAMES per pull, because `produced` is then
        // 0 for the only chunk. Both twins already read it this way -
        // `circuitpython_spike/shared-module/audioecho/FeedbackDelay.c` and
        // the CPython `audioecho.py`, which appends contiguously.
        audioif_feedback_delay_process_s16(&self->config, &self->state,
            &self->buffer[produced * self->base.channel_count],
            self->pending, run);
        self->pending += run * self->base.channel_count;
        self->pending_frames -= run;
        produced += run;
    }
    // A starved chain gets silence rather than a short block: this node sits
    // in the middle of a live graph and never reports itself finished. Note
    // that the tail stops with the source -- the line is only advanced by
    // frames that arrive, so repeats do not ring on into silence. That
    // matches audiodelays.Echo, whose users expect it.
    if (produced == 0) {
        memset(self->buffer, 0, sizeof(self->buffer));
        produced = AUDIOIF_FEEDBACK_DELAY_FRAMES;
    }
    *buffer = (uint8_t *)self->buffer;
    *buffer_length = produced * 2u * self->base.channel_count;
    return GET_BUFFER_MORE_DATA;
}

static void audioecho_feedback_delay_reset_buffer(mp_obj_t self_in,
    bool single_channel_output, uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    audioecho_feedback_delay_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->pending = NULL;
    self->pending_frames = 0;
    // Unlike audiodynamics, everything goes. A delay's whole state is
    // audible: a chain restarted with the old repeats still in the line
    // plays the previous take over the new one.
    audioif_feedback_delay_reset(&self->state, &self->config);
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
static mp_obj_t audioecho_feedback_delay_deinit(mp_obj_t self_in) {
    audioecho_feedback_delay_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    audioif_pump_lock_acquire();
    self->source = mp_const_none;
    self->wow_shape = mp_const_none;
    self->pending = NULL;
    self->pending_frames = 0;
    audioif_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioecho_feedback_delay_deinit_obj, audioecho_feedback_delay_deinit);

static const mp_rom_map_elem_t audioecho_feedback_delay_locals_table[] = {
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
static MP_DEFINE_CONST_DICT(audioecho_feedback_delay_locals,
    audioecho_feedback_delay_locals_table);

static const audiosample_p_t audioecho_feedback_delay_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = audioecho_feedback_delay_reset_buffer,
    .get_buffer = audioecho_feedback_delay_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audioecho_feedback_delay_type,
    MP_QSTR_FeedbackDelay,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audioecho_feedback_delay_make_new,
    attr, cp_compat_attr,
    locals_dict, &audioecho_feedback_delay_locals,
    protocol, &audioecho_feedback_delay_proto
    );
