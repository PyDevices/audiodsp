// audiooscillator.Oscillator bindings for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <string.h>

#include "shared-bindings/audiooscillator/Oscillator.h"
#include "shared-bindings/audiocore/__init__.h"

#include "shared/runtime/context_manager_helpers.h"
#include "py/runtime.h"

//| class Oscillator:
//|     """Saw, pulse, triangle and sine from one phase, band-limited, with
//|     pitch, linear FM, pulse width and hard sync read from audio streams
//|     sample by sample.
//|
//|     A source: it hands out 256 frames at a time, or ``buffer_size`` bytes
//|     of them, and never reports itself finished. The output trails the
//|     phase by one sample, which is what lets a sync edge correct the sample
//|     before it."""
//|
//|     def __init__(
//|         self,
//|         *,
//|         sample_rate: int = 48000,
//|         channel_count: int = 1,
//|         buffer_size: Optional[int] = None,
//|         pitch: Optional[circuitpython_typing.AudioSample] = None,
//|         fm: Optional[circuitpython_typing.AudioSample] = None,
//|         pwm: Optional[circuitpython_typing.AudioSample] = None,
//|         sync: Optional[circuitpython_typing.AudioSample] = None,
//|         frequency: float = 440.0,
//|         waveform: int = SAW,
//|         waveform_right: int = SAW,
//|         pulse_width: float = 0.5,
//|         level: float = 1.0,
//|         pitch_octaves: float = 10.0,
//|         fm_depth: float = 0.0,
//|         pwm_depth: float = 0.5,
//|     ) -> None:
//|         """Create an oscillator.
//|
//|         ``waveform`` is any sum of `SAW`, `PULSE`, `TRIANGLE` and `SINE`,
//|         and the waveforms it names are added; a two-channel oscillator
//|         puts ``waveform_right`` on the right from the same phase. Each
//|         input is read from its first channel as a fraction ``v`` of full
//|         scale: ``pitch`` multiplies the frequency by
//|         ``2 ** (v * pitch_octaves)``, ``fm`` adds ``v * fm_depth`` Hz,
//|         ``pwm`` adds ``v * pwm_depth`` to the pulse width (kept in
//|         0.05..0.95), and a rising zero crossing of ``sync`` resets the
//|         phase, placed between the samples it fell between."""
//|         ...

// The options `Oscillator(...)` and `set(...)` accept, paired with the shared
// DSP's enum. The stream's own keywords (sample_rate, channel_count,
// buffer_size) and the four inputs are handled ahead of this table.
typedef struct {
    qstr name;
    audiodsp_oscillator_option_t option;
} oscillator_option_name_t;

static const oscillator_option_name_t oscillator_option_names[] = {
    { MP_QSTR_frequency, AUDIODSP_OSCILLATOR_OPT_FREQUENCY },
    { MP_QSTR_waveform, AUDIODSP_OSCILLATOR_OPT_WAVEFORM },
    { MP_QSTR_waveform_right, AUDIODSP_OSCILLATOR_OPT_WAVEFORM_RIGHT },
    { MP_QSTR_pulse_width, AUDIODSP_OSCILLATOR_OPT_PULSE_WIDTH },
    { MP_QSTR_level, AUDIODSP_OSCILLATOR_OPT_LEVEL },
    { MP_QSTR_pitch_octaves, AUDIODSP_OSCILLATOR_OPT_PITCH_OCTAVES },
    { MP_QSTR_fm_depth, AUDIODSP_OSCILLATOR_OPT_FM_DEPTH },
    { MP_QSTR_pwm_depth, AUDIODSP_OSCILLATOR_OPT_PWM_DEPTH },
};

static const qstr oscillator_input_names[AUDIODSP_OSCILLATOR_INPUTS] = {
    MP_QSTR_pitch, MP_QSTR_fm, MP_QSTR_pwm, MP_QSTR_sync,
};

static int oscillator_input_index(qstr name) {
    for (int index = 0; index < (int)AUDIODSP_OSCILLATOR_INPUTS; ++index) {
        if (oscillator_input_names[index] == name) {
            return index;
        }
    }
    return -1;
}

static bool oscillator_stream_keyword(qstr name) {
    return name == MP_QSTR_sample_rate || name == MP_QSTR_channel_count ||
           name == MP_QSTR_buffer_size;
}

