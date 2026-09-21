// audioverb.Tank bindings for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <string.h>

#include "shared-bindings/audioverb/Tank.h"
#include "shared-bindings/audiocore/__init__.h"

#include "py/objproperty.h"
#include "shared/runtime/context_manager_helpers.h"
#include "py/runtime.h"

//| class Tank:
//|     """Dattorro's plate reverberator, with its twelve line lengths and its
//|     output taps handed in from Python.
//|
//|     Processes signed 16-bit audio and hands out 256 frames at a time. It
//|     sits in an audiosample chain like any other effect, and never reports
//|     itself finished - a starved chain gets silence. The lines only advance
//|     with frames that arrive, so the tail stops with the source rather than
//|     ringing on, the same way `audiodelays.Echo` and
//|     `audioecho.FeedbackDelay` behave."""
//|
//|     def __init__(
//|         self,
//|         *,
//|         sample_rate: int = 48000,
//|         channel_count: int = 2,
//|         max_predelay_ms: float = 200.0,
//|         delays: Sequence[int] | None = None,
//|         taps: Sequence[float] | None = None,
//|         decay: float = 0.5,
//|         diffusion: float = 0.75,
//|         damping_hz: float = 0.0,
//|         bandwidth_hz: float = 0.0,
//|         low_cut_hz: float = 0.0,
//|         predelay_ms: float = 0.0,
//|         mod_depth_ms: float = 0.0,
//|         mod_rate_hz: float = 0.0,
//|         drive: float = 0.0,
//|         width: float = 1.0,
//|         tone_db: float = 0.0,
//|         mix: float = 0.3,
//|     ) -> None:
//|         """Create a reverberation tank. ``max_predelay_ms``, ``delays`` and
//|         ``taps`` size the allocation and cut the topology, and none of the
//|         three can change afterwards.
//|
//|         ``delays`` is twelve line lengths in frames - the four input
//|         diffusers, then each tank half's modulated all-pass, delay,
//|         all-pass and delay. ``taps`` is four values per tap: channel, line
//|         index, offset in frames, gain. Both default to Dattorro's own
//|         published table, scaled from 29761 Hz to ``sample_rate``.
//|
//|         Every filter is out of the path at zero, so a bare ``Tank()`` is
//|         that network and nothing else. ``damping_hz`` is a one-pole *inside*
//|         each half's loop, so every pass through the tank loses a little more
//|         top than the last; ``bandwidth_hz`` and ``low_cut_hz`` band-limit
//|         what goes in. ``mod_depth_ms`` and ``mod_rate_hz`` wobble the first
//|         all-pass in each half against a quadrature pair, which is what
//|         breaks a static plate's picket-fence ringing. ``tone_db`` tilts the
//|         wet output about 1 kHz and ``width`` is its mid/side spread."""
//|         ...

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

//|     def play(self, sample: circuitpython_typing.AudioSample) -> None:
//|         """Set the source the tank reads from."""
//|         ...
static mp_obj_t audioverb_tank_play(mp_obj_t self_in, mp_obj_t sample) {
    audioverb_tank_obj_t *self = MP_OBJ_TO_PTR(self_in);
    (void)audiosample_check(sample);
    self->source = sample;
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audioverb_tank_play_obj, audioverb_tank_play);

//|     def set(self, **options: float) -> None:
//|         """Change settings mid-stream. The lines keep their contents; only
//|         what the network does to them changes."""
//|         ...
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
    audiodsp_tank_config_finish(&self->config);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(audioverb_tank_set_obj, 1,
    audioverb_tank_set);

//|     def clear(self) -> None:
//|         """Empty every line and every filter."""
//|         ...
//|
//|
static mp_obj_t audioverb_tank_clear(mp_obj_t self_in) {
    audioverb_tank_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_tank_reset(&self->state, &self->config);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audioverb_tank_clear_obj,
    audioverb_tank_clear);

// `deinit()` releases what this binding holds and marks the node
// deinitialised, so the guarded getters raise afterwards. The MicroPython
// binding of this same type gained it on 2026-09-09 (audiodsp#58, #60, #63) and
// this copy did not, which is audiodsp#75: the two bindings are hand-written and
// nothing held them to each other. Same fields, same order, deliberately.
static mp_obj_t audioverb_tank_deinit(mp_obj_t self_in) {
    audioverb_tank_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    self->source = mp_const_none;
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audioverb_tank_deinit_obj, audioverb_tank_deinit);

static const mp_rom_map_elem_t audioverb_tank_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audioverb_tank_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audioverb_tank_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&audioverb_tank_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&audioverb_tank_clear_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audioverb_tank_locals_dict,
    audioverb_tank_locals_dict_table);

static const audiosample_p_t audioverb_tank_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audioverb_tank_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audioverb_tank_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audioverb_tank_type,
    MP_QSTR_Tank,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audioverb_tank_make_new,
    locals_dict, &audioverb_tank_locals_dict,
    protocol, &audioverb_tank_proto
    );
