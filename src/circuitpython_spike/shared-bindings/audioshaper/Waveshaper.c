// audioshaper.Waveshaper bindings for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <string.h>

#include "shared-bindings/audioshaper/Waveshaper.h"
#include "shared-bindings/audiocore/__init__.h"

#include "py/objproperty.h"
#include "shared/runtime/context_manager_helpers.h"
#include "py/runtime.h"

//| class Waveshaper:
//|     """A nonlinearity read out of a table you supply, applied above the
//|     sample rate between a matched pair of polyphase all-pass half-bands.
//|
//|     Processes signed 16-bit audio, and hands out 256 frames at a time. It
//|     sits in an audiosample chain like any other effect, and never reports
//|     itself finished - a starved chain gets silence. It has no tail of its
//|     own: no delay line, no reverberation."""
//|
//|     def __init__(
//|         self,
//|         *,
//|         sample_rate: int = 48000,
//|         curve: circuitpython_typing.ReadableBuffer,
//|         oversample: int = 4,
//|         channel_count: int = 2,
//|         pre_gain: float = 1.0,
//|         bias: float = 0.0,
//|         post_gain: float = 1.0,
//|         mix: float = 1.0,
//|         hysteresis: float = 0.0,
//|         hysteresis_width: float = 0.02,
//|         hysteresis_bias: float = 0.0,
//|     ) -> None:
//|         """Create a waveshaper. ``curve`` is int16 Q15, at least two
//|         points, spanning -1..+1 of input; compute it once on a desktop and
//|         ship it as data, never rebuild it on the board. ``oversample`` is
//|         1, 2, 4 or 8 and cannot change afterwards: it decides how many
//|         half-band stages carry state.
//|
//|         ``pre_gain`` is the drive knob - gain into one normalised curve,
//|         rather than a curve rebuilt on every knob move. ``bias`` moves the
//|         operating point; a bias that has to move *per sample* is a second
//|         stream summed in front of this node, not an argument here.
//|         ``mix`` is a straight 0..1 crossfade, ``audiofilters.Distortion``'s
//|         convention.
//|
//|         ``hysteresis`` is off by default. Above zero it puts a play
//|         (backlash) operator in front of the table, so a slow signal in and
//|         out traces two different paths and encloses an area, which a static
//|         curve cannot. ``hysteresis_width`` is its half-width as a fraction
//|         of full scale *at the input*, so the drive knob does not smear it,
//|         and ``hysteresis_bias`` splits that half-width between the rising
//|         and falling branches."""
//|         ...

// The options __init__ and set() accept, paired with the shared DSP's enum.
// `sample_rate`, `channel_count`, `oversample` and `curve` are deliberately
// absent: the first three are fixed at construction and the fourth is not a
// float, so all four are handled ahead of this table and keyword order stays
// irrelevant.
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
    audiodsp_shaper_set_curve(&self->config, copy, (uint32_t)(info.len / 2));
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
    // Powers of two only, and no higher than 8: the state is a fixed number
    // of half-band stages, and rounding a stray 3 down to 2 would be a
    // different effect than the caller asked for, quietly.
    if (oversample != 1u && oversample != 2u && oversample != 4u &&
        oversample != 8u) {
        mp_raise_ValueError(MP_ERROR_TEXT("oversample must be 1, 2, 4 or 8"));
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

//|     def play(self, sample: circuitpython_typing.AudioSample) -> None:
//|         """Set the source the shaper reads from."""
//|         ...
static mp_obj_t audioshaper_waveshaper_play(mp_obj_t self_in,
    mp_obj_t sample) {
    audioshaper_waveshaper_obj_t *self = MP_OBJ_TO_PTR(self_in);
    (void)audiosample_check(sample);
    self->source = sample;
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audioshaper_waveshaper_play_obj,
    audioshaper_waveshaper_play);

//|     def set(self, **options: float) -> None:
//|         """Change settings mid-stream. The half-band memories and the play
//|         position keep their contents; only what the node does to them
//|         changes. ``curve`` may be swapped here too."""
//|         ...
static mp_obj_t audioshaper_waveshaper_set(size_t n_args,
    const mp_obj_t *args, mp_map_t *kw_args) {
    audioshaper_waveshaper_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    (void)n_args;
    waveshaper_apply_kwargs(self, kw_args);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(audioshaper_waveshaper_set_obj, 1,
    audioshaper_waveshaper_set);

//|     def clear(self) -> None:
//|         """Empty the half-band memories and the play position. That is the
//|         whole of this node's state: it has no delay line."""
//|         ...
//|
//|
static mp_obj_t audioshaper_waveshaper_clear(mp_obj_t self_in) {
    audioshaper_waveshaper_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_shaper_reset(&self->state);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audioshaper_waveshaper_clear_obj,
    audioshaper_waveshaper_clear);

// `deinit()` releases what this binding holds and marks the node
// deinitialised, so the guarded getters raise afterwards. The MicroPython
// binding of this same type gained it on 2026-09-09 (audiodsp#58, #60, #63) and
// this copy did not, which is audiodsp#75: the two bindings are hand-written and
// nothing held them to each other. Same fields, same order, deliberately.
static mp_obj_t audioshaper_waveshaper_deinit(mp_obj_t self_in) {
    audioshaper_waveshaper_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    self->source = mp_const_none;
    self->pending = NULL;
    self->pending_frames = 0;
    self->curve = NULL;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audioshaper_waveshaper_deinit_obj, audioshaper_waveshaper_deinit);

static const mp_rom_map_elem_t
audioshaper_waveshaper_locals_dict_table[] = {
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
static MP_DEFINE_CONST_DICT(audioshaper_waveshaper_locals_dict,
    audioshaper_waveshaper_locals_dict_table);

static const audiosample_p_t audioshaper_waveshaper_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)
        audioshaper_waveshaper_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)
        audioshaper_waveshaper_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audioshaper_waveshaper_type,
    MP_QSTR_Waveshaper,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audioshaper_waveshaper_make_new,
    locals_dict, &audioshaper_waveshaper_locals_dict,
    protocol, &audioshaper_waveshaper_proto
    );