// Every option is read and checked into a copy of the config before any of
// them is applied, so a bad keyword after a good one changes nothing.
static void oscillator_apply_kwargs(audiooscillator_oscillator_obj_t *self,
    const mp_map_t *kw, bool live) {
    audiodsp_oscillator_config_t next = self->config;
    for (size_t i = 0; i < kw->alloc; ++i) {
        if (!mp_map_slot_is_filled(kw, i)) {
            continue;
        }
        qstr name = mp_obj_str_get_qstr(kw->table[i].key);
        if ((!live && oscillator_stream_keyword(name)) ||
            (!live && oscillator_input_index(name) >= 0)) {
            continue;
        }
        bool known = false;
        for (size_t option = 0;
             option < MP_ARRAY_SIZE(oscillator_option_names); ++option) {
            if (oscillator_option_names[option].name == name) {
                audiodsp_oscillator_configure(&next,
                    oscillator_option_names[option].option,
                    (float)mp_obj_get_float(kw->table[i].value));
                known = true;
                break;
            }
        }
        if (!known) {
            mp_raise_msg_varg(&mp_type_TypeError,
                MP_ERROR_TEXT("unknown Oscillator option '%q'"), name);
        }
    }
    audiodsp_oscillator_config_finish(&next);
    (void)live;
    self->config = next;
}

// Connects every input `kw` names. Checked first, stored after, so a bad one
// leaves all of them as they were.
static void oscillator_apply_inputs(audiooscillator_oscillator_obj_t *self,
    const mp_map_t *kw, bool strict) {
    mp_obj_t chosen[AUDIODSP_OSCILLATOR_INPUTS];
    uint8_t channels[AUDIODSP_OSCILLATOR_INPUTS];
    bool named[AUDIODSP_OSCILLATOR_INPUTS] = { false, false, false, false };
    for (size_t i = 0; i < kw->alloc; ++i) {
        if (!mp_map_slot_is_filled(kw, i)) {
            continue;
        }
        qstr name = mp_obj_str_get_qstr(kw->table[i].key);
        int index = oscillator_input_index(name);
        if (index < 0) {
            if (strict) {
                mp_raise_msg_varg(&mp_type_TypeError,
                    MP_ERROR_TEXT("unknown Oscillator input '%q'"), name);
            }
            continue;
        }
        mp_obj_t sample = kw->table[i].value;
        named[index] = true;
        chosen[index] = MP_OBJ_NULL;
        channels[index] = 1;
        if (sample != mp_const_none) {
            audiosample_base_t *base = audiosample_check(sample);
            if (base->channel_count < 1 || base->channel_count > 2) {
                mp_raise_ValueError(
                    MP_ERROR_TEXT("an input must be mono or stereo"));
            }
            chosen[index] = sample;
            channels[index] = base->channel_count;
        }
    }
    for (int index = 0; index < (int)AUDIODSP_OSCILLATOR_INPUTS; ++index) {
        if (!named[index]) {
            continue;
        }
        self->input[index] = chosen[index];
        self->input_channels[index] = channels[index];
        self->pending[index] = NULL;
        self->pending_frames[index] = 0;
    }
}

static mp_obj_t audiooscillator_oscillator_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, true);
    mp_map_t kw_map;
    mp_map_init_fixed_table(&kw_map, n_kw, all_args + n_args);

    uint32_t sample_rate = 48000;
    uint32_t channel_count = 1;
    mp_obj_t buffer_size = mp_const_none;
    for (size_t i = 0; i < kw_map.alloc; ++i) {
        if (!mp_map_slot_is_filled(&kw_map, i)) {
            continue;
        }
        qstr name = mp_obj_str_get_qstr(kw_map.table[i].key);
        if (name == MP_QSTR_sample_rate) {
            sample_rate = (uint32_t)mp_obj_get_int(kw_map.table[i].value);
        } else if (name == MP_QSTR_channel_count) {
            channel_count = (uint32_t)mp_obj_get_int(kw_map.table[i].value);
        } else if (name == MP_QSTR_buffer_size) {
            buffer_size = kw_map.table[i].value;
        }
    }
    if (sample_rate < 1u) {
        mp_raise_ValueError(MP_ERROR_TEXT("sample_rate must be at least 1"));
    }
    if (channel_count < 1u || channel_count > 2u) {
        mp_raise_ValueError(MP_ERROR_TEXT("channel_count must be 1 or 2"));
    }
    uint32_t frames = AUDIODSP_OSCILLATOR_FRAMES;
    if (buffer_size != mp_const_none) {
        const mp_int_t bytes = mp_obj_get_int(buffer_size);
        const mp_int_t width = 2 * (mp_int_t)channel_count;
        if (bytes < width || bytes % width != 0 ||
            bytes > (mp_int_t)AUDIODSP_OSCILLATOR_FRAMES * width) {
            mp_raise_ValueError(MP_ERROR_TEXT(
                "buffer_size must be whole frames, from one to 256"));
        }
        frames = (uint32_t)(bytes / width);
    }

    audiooscillator_oscillator_obj_t *self =
        mp_obj_malloc(audiooscillator_oscillator_obj_t, type);
    self->base.sample_rate = sample_rate;
    self->base.max_buffer_length = frames * 2u * channel_count;
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)channel_count;
    self->base.samples_signed = 1;
    self->base.single_buffer = false;
    self->frames = frames;
    for (int index = 0; index < (int)AUDIODSP_OSCILLATOR_INPUTS; ++index) {
        self->input[index] = MP_OBJ_NULL;
        self->pending[index] = NULL;
        self->pending_frames[index] = 0;
        self->input_channels[index] = 1;
    }

    audiodsp_oscillator_config_init(&self->config, sample_rate);
    audiodsp_oscillator_set_channel_count(&self->config, channel_count);
    audiodsp_oscillator_state_init(&self->state);

    oscillator_apply_kwargs(self, &kw_map, false);
    oscillator_apply_inputs(self, &kw_map, false);
    return MP_OBJ_FROM_PTR(self);
}

