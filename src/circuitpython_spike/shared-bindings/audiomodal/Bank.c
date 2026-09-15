// audiomodal.Bank bindings for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <string.h>

#include "shared-bindings/audiomodal/Bank.h"
#include "shared-bindings/audiocore/__init__.h"

#include "py/objproperty.h"
#include "shared/runtime/context_manager_helpers.h"
#include "py/runtime.h"

//| class Bank:
//|     """A bank of resonators: N decaying sinusoids struck by one excitation.
//|
//|     Processes signed 16-bit audio and hands out 256 frames at a time. It
//|     sits in an audiosample chain like any other effect and never reports
//|     itself finished. Unlike `audiodelays.Echo`, `audioecho.FeedbackDelay`
//|     and `audioverb.Tank`, the tail keeps ringing after the source stops -
//|     that is the whole behaviour of a struck object, and a drum whose decay
//|     ended the instant the stick left would be the one thing this is not."""
//|
//|     def __init__(
//|         self,
//|         *,
//|         sample_rate: int = 48000,
//|         channel_count: int = 2,
//|         modes: int = 8,
//|         mix: float = 1.0,
//|         gain: float = 1.0,
//|     ) -> None:
//|         """Create a resonator bank. ``modes`` sizes the allocation and
//|         cannot change afterwards. Every mode starts silent and the default
//|         ``mix`` is fully wet, so a bare ``Bank()`` is silence until a mode
//|         is set - not a passthrough. Set ``mix=0.0`` for the untouched
//|         signal.
//|
//|         ``mix`` crossfades the untouched signal against the resonated one
//|         and ``gain`` scales the summed bank before that crossfade, so a
//|         caller can write the literature's relative amplitudes per mode and
//|         set the level once."""
//|         ...

// The options `Bank(...)` and `set(...)` accept, paired with the shared DSP's
// enum. `modes`, `sample_rate` and `channel_count` are deliberately absent:
// they size the allocation, so they are applied ahead of this table rather
// than from it, and keyword order stays irrelevant.
typedef struct {
    qstr name;
    audioif_modal_option_t option;
} modal_option_name_t;

static const modal_option_name_t modal_option_names[] = {
    { MP_QSTR_mix, AUDIOIF_MODAL_OPT_MIX },
    { MP_QSTR_gain, AUDIOIF_MODAL_OPT_GAIN },
};

static void modal_apply_kwargs(audiomodal_bank_obj_t *self,
    const mp_map_t *kw) {
    for (size_t i = 0; i < kw->alloc; ++i) {
        if (!mp_map_slot_is_filled(kw, i)) {
            continue;
        }
        qstr name = mp_obj_str_get_qstr(kw->table[i].key);
        if (name == MP_QSTR_sample_rate || name == MP_QSTR_channel_count ||
            name == MP_QSTR_modes) {
            continue;
        }
        float value = (float)mp_obj_get_float(kw->table[i].value);
        bool known = false;
        for (size_t option = 0;
             option < MP_ARRAY_SIZE(modal_option_names); ++option) {
            if (modal_option_names[option].name == name) {
                audioif_modal_configure(&self->config,
                    modal_option_names[option].option, value);
                known = true;
                break;
            }
        }
        if (!known) {
            mp_raise_msg_varg(&mp_type_TypeError,
                MP_ERROR_TEXT("unknown Bank option '%q'"), name);
        }
    }
}

