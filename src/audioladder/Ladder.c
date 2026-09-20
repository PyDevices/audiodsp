// audioladder.Ladder. See Ladder.h for provenance.
// SPDX-License-Identifier: MIT

#include "audioladder/Ladder.h"

#include <string.h>

#include "cp_compat/context_manager_helpers.h"
#include "py/runtime.h"
#include "shared/audioif_pump_lock.h"

// The options `Ladder(...)` and `set(...)` accept, paired with the shared
// DSP's enum. `sample_rate` and `channel_count` are deliberately absent: they
// describe the stream rather than the filter, so they are applied ahead of
// this table rather than from it, and keyword order stays irrelevant.
typedef struct {
    qstr name;
    audioif_ladder_option_t option;
} ladder_option_name_t;

static const ladder_option_name_t ladder_option_names[] = {
    { MP_QSTR_cutoff_hz, AUDIOIF_LADDER_OPT_CUTOFF_HZ },
    { MP_QSTR_resonance, AUDIOIF_LADDER_OPT_RESONANCE },
    { MP_QSTR_drive, AUDIOIF_LADDER_OPT_DRIVE },
    { MP_QSTR_poles, AUDIOIF_LADDER_OPT_POLES },
    { MP_QSTR_passband_comp, AUDIOIF_LADDER_OPT_PASSBAND_COMP },
    { MP_QSTR_oversample, AUDIOIF_LADDER_OPT_OVERSAMPLE },
    { MP_QSTR_mix, AUDIOIF_LADDER_OPT_MIX },
};

static void ladder_apply_kwargs(audioladder_ladder_obj_t *self,
    const mp_map_t *kw) {
    for (size_t i = 0; i < kw->alloc; ++i) {
        if (!mp_map_slot_is_filled(kw, i)) {
            continue;
        }
        qstr name = mp_obj_str_get_qstr(kw->table[i].key);
        if (name == MP_QSTR_sample_rate || name == MP_QSTR_channel_count) {
            continue;
        }
        float value = (float)mp_obj_get_float(kw->table[i].value);
        bool known = false;
        for (size_t option = 0;
             option < MP_ARRAY_SIZE(ladder_option_names); ++option) {
            if (ladder_option_names[option].name == name) {
                audioif_ladder_configure(&self->config,
                    ladder_option_names[option].option, value);
                known = true;
                break;
            }
        }
        if (!known) {
            mp_raise_msg_varg(&mp_type_TypeError,
                MP_ERROR_TEXT("unknown Ladder option '%q'"), name);
        }
    }
}

static mp_obj_t audioladder_ladder_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, true);
    mp_map_t kw_map;
    mp_map_init_fixed_table(&kw_map, n_kw, all_args + n_args);

    uint32_t sample_rate = 48000;
    uint32_t channel_count = 2;
    for (size_t i = 0; i < kw_map.alloc; ++i) {
        if (!mp_map_slot_is_filled(&kw_map, i)) {
            continue;
        }
        qstr name = mp_obj_str_get_qstr(kw_map.table[i].key);
        if (name == MP_QSTR_sample_rate) {
            sample_rate = (uint32_t)mp_obj_get_int(kw_map.table[i].value);
        } else if (name == MP_QSTR_channel_count) {
            channel_count = (uint32_t)mp_obj_get_int(kw_map.table[i].value);
        }
    }
    if (sample_rate < 1u) {
        mp_raise_ValueError(MP_ERROR_TEXT("sample_rate must be at least 1"));
    }
    if (channel_count < 1u || channel_count > 2u) {
        mp_raise_ValueError(MP_ERROR_TEXT("channel_count must be 1 or 2"));
    }

    audioladder_ladder_obj_t *self =
        mp_obj_malloc(audioladder_ladder_obj_t, type);
    self->base.sample_rate = sample_rate;
    self->base.max_buffer_length = sizeof(self->buffer);
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)channel_count;
    self->base.samples_signed = 1;
    self->base.single_buffer = false;
    self->source = MP_OBJ_NULL;
    self->pending = NULL;
    self->pending_frames = 0;

    audioif_ladder_config_init(&self->config, sample_rate);
    audioif_ladder_set_channel_count(&self->config, channel_count);
    audioif_ladder_state_init(&self->state);

    ladder_apply_kwargs(self, &kw_map);
    audioif_ladder_config_finish(&self->config);
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audioladder_ladder_play(mp_obj_t self_in, mp_obj_t sample) {
    audioladder_ladder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    (void)audiosample_check(sample);
    audioif_pump_lock_acquire();
    self->source = sample;
    self->pending = NULL;
    self->pending_frames = 0;
    audioif_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audioladder_ladder_play_obj,
    audioladder_ladder_play);