//|     def set(self, **options: float) -> None:
//|         """Change settings mid-stream. The phase carries on."""
//|         ...
static mp_obj_t audiooscillator_oscillator_set(size_t n_args,
    const mp_obj_t *args, mp_map_t *kw_args) {
    audiooscillator_oscillator_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    audiosample_check_for_deinit(&self->base);
    (void)n_args;
    oscillator_apply_kwargs(self, kw_args, true);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(audiooscillator_oscillator_set_obj, 1,
    audiooscillator_oscillator_set);

//|     def modulate(self, **inputs: Optional[circuitpython_typing.AudioSample]) -> None:
//|         """Connect inputs by name: ``pitch``, ``fm``, ``pwm``, ``sync``.
//|         None disconnects one; an input not named is left as it is."""
//|         ...
static mp_obj_t audiooscillator_oscillator_modulate(size_t n_args,
    const mp_obj_t *args, mp_map_t *kw_args) {
    audiooscillator_oscillator_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    audiosample_check_for_deinit(&self->base);
    (void)n_args;
    oscillator_apply_inputs(self, kw_args, true);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(audiooscillator_oscillator_modulate_obj, 1,
    audiooscillator_oscillator_modulate);

//|     def clear(self) -> None:
//|         """Phase back to zero, with nothing left of the last edge."""
//|         ...
//|
//|
static mp_obj_t audiooscillator_oscillator_clear(mp_obj_t self_in) {
    audiooscillator_oscillator_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    audiodsp_oscillator_reset(&self->state);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiooscillator_oscillator_clear_obj,
    audiooscillator_oscillator_clear);

// `deinit()` releases what this binding holds and marks the node
// deinitialised, which is what makes every guarded entry point raise
// afterwards (audiodsp#58, #60, #63). What is cleared is what the object holds
// a reference to: the inputs, and every borrowed pointer into their buffers.
static mp_obj_t audiooscillator_oscillator_deinit(mp_obj_t self_in) {
    audiooscillator_oscillator_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    for (int index = 0; index < (int)AUDIODSP_OSCILLATOR_INPUTS; ++index) {
        self->input[index] = MP_OBJ_NULL;
        self->pending[index] = NULL;
        self->pending_frames[index] = 0;
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiooscillator_oscillator_deinit_obj,
    audiooscillator_oscillator_deinit);

static const mp_rom_map_elem_t audiooscillator_oscillator_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit),
      MP_ROM_PTR(&audiooscillator_oscillator_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_set),
      MP_ROM_PTR(&audiooscillator_oscillator_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_modulate),
      MP_ROM_PTR(&audiooscillator_oscillator_modulate_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear),
      MP_ROM_PTR(&audiooscillator_oscillator_clear_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiooscillator_oscillator_locals_dict,
    audiooscillator_oscillator_locals_dict_table);

static const audiosample_p_t audiooscillator_oscillator_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)
        audiooscillator_oscillator_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)
        audiooscillator_oscillator_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiooscillator_oscillator_type,
    MP_QSTR_Oscillator,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiooscillator_oscillator_make_new,
    locals_dict, &audiooscillator_oscillator_locals_dict,
    protocol, &audiooscillator_oscillator_proto
    );