static mp_obj_t audiomodal_bank_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, true);
    mp_map_t kw_map;
    mp_map_init_fixed_table(&kw_map, n_kw, all_args + n_args);

    uint32_t sample_rate = 48000;
    uint32_t channel_count = 2;
    mp_int_t modes = 8;
    for (size_t i = 0; i < kw_map.alloc; ++i) {
        if (!mp_map_slot_is_filled(&kw_map, i)) {
            continue;
        }
        qstr name = mp_obj_str_get_qstr(kw_map.table[i].key);
        if (name == MP_QSTR_sample_rate) {
            sample_rate = (uint32_t)mp_obj_get_int(kw_map.table[i].value);
        } else if (name == MP_QSTR_channel_count) {
            channel_count = (uint32_t)mp_obj_get_int(kw_map.table[i].value);
        } else if (name == MP_QSTR_modes) {
            modes = mp_obj_get_int(kw_map.table[i].value);
        }
    }
    if (modes < 1 || modes > (mp_int_t)AUDIOIF_MODAL_MAX_MODES) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("modes must be 1 to %d"),
            (int)AUDIOIF_MODAL_MAX_MODES);
    }
    if (channel_count < 1u || channel_count > 2u) {
        mp_raise_ValueError(MP_ERROR_TEXT("channel_count must be 1 or 2"));
    }

    audiomodal_bank_obj_t *self = mp_obj_malloc(audiomodal_bank_obj_t, type);
    self->base.sample_rate = sample_rate;
    self->base.max_buffer_length = sizeof(self->buffer);
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)channel_count;
    self->base.samples_signed = 1;
    self->base.single_buffer = false;
    self->source = MP_OBJ_NULL;
    self->pending = NULL;
    self->pending_frames = 0;

    self->modes = m_malloc((size_t)modes * sizeof(audioif_modal_mode_t));
    self->coeffs = m_malloc((size_t)modes * sizeof(audioif_modal_coeff_t));
    audioif_modal_config_init(&self->config, sample_rate, channel_count,
        (uint32_t)modes, self->modes, self->coeffs);
    uint32_t words = audioif_modal_state_floats(&self->config);
    self->s1 = m_malloc((size_t)words * sizeof(float));
    self->s2 = m_malloc((size_t)words * sizeof(float));
    audioif_modal_state_init(&self->state, &self->config, self->s1, self->s2,
        words);

    modal_apply_kwargs(self, &kw_map);
    audioif_modal_config_finish(&self->config);
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audiomodal_bank_play(mp_obj_t self_in, mp_obj_t sample) {
    audiomodal_bank_obj_t *self = MP_OBJ_TO_PTR(self_in);
    (void)audiosample_check(sample);
    self->source = sample;
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiomodal_bank_play_obj,
    audiomodal_bank_play);

static mp_obj_t audiomodal_bank_set(size_t n_args, const mp_obj_t *args,
    mp_map_t *kw_args) {
    audiomodal_bank_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    (void)n_args;
    modal_apply_kwargs(self, kw_args);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(audiomodal_bank_set_obj, 1,
    audiomodal_bank_set);

// set_mode(index, frequency, decay, gain) -- one mode, positionally, because
// that is the order a modal table is published in and the order a caller
// reading one out of a paper will type.
static mp_obj_t audiomodal_bank_set_mode(size_t n_args,
    const mp_obj_t *args) {
    audiomodal_bank_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_int_t index = mp_obj_get_int(args[1]);
    if (index < 0 || (uint32_t)index >= self->config.mode_count) {
        mp_raise_msg_varg(&mp_type_IndexError,
            MP_ERROR_TEXT("mode index must be 0 to %d"),
            (int)self->config.mode_count - 1);
    }
    audioif_modal_set_mode(&self->config, (uint32_t)index,
        (float)mp_obj_get_float(args[2]), (float)mp_obj_get_float(args[3]),
        (float)mp_obj_get_float(args[4]));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiomodal_bank_set_mode_obj, 5, 5,
    audiomodal_bank_set_mode);

// set_modes(table) -- the whole table in one call. An instrument changing kit
// mid-bar cannot afford one Python call per mode, and a drum with a dozen
// partials is a dozen calls.
static mp_obj_t audiomodal_bank_set_modes(mp_obj_t self_in, mp_obj_t table) {
    audiomodal_bank_obj_t *self = MP_OBJ_TO_PTR(self_in);
    size_t count = 0;
    mp_obj_t *rows = NULL;
    mp_obj_get_array(table, &count, &rows);
    if (count > self->config.mode_count) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("table has %d rows, bank holds %d"), (int)count,
            (int)self->config.mode_count);
    }
    for (size_t i = 0; i < count; ++i) {
        size_t fields = 0;
        mp_obj_t *row = NULL;
        mp_obj_get_array(rows[i], &fields, &row);
        if (fields != 3) {
            mp_raise_ValueError(MP_ERROR_TEXT(
                "each mode is (frequency, decay, gain)"));
        }
        audioif_modal_set_mode(&self->config, (uint32_t)i,
            (float)mp_obj_get_float(row[0]), (float)mp_obj_get_float(row[1]),
            (float)mp_obj_get_float(row[2]));
    }
    // Rows the table did not reach are silenced rather than left holding the
    // last kit's partials, so a shorter table is a smaller drum and not a
    // chord of two.
    for (uint32_t i = (uint32_t)count; i < self->config.mode_count; ++i) {
        audioif_modal_set_mode(&self->config, i, 0.0f,
            AUDIOIF_MODAL_MIN_DECAY, 0.0f);
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiomodal_bank_set_modes_obj,
    audiomodal_bank_set_modes);