static mp_obj_t audioladder_ladder_set(size_t n_args, const mp_obj_t *args,
    mp_map_t *kw_args) {
    audioladder_ladder_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    (void)n_args;
    ladder_apply_kwargs(self, kw_args);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audioladder_ladder_set_obj, 1,
    audioladder_ladder_set);

static mp_obj_t audioladder_ladder_clear(mp_obj_t self_in) {
    audioladder_ladder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audioif_pump_lock_acquire();
    audioif_ladder_reset(&self->state);
    audioif_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioladder_ladder_clear_obj,
    audioladder_ladder_clear);

static audioio_get_buffer_result_t audioladder_ladder_get_buffer(
    mp_obj_t self_in, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    audioladder_ladder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint32_t produced = 0;
    while (produced < AUDIOIF_LADDER_FRAMES) {
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
        uint32_t run = AUDIOIF_LADDER_FRAMES - produced;
        if (run > self->pending_frames) {
            run = self->pending_frames;
        }
        audioif_ladder_process_s16(&self->config, &self->state,
            &self->buffer[produced * self->base.channel_count],
            self->pending, run);
        self->pending += run * self->base.channel_count;
        self->pending_frames -= run;
        produced += run;
    }
    // A starved chain gets silence rather than a short block: this node sits
    // in the middle of a live graph and never reports itself finished. A
    // self-oscillation stops with the source, for the same reason audioecho's
    // repeats do -- the loop is only advanced by frames that arrive.
    if (produced == 0) {
        memset(self->buffer, 0, sizeof(self->buffer));
        produced = AUDIOIF_LADDER_FRAMES;
    }
    *buffer = (uint8_t *)self->buffer;
    *buffer_length = produced * 2u * self->base.channel_count;
    return GET_BUFFER_MORE_DATA;
}

static void audioladder_ladder_reset_buffer(mp_obj_t self_in,
    bool single_channel_output, uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    audioladder_ladder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->pending = NULL;
    self->pending_frames = 0;
    // Like audioecho and unlike audiodynamics, everything goes: a
    // self-oscillating filter restarted with its integrators still charged
    // carries the previous take's tone into the new one.
    audioif_ladder_reset(&self->state);
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
static mp_obj_t audioladder_ladder_deinit(mp_obj_t self_in) {
    audioladder_ladder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    audioif_pump_lock_acquire();
    self->source = mp_const_none;
    self->pending = NULL;
    self->pending_frames = 0;
    audioif_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioladder_ladder_deinit_obj, audioladder_ladder_deinit);

static const mp_rom_map_elem_t audioladder_ladder_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audioladder_ladder_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audioladder_ladder_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&audioladder_ladder_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&audioladder_ladder_clear_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audioladder_ladder_locals,
    audioladder_ladder_locals_table);

static const audiosample_p_t audioladder_ladder_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = audioladder_ladder_reset_buffer,
    .get_buffer = audioladder_ladder_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audioladder_ladder_type,
    MP_QSTR_Ladder,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audioladder_ladder_make_new,
    attr, cp_compat_attr,
    locals_dict, &audioladder_ladder_locals,
    protocol, &audioladder_ladder_proto
    );
