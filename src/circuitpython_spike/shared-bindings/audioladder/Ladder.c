// audioladder.Ladder bindings for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <string.h>

#include "shared-bindings/audioladder/Ladder.h"
#include "shared-bindings/audiocore/__init__.h"

#include "py/objproperty.h"
#include "shared/runtime/context_manager_helpers.h"
#include "py/runtime.h"

//| class Ladder:
//|     """Four one-pole stages round a global feedback loop, with an odd
//|     saturator inside the loop.
//|
//|     Processes signed 16-bit mono or stereo, and hands out 256 frames at a
//|     time. It sits in an audiosample chain like any other effect, and never
//|     reports itself finished - a starved chain gets silence. The loop only
//|     advances with frames that arrive, so a self-oscillation stops with the
//|     source rather than ringing on."""
//|
//|     def __init__(
//|         self,
//|         *,
//|         sample_rate: int = 48000,
//|         channel_count: int = 2,
//|         cutoff_hz: float = 1000.0,
//|         resonance: float = 0.0,
//|         drive: float = 1.0,
//|         poles: int = 4,
//|         passband_comp: float = 0.0,
//|         oversample: int = 2,
//|         mix: float = 1.0,
//|     ) -> None:
//|         """Create a ladder filter.
//|
//|         ``resonance`` is the classic 0..4 feedback: at 4 the loop sustains
//|         a tone of its own, at ``cutoff_hz``, from silence, and the
//|         saturator inside the loop is what stops that tone growing without
//|         bound. ``drive`` is a linear gain into the loop, so how hard the
//|         saturator is hit - and therefore how much growl there is - follows
//|         the input level the way it does in the circuit. ``poles`` taps the
//|         output from the first, second, third or fourth stage (6, 12, 18 or
//|         24 dB an octave); the feedback is always the fourth, so the
//|         resonance is the same filter's at every tap. ``passband_comp``
//|         gives back the passband the feedback subtracts, 0 for none - what
//|         the circuit does - and 1 for all of it. ``oversample`` runs the
//|         loop at twice the rate so the harmonics the saturator makes fold
//|         back less. ``mix`` is a plain crossfade, 0 a wire and 1 the
//|         filter."""
//|         ...

// The options __init__ and set() accept, paired with the shared DSP's enum.
// `sample_rate` and `channel_count` are deliberately absent: they describe
// the stream rather than the filter, so they are applied ahead of this table
// rather than from it, and keyword order stays irrelevant.
typedef struct {
    qstr name;
    audiodsp_ladder_option_t option;
} ladder_option_name_t;

static const ladder_option_name_t ladder_option_names[] = {
    { MP_QSTR_cutoff_hz, AUDIODSP_LADDER_OPT_CUTOFF_HZ },
    { MP_QSTR_resonance, AUDIODSP_LADDER_OPT_RESONANCE },
    { MP_QSTR_drive, AUDIODSP_LADDER_OPT_DRIVE },
    { MP_QSTR_poles, AUDIODSP_LADDER_OPT_POLES },
    { MP_QSTR_passband_comp, AUDIODSP_LADDER_OPT_PASSBAND_COMP },
    { MP_QSTR_oversample, AUDIODSP_LADDER_OPT_OVERSAMPLE },
    { MP_QSTR_mix, AUDIODSP_LADDER_OPT_MIX },
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
                audiodsp_ladder_configure(&self->config,
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

    audiodsp_ladder_config_init(&self->config, sample_rate);
    audiodsp_ladder_set_channel_count(&self->config, channel_count);
    audiodsp_ladder_state_init(&self->state);

    ladder_apply_kwargs(self, &kw_map);
    audiodsp_ladder_config_finish(&self->config);
    return MP_OBJ_FROM_PTR(self);
}

//|     def play(self, sample: circuitpython_typing.AudioSample) -> None:
//|         """Set the source the filter reads from."""
//|         ...
static mp_obj_t audioladder_ladder_play(mp_obj_t self_in, mp_obj_t sample) {
    audioladder_ladder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    (void)audiosample_check(sample);
    self->source = sample;
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audioladder_ladder_play_obj,
    audioladder_ladder_play);

//|     def set(self, **options: float) -> None:
//|         """Change settings mid-stream. The integrators keep their charge;
//|         only what the loop does to them changes. This is the call a macro
//|         rides on, once a block."""
//|         ...
static mp_obj_t audioladder_ladder_set(size_t n_args, const mp_obj_t *args,
    mp_map_t *kw_args) {
    audioladder_ladder_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    (void)n_args;
    ladder_apply_kwargs(self, kw_args);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(audioladder_ladder_set_obj, 1,
    audioladder_ladder_set);

//|     def clear(self) -> None:
//|         """Empty the integrators and the solver's history, which also
//|         stops a self-oscillation."""
//|         ...
//|
//|
static mp_obj_t audioladder_ladder_clear(mp_obj_t self_in) {
    audioladder_ladder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_ladder_reset(&self->state);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audioladder_ladder_clear_obj,
    audioladder_ladder_clear);

// `deinit()` releases what this binding holds and marks the node
// deinitialised, so the guarded getters raise afterwards. The MicroPython
// binding of this same type gained it on 2026-09-09 (audiodsp#58, #60, #63) and
// this copy did not, which is audiodsp#75: the two bindings are hand-written and
// nothing held them to each other. Same fields, same order, deliberately.
static mp_obj_t audioladder_ladder_deinit(mp_obj_t self_in) {
    audioladder_ladder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    self->source = mp_const_none;
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audioladder_ladder_deinit_obj, audioladder_ladder_deinit);

static const mp_rom_map_elem_t audioladder_ladder_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audioladder_ladder_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audioladder_ladder_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&audioladder_ladder_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&audioladder_ladder_clear_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audioladder_ladder_locals_dict,
    audioladder_ladder_locals_dict_table);

static const audiosample_p_t audioladder_ladder_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)
        audioladder_ladder_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)
        audioladder_ladder_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audioladder_ladder_type,
    MP_QSTR_Ladder,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audioladder_ladder_make_new,
    locals_dict, &audioladder_ladder_locals_dict,
    protocol, &audioladder_ladder_proto
    );