static mp_obj_t audiomodal_bank_clear(mp_obj_t self_in) {
    audiomodal_bank_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audioif_modal_reset(&self->state);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiomodal_bank_clear_obj,
    audiomodal_bank_clear);

// True while any mode still holds energy. Exact rather than a threshold,
// because the flush in the kernel makes a finished mode exactly zero -- so a
// voice allocator can ask "has this drum stopped?" and be told the truth.
static mp_obj_t audiomodal_bank_obj_get_ringing(mp_obj_t self_in) {
    audiomodal_bank_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(!audioif_modal_silent(&self->state));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiomodal_bank_get_ringing_obj,
    audiomodal_bank_obj_get_ringing);
MP_PROPERTY_GETTER(audiomodal_bank_ringing_obj,
    (mp_obj_t)&audiomodal_bank_get_ringing_obj);

static mp_obj_t audiomodal_bank_obj_get_modes(mp_obj_t self_in) {
    audiomodal_bank_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->config.mode_count);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiomodal_bank_get_modes_obj,
    audiomodal_bank_obj_get_modes);
MP_PROPERTY_GETTER(audiomodal_bank_modes_obj,
    (mp_obj_t)&audiomodal_bank_get_modes_obj);

// `deinit()` releases what this binding holds and marks the node
// deinitialised, which is what makes every guarded entry point raise
// afterwards (audioif#58, #60, #63). The inline output buffer goes with the
// object; what is cleared here is what the object holds a *reference* to --
// the upstream source, so releasing the tail of a chain lets the GC reclaim
// the rest of it, and every borrowed pointer into a source's buffer, so
// nothing dangles.
static mp_obj_t audiomodal_bank_deinit(mp_obj_t self_in) {
    audiomodal_bank_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    self->source = mp_const_none;
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiomodal_bank_deinit_obj,
    audiomodal_bank_deinit);


//|     def set_mode(
//|         self, index: int, frequency: float, decay: float, gain: float
//|     ) -> None:
//|         """Place one mode. ``decay`` is its 60 dB time in **seconds**, not
//|         a Q, and ``gain`` is the peak of its impulse response - so a modal
//|         table read out of a paper goes in as published."""
//|         ...
//|
//|     def set_modes(self, table: Sequence[Sequence[float]]) -> None:
//|         """Place the whole table at once, as ``(frequency, decay, gain)``
//|         rows. Rows the table does not reach are silenced, so a shorter
//|         table is a smaller object and not a chord of two."""
//|         ...
//|
//|     def clear(self) -> None:
//|         """Stop every mode ringing at once."""
//|         ...
//|
//|     ringing: bool
//|     """True while any mode still holds energy. Exact rather than a
//|     threshold, because a finished mode is flushed to exact zero."""
//|
//|     modes: int
//|     """How many modes this bank was built with."""
//|
//|
static const mp_rom_map_elem_t audiomodal_bank_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiomodal_bank_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiomodal_bank_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&audiomodal_bank_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_mode),
      MP_ROM_PTR(&audiomodal_bank_set_mode_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_modes),
      MP_ROM_PTR(&audiomodal_bank_set_modes_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&audiomodal_bank_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_ringing),
      MP_ROM_PTR(&audiomodal_bank_ringing_obj) },
    { MP_ROM_QSTR(MP_QSTR_modes), MP_ROM_PTR(&audiomodal_bank_modes_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiomodal_bank_locals_dict,
    audiomodal_bank_locals_dict_table);

static const audiosample_p_t audiomodal_bank_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audiomodal_bank_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audiomodal_bank_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiomodal_bank_type,
    MP_QSTR_Bank,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiomodal_bank_make_new,
    locals_dict, &audiomodal_bank_locals_dict,
    protocol, &audiomodal_bank_proto
    );
