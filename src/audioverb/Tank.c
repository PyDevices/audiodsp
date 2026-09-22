// audioverb.Tank. See Tank.h for provenance.
// SPDX-License-Identifier: MIT

#include "audioverb/Tank.h"

#include <string.h>

#include "cp_compat/context_manager_helpers.h"
#include "py/runtime.h"
#include "shared/audiodsp_pump_lock.h"

// The options `Tank(...)` and `set(...)` accept, paired with the shared DSP's
// enum. `sample_rate`, `channel_count`, `max_predelay_ms`, `delays` and `taps`
// are deliberately absent: they size the allocation and cut the topology, so
// they are applied ahead of this table rather than from it, and keyword order
// stays irrelevant.
typedef struct {
    qstr name;
    audiodsp_tank_option_t option;
} tank_option_name_t;

static const tank_option_name_t tank_option_names[] = {
    { MP_QSTR_decay, AUDIODSP_TANK_OPT_DECAY },
    { MP_QSTR_diffusion, AUDIODSP_TANK_OPT_DIFFUSION },
    { MP_QSTR_damping_hz, AUDIODSP_TANK_OPT_DAMPING_HZ },
    { MP_QSTR_bandwidth_hz, AUDIODSP_TANK_OPT_BANDWIDTH_HZ },
    { MP_QSTR_low_cut_hz, AUDIODSP_TANK_OPT_LOW_CUT_HZ },
    { MP_QSTR_predelay_ms, AUDIODSP_TANK_OPT_PREDELAY_MS },
    { MP_QSTR_mod_depth_ms, AUDIODSP_TANK_OPT_MOD_DEPTH_MS },
    { MP_QSTR_mod_rate_hz, AUDIODSP_TANK_OPT_MOD_RATE_HZ },
    { MP_QSTR_drive, AUDIODSP_TANK_OPT_DRIVE },
    { MP_QSTR_width, AUDIODSP_TANK_OPT_WIDTH },
    { MP_QSTR_tone_db, AUDIODSP_TANK_OPT_TONE_DB },
    { MP_QSTR_mix, AUDIODSP_TANK_OPT_MIX },
};

// The five keywords that are not options, so both the constructor and the
// option loop can recognise them without repeating the list.
static bool tank_is_shape_keyword(qstr name) {
    return name == MP_QSTR_sample_rate || name == MP_QSTR_channel_count ||
        name == MP_QSTR_max_predelay_ms || name == MP_QSTR_delays ||
        name == MP_QSTR_taps;
}

static void tank_raise_status(audiodsp_tank_status_t status) {
    switch (status) {
        case AUDIODSP_TANK_OK:
            return;
        case AUDIODSP_TANK_ERR_COUNT:
            mp_raise_ValueError(MP_ERROR_TEXT(
                "delays needs 12 line lengths; taps needs 4 values per tap"));
        case AUDIODSP_TANK_ERR_LENGTH:
            mp_raise_ValueError(MP_ERROR_TEXT("every line needs 4 frames"));
        case AUDIODSP_TANK_ERR_TOTAL:
            mp_raise_ValueError(MP_ERROR_TEXT("the lines do not fit"));
        case AUDIODSP_TANK_ERR_CHANNEL:
            mp_raise_ValueError(MP_ERROR_TEXT("a tap channel is not 0 or 1"));
        case AUDIODSP_TANK_ERR_LINE:
            mp_raise_ValueError(MP_ERROR_TEXT("a tap line is not 0..11"));
        case AUDIODSP_TANK_ERR_OFFSET:
            mp_raise_ValueError(MP_ERROR_TEXT("a tap is past its line"));
    }
}

static uint32_t tank_read_floats(mp_obj_t sequence, float *out, uint32_t max) {
    mp_obj_iter_buf_t iter_buf;
    mp_obj_t iterable = mp_getiter(sequence, &iter_buf);
    mp_obj_t item;
    uint32_t count = 0;
    while ((item = mp_iternext(iterable)) != MP_OBJ_STOP_ITERATION) {
        if (count >= max) {
            mp_raise_ValueError(MP_ERROR_TEXT("too many values"));
        }
        out[count++] = (float)mp_obj_get_float(item);
    }
    return count;
}

static void tank_apply_kwargs(audioverb_tank_obj_t *self, const mp_map_t *kw) {
    for (size_t i = 0; i < kw->alloc; ++i) {
        if (!mp_map_slot_is_filled(kw, i)) {
            continue;
        }
        qstr name = mp_obj_str_get_qstr(kw->table[i].key);
        if (tank_is_shape_keyword(name)) {
            continue;
        }
        float value = (float)mp_obj_get_float(kw->table[i].value);
        bool known = false;
        for (size_t option = 0; option < MP_ARRAY_SIZE(tank_option_names);
             ++option) {
            if (tank_option_names[option].name == name) {
                audiodsp_tank_configure(&self->config,
                    tank_option_names[option].option, value);
                known = true;
                break;
            }
        }
        if (!known) {
            mp_raise_msg_varg(&mp_type_TypeError,
                MP_ERROR_TEXT("unknown Tank option '%q'"), name);
        }
    }
}

static mp_obj_t audioverb_tank_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, true);
    mp_map_t kw_map;
    mp_map_init_fixed_table(&kw_map, n_kw, all_args + n_args);

    uint32_t sample_rate = 48000;
    uint32_t channel_count = 2;
    mp_float_t max_predelay_ms = 200;
    mp_obj_t delays = MP_OBJ_NULL;
    mp_obj_t taps = MP_OBJ_NULL;
    for (size_t i = 0; i < kw_map.alloc; ++i) {
        if (!mp_map_slot_is_filled(&kw_map, i)) {
            continue;
        }
        qstr name = mp_obj_str_get_qstr(kw_map.table[i].key);
        if (name == MP_QSTR_sample_rate) {
            sample_rate = (uint32_t)mp_obj_get_int(kw_map.table[i].value);
        } else if (name == MP_QSTR_channel_count) {
            channel_count = (uint32_t)mp_obj_get_int(kw_map.table[i].value);
        } else if (name == MP_QSTR_max_predelay_ms) {
            max_predelay_ms = mp_obj_get_float(kw_map.table[i].value);
        } else if (name == MP_QSTR_delays) {
            delays = kw_map.table[i].value;
        } else if (name == MP_QSTR_taps) {
            taps = kw_map.table[i].value;
        }
    }
    if (sample_rate < 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("sample_rate must be at least 1"));
    }
    if (max_predelay_ms < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("max_predelay_ms must not be negative"));
    }
    if (channel_count < 1u || channel_count > 2u) {
        mp_raise_ValueError(MP_ERROR_TEXT("channel_count must be 1 or 2"));
    }

    audioverb_tank_obj_t *self = mp_obj_malloc(audioverb_tank_obj_t, type);
    self->base.sample_rate = sample_rate;
    self->base.max_buffer_length = sizeof(self->buffer);
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)channel_count;
    self->base.samples_signed = 1;
    self->base.single_buffer = false;
    self->source = MP_OBJ_NULL;
    self->pending = NULL;
    self->pending_frames = 0;

    audiodsp_tank_config_init(&self->config, sample_rate,
        (float)max_predelay_ms);
    audiodsp_tank_set_channel_count(&self->config, channel_count);
    // The topology first: both tables size the allocation, so neither can be
    // changed once the lines exist.
    if (delays != MP_OBJ_NULL) {
        float values[AUDIODSP_TANK_LINES];
        uint32_t count = tank_read_floats(delays, values, AUDIODSP_TANK_LINES);
        uint32_t frames[AUDIODSP_TANK_LINES];
        for (uint32_t line = 0; line < count; ++line) {
            frames[line] = values[line] < 0.0f ? 0u : (uint32_t)values[line];
        }
        tank_raise_status(
            audiodsp_tank_set_delays(&self->config, frames, count));
    }
    if (taps != MP_OBJ_NULL) {
        float values[AUDIODSP_TANK_MAX_TAPS * 4u];
        uint32_t count =
            tank_read_floats(taps, values, AUDIODSP_TANK_MAX_TAPS * 4u);
        tank_raise_status(audiodsp_tank_set_taps(&self->config, values, count));
    }

    uint32_t samples = audiodsp_tank_buffer_samples(&self->config);
    int16_t *lines = m_malloc((size_t)samples * sizeof(int16_t));
    memset(lines, 0, (size_t)samples * sizeof(int16_t));
    audiodsp_tank_state_init(&self->state, &self->config, lines);

    tank_apply_kwargs(self, &kw_map);
    audiodsp_tank_config_finish(&self->config);
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audioverb_tank_play(mp_obj_t self_in, mp_obj_t sample) {
    audioverb_tank_obj_t *self = MP_OBJ_TO_PTR(self_in);
    (void)audiosample_check(sample);
    audiodsp_pump_lock_acquire();
    self->source = sample;
    self->pending = NULL;
    self->pending_frames = 0;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audioverb_tank_play_obj, audioverb_tank_play);

static mp_obj_t audioverb_tank_set(size_t n_args, const mp_obj_t *args,
    mp_map_t *kw_args) {
    audioverb_tank_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    (void)n_args;
    for (size_t i = 0; i < kw_args->alloc; ++i) {
        if (!mp_map_slot_is_filled(kw_args, i)) {
            continue;
        }
        qstr name = mp_obj_str_get_qstr(kw_args->table[i].key);
        if (tank_is_shape_keyword(name)) {
            mp_raise_msg_varg(&mp_type_TypeError,
                MP_ERROR_TEXT("'%q' is fixed at construction"), name);
        }
    }
    tank_apply_kwargs(self, kw_args);
    audiodsp_pump_lock_acquire();
    audiodsp_tank_config_finish(&self->config);
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audioverb_tank_set_obj, 1,
    audioverb_tank_set);

static mp_obj_t audioverb_tank_clear(mp_obj_t self_in) {
    audioverb_tank_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_acquire();
    audiodsp_tank_reset(&self->state, &self->config);
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioverb_tank_clear_obj,
    audioverb_tank_clear);

static audioio_get_buffer_result_t audioverb_tank_get_buffer(mp_obj_t self_in,
    bool single_channel_output, uint8_t channel, uint8_t **buffer,
    uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    audioverb_tank_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint32_t produced = 0;
    while (produced < AUDIODSP_TANK_FRAMES) {
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
        uint32_t run = AUDIODSP_TANK_FRAMES - produced;
        if (run > self->pending_frames) {
            run = self->pending_frames;
        }
        audiodsp_tank_process_s16(&self->config, &self->state,
            &self->buffer[produced * self->base.channel_count], self->pending,
            run);
        self->pending += run * self->base.channel_count;
        self->pending_frames -= run;
        produced += run;
    }
    // A starved chain gets silence rather than a short block: this node sits
    // in the middle of a live graph and never reports itself finished. The
    // tail stops with the source -- the lines only advance for frames that
    // arrive -- which is `audioecho.FeedbackDelay`'s behaviour and
    // `audiodelays.Echo`'s before it. A class that wants the tail rung out
    // feeds the tank silence for as long as `tail_samples` says.
    if (produced == 0) {
        memset(self->buffer, 0, sizeof(self->buffer));
        produced = AUDIODSP_TANK_FRAMES;
    }
    *buffer = (uint8_t *)self->buffer;
    *buffer_length = produced * 2u * self->base.channel_count;
    return GET_BUFFER_MORE_DATA;
}

static void audioverb_tank_reset_buffer(mp_obj_t self_in,
    bool single_channel_output, uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    audioverb_tank_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->pending = NULL;
    self->pending_frames = 0;
    // Unlike audiodynamics, everything goes. A reverberation tail is entirely
    // state: a chain restarted with the old tail still in the lines plays the
    // previous take underneath the new one.
    audiodsp_tank_reset(&self->state, &self->config);
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
static mp_obj_t audioverb_tank_deinit(mp_obj_t self_in) {
    audioverb_tank_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    self->source = mp_const_none;
    self->pending = NULL;
    self->pending_frames = 0;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioverb_tank_deinit_obj, audioverb_tank_deinit);

static const mp_rom_map_elem_t audioverb_tank_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audioverb_tank_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audioverb_tank_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&audioverb_tank_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&audioverb_tank_clear_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audioverb_tank_locals, audioverb_tank_locals_table);

// What is behind this node, for audiosample_find_type()'s walk.
// audiodsp#112: the pump refuses a file-backed source at handover,
// and the protocol had no way to look past the tail.
static mp_obj_t audioverb_tank_sources(mp_obj_t self_in, uint8_t index) {
    return index == 0 ? ((audioverb_tank_obj_t *)MP_OBJ_TO_PTR(self_in))->source
                      : MP_OBJ_NULL;
}

static const audiosample_p_t audioverb_tank_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = audioverb_tank_reset_buffer,
    .get_buffer = audioverb_tank_get_buffer,
    .sources = audioverb_tank_sources,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audioverb_tank_type,
    MP_QSTR_Tank,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audioverb_tank_make_new,
    attr, cp_compat_attr,
    locals_dict, &audioverb_tank_locals,
    protocol, &audioverb_tank_proto
    );
