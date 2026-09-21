// Ported from CircuitPython's shared-bindings/synthio/Note.c and
// shared-module/synthio/Note.c (upstream repo:
// https://github.com/adafruit/circuitpython, MIT), merged into one file.
// Docstrings dropped. See docs/upstream-diff.md: every type using
// MP_PROPERTY_GETTER/GETSET needs `attr, cp_compat_attr` wired in.
//
// SPDX-FileCopyrightText: Copyright (c) 2023 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2021 Artyom Skrobov
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include <math.h>
#include <string.h>

#include "cp_compat/argcheck.h"
#include "cp_compat/objproperty.h"
#include "cp_compat/util.h"
#include "synthio/Note.h"
#include "shared/audiodsp_synth_dsp.h"
#include "synthio/__init__.h"

#include "py/runtime.h"

// --- from shared-module/synthio/Note.c ------------------------------------

mp_float_t common_hal_synthio_note_get_frequency(synthio_note_obj_t *self) {
    return self->frequency;
}

void common_hal_synthio_note_set_frequency(synthio_note_obj_t *self, mp_float_t value_in) {
    mp_float_t val = mp_arg_validate_float_range(value_in, 0, 32767, MP_QSTR_frequency);
    self->frequency = val;
    self->frequency_scaled = synthio_frequency_convert_float_to_scaled(val);
}

mp_obj_t common_hal_synthio_note_get_filter_obj(synthio_note_obj_t *self) {
    return self->filter_obj;
}

void common_hal_synthio_note_set_filter(synthio_note_obj_t *self, mp_obj_t filter_in) {
    // audiodsp extension (#11): also accept a tuple/list of Biquads - a
    // serial cascade of up to SYNTHIO_NOTE_MAX_FILTER_STAGES stages.
    if (filter_in != mp_const_none && !mp_obj_is_type(filter_in, &synthio_biquad_type_obj)) {
        if (mp_obj_is_type(filter_in, &mp_type_tuple) || mp_obj_is_type(filter_in, &mp_type_list)) {
            size_t len;
            mp_obj_t *items;
            mp_obj_get_array(filter_in, &len, &items);
            if (len > SYNTHIO_NOTE_MAX_FILTER_STAGES) {
                mp_raise_ValueError(MP_ERROR_TEXT("filter cascade too long"));
            }
            for (size_t i = 0; i < len; i++) {
                if (!mp_obj_is_type(items[i], &synthio_biquad_type_obj)) {
                    mp_raise_TypeError_varg(
                        MP_ERROR_TEXT("%q must be of type %q, not %q"),
                        MP_QSTR_filter, MP_QSTR_Biquad, mp_obj_get_type(items[i])->name);
                }
            }
        } else {
            mp_raise_TypeError_varg(
                MP_ERROR_TEXT("%q must be of type %q, not %q"),
                MP_QSTR_filter, MP_QSTR_Biquad, mp_obj_get_type(filter_in)->name);
        }
    }
    self->filter_obj = filter_in;
}

mp_float_t common_hal_synthio_note_get_ring_frequency(synthio_note_obj_t *self) {
    return self->ring_frequency;
}

void common_hal_synthio_note_set_ring_frequency(synthio_note_obj_t *self, mp_float_t value_in) {
    mp_float_t val = mp_arg_validate_float_range(value_in, 0, 32767, MP_QSTR_ring_frequency);
    self->ring_frequency = val;
    self->ring_frequency_scaled = synthio_frequency_convert_float_to_scaled(val);
}

mp_obj_t common_hal_synthio_note_get_panning(synthio_note_obj_t *self) {
    return self->panning.obj;
}

void common_hal_synthio_note_set_panning(synthio_note_obj_t *self, mp_obj_t value_in) {
    synthio_block_assign_slot(value_in, &self->panning, MP_QSTR_panning);
}

mp_obj_t common_hal_synthio_note_get_amplitude(synthio_note_obj_t *self) {
    return self->amplitude.obj;
}

void common_hal_synthio_note_set_amplitude(synthio_note_obj_t *self, mp_obj_t value_in) {
    synthio_block_assign_slot(value_in, &self->amplitude, MP_QSTR_amplitude);
}

mp_obj_t common_hal_synthio_note_get_bend(synthio_note_obj_t *self) {
    return self->bend.obj;
}

void common_hal_synthio_note_set_bend(synthio_note_obj_t *self, mp_obj_t value_in) {
    synthio_block_assign_slot(value_in, &self->bend, MP_QSTR_bend);
}

mp_obj_t common_hal_synthio_note_get_ring_bend(synthio_note_obj_t *self) {
    return self->ring_bend.obj;
}

void common_hal_synthio_note_set_ring_bend(synthio_note_obj_t *self, mp_obj_t value_in) {
    synthio_block_assign_slot(value_in, &self->ring_bend, MP_QSTR_ring_bend);
}

mp_obj_t common_hal_synthio_note_get_envelope_obj(synthio_note_obj_t *self) {
    return self->envelope_obj;
}

void common_hal_synthio_note_set_envelope(synthio_note_obj_t *self, mp_obj_t envelope_in) {
    if (envelope_in != mp_const_none) {
        mp_arg_validate_type(envelope_in, (mp_obj_type_t *)&synthio_envelope_type_obj, MP_QSTR_envelope);
        if (self->sample_rate != 0) {
            synthio_envelope_definition_set(&self->envelope_def, envelope_in, self->sample_rate);
        }
    }
    self->envelope_obj = envelope_in;
}

mp_obj_t common_hal_synthio_note_get_waveform_obj(synthio_note_obj_t *self) {
    return self->waveform_obj;
}

void common_hal_synthio_note_set_waveform(synthio_note_obj_t *self, mp_obj_t waveform_in) {
    if (waveform_in == mp_const_none) {
        memset(&self->waveform_buf, 0, sizeof(self->waveform_buf));
    } else {
        mp_buffer_info_t bufinfo_waveform;
        synthio_synth_parse_waveform(&bufinfo_waveform, waveform_in);
        self->waveform_buf = bufinfo_waveform;
    }
    self->waveform_obj = waveform_in;
}

mp_obj_t common_hal_synthio_note_get_waveform_loop_start(synthio_note_obj_t *self) {
    return self->waveform_loop_start.obj;
}

void common_hal_synthio_note_set_waveform_loop_start(synthio_note_obj_t *self, mp_obj_t value_in) {
    synthio_block_assign_slot(value_in, &self->waveform_loop_start, MP_QSTR_waveform_loop_start);
}

mp_obj_t common_hal_synthio_note_get_waveform_loop_end(synthio_note_obj_t *self) {
    return self->waveform_loop_end.obj;
}

void common_hal_synthio_note_set_waveform_loop_end(synthio_note_obj_t *self, mp_obj_t value_in) {
    synthio_block_assign_slot(value_in, &self->waveform_loop_end, MP_QSTR_waveform_loop_end);
}

mp_obj_t common_hal_synthio_note_get_ring_waveform_obj(synthio_note_obj_t *self) {
    return self->ring_waveform_obj;
}

void common_hal_synthio_note_set_ring_waveform(synthio_note_obj_t *self, mp_obj_t ring_waveform_in) {
    if (ring_waveform_in == mp_const_none) {
        memset(&self->ring_waveform_buf, 0, sizeof(self->ring_waveform_buf));
    } else {
        mp_buffer_info_t bufinfo_ring_waveform;
        synthio_synth_parse_waveform(&bufinfo_ring_waveform, ring_waveform_in);
        self->ring_waveform_buf = bufinfo_ring_waveform;
    }
    self->ring_waveform_obj = ring_waveform_in;
}

mp_obj_t common_hal_synthio_note_get_ring_waveform_loop_start(synthio_note_obj_t *self) {
    return self->ring_waveform_loop_start.obj;
}

void common_hal_synthio_note_set_ring_waveform_loop_start(synthio_note_obj_t *self, mp_obj_t value_in) {
    synthio_block_assign_slot(value_in, &self->ring_waveform_loop_start, MP_QSTR_ring_waveform_loop_start);
}

mp_obj_t common_hal_synthio_note_get_ring_waveform_loop_end(synthio_note_obj_t *self) {
    return self->ring_waveform_loop_end.obj;
}

void common_hal_synthio_note_set_ring_waveform_loop_end(synthio_note_obj_t *self, mp_obj_t value_in) {
    synthio_block_assign_slot(value_in, &self->ring_waveform_loop_end, MP_QSTR_ring_waveform_loop_end);
}

void synthio_note_recalculate(synthio_note_obj_t *self, int32_t sample_rate) {
    if (sample_rate == self->sample_rate) {
        return;
    }
    self->sample_rate = sample_rate;

    if (self->envelope_obj != mp_const_none) {
        synthio_envelope_definition_set(&self->envelope_def, self->envelope_obj, sample_rate);
    }
}

void synthio_note_start(synthio_note_obj_t *self, int32_t sample_rate) {
    synthio_note_recalculate(self, sample_rate);
    for (size_t i = 0; i < SYNTHIO_NOTE_MAX_FILTER_STAGES; i++) {
        synthio_biquad_filter_reset(&self->filter_state[i]);
    }
}

// Perform a pitch bend operation
//
// bend_value is in the range [0, 65535]. "no change" is 32768. The bend unit is 32768/octave.
//
// compare to (frequency_scaled * pow(2, (bend_value-32768)/32768))
// a 13-entry pitch table
#define BEND_SCALE (32768)
#define BEND_OFFSET (BEND_SCALE)

static uint32_t pitch_bend(uint32_t frequency_scaled, int32_t bend_value) {
    return audiodsp_pitch_bend(frequency_scaled, bend_value);
}

#define ZERO MICROPY_FLOAT_CONST(0.)
#define ONE MICROPY_FLOAT_CONST(1.)
#define ALMOST_ONE (MICROPY_FLOAT_CONST(32767.) / 32768)

uint32_t synthio_note_step(synthio_note_obj_t *self, int32_t sample_rate, int16_t dur, int16_t loudness[2]) {
    int panning = synthio_block_slot_get_scaled(&self->panning, -ALMOST_ONE, ALMOST_ONE);
    // panning > 0 attenuates the LEFT channel, matching audiomixer.Mixer.
    // CircuitPython 10.3.0 flipped this (shared-module/synthio/Note.c); before
    // it, synthio and Mixer panned in opposite directions.
    int left_panning_scaled = 32768, right_panning_scaled = 32768;
    if (panning >= 0) {
        left_panning_scaled = 32767 - panning;
    } else {
        right_panning_scaled = 32767 + panning;
    }

    int amplitude = synthio_block_slot_get_scaled(&self->amplitude, -ALMOST_ONE, ALMOST_ONE);
    left_panning_scaled = (left_panning_scaled * amplitude) >> 15;
    right_panning_scaled = (right_panning_scaled * amplitude) >> 15;
    loudness[0] = (loudness[0] * left_panning_scaled) >> 15;
    loudness[1] = (loudness[1] * right_panning_scaled) >> 15;

    if (self->ring_frequency_scaled != 0) {
        int ring_bend_value = synthio_block_slot_get_scaled(&self->ring_bend, -12, 12);
        self->ring_frequency_bent = pitch_bend(self->ring_frequency_scaled, ring_bend_value);
    }

    int bend_value = synthio_block_slot_get_scaled(&self->bend, -12, 12);
    uint32_t frequency_scaled = pitch_bend(self->frequency_scaled, bend_value);
    return frequency_scaled;

}

// --- from shared-bindings/synthio/Note.c ----------------------------------

static const mp_arg_t note_properties[] = {
    { MP_QSTR_frequency, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = NULL } },
    { MP_QSTR_panning, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(0) } },
    { MP_QSTR_amplitude, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(1) } },
    { MP_QSTR_bend, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(0) } },
    { MP_QSTR_waveform, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_NONE } },
    { MP_QSTR_waveform_loop_start, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(0) } },
    { MP_QSTR_waveform_loop_end, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(SYNTHIO_WAVEFORM_SIZE) } },
    { MP_QSTR_envelope, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_NONE } },
    { MP_QSTR_filter, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_NONE } },
    { MP_QSTR_ring_frequency, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(0) } },
    { MP_QSTR_ring_bend, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(0) } },
    { MP_QSTR_ring_waveform, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_NONE } },
    { MP_QSTR_ring_waveform_loop_start, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(0) } },
    { MP_QSTR_ring_waveform_loop_end, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(SYNTHIO_WAVEFORM_SIZE) } },
};

static mp_obj_t synthio_note_make_new(const mp_obj_type_t *type_in, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_val_t args[MP_ARRAY_SIZE(note_properties)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(note_properties), note_properties, args);

    synthio_note_obj_t *self = mp_obj_malloc(synthio_note_obj_t, &synthio_note_type);
    mp_obj_t self_obj = MP_OBJ_FROM_PTR(self);

    properties_construct_helper(self_obj, note_properties, args, MP_ARRAY_SIZE(note_properties));

    return self_obj;
};

static mp_obj_t synthio_note_get_frequency(mp_obj_t self_in) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_float(common_hal_synthio_note_get_frequency(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_note_get_frequency_obj, synthio_note_get_frequency);

static mp_obj_t synthio_note_set_frequency(mp_obj_t self_in, mp_obj_t arg) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_note_set_frequency(self, mp_obj_get_float(arg));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_note_set_frequency_obj, synthio_note_set_frequency);
MP_PROPERTY_GETSET(synthio_note_frequency_obj,
    (mp_obj_t)&synthio_note_get_frequency_obj,
    (mp_obj_t)&synthio_note_set_frequency_obj);

static mp_obj_t synthio_note_get_filter(mp_obj_t self_in) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_note_get_filter_obj(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_note_get_filter_obj, synthio_note_get_filter);

static mp_obj_t synthio_note_set_filter(mp_obj_t self_in, mp_obj_t arg) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_note_set_filter(self, arg);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_note_set_filter_obj, synthio_note_set_filter);
MP_PROPERTY_GETSET(synthio_note_filter_obj,
    (mp_obj_t)&synthio_note_get_filter_obj,
    (mp_obj_t)&synthio_note_set_filter_obj);

static mp_obj_t synthio_note_get_panning(mp_obj_t self_in) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_note_get_panning(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_note_get_panning_obj, synthio_note_get_panning);

static mp_obj_t synthio_note_set_panning(mp_obj_t self_in, mp_obj_t arg) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_note_set_panning(self, arg);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_note_set_panning_obj, synthio_note_set_panning);
MP_PROPERTY_GETSET(synthio_note_panning_obj,
    (mp_obj_t)&synthio_note_get_panning_obj,
    (mp_obj_t)&synthio_note_set_panning_obj);

static mp_obj_t synthio_note_get_amplitude(mp_obj_t self_in) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_note_get_amplitude(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_note_get_amplitude_obj, synthio_note_get_amplitude);

static mp_obj_t synthio_note_set_amplitude(mp_obj_t self_in, mp_obj_t arg) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_note_set_amplitude(self, arg);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_note_set_amplitude_obj, synthio_note_set_amplitude);
MP_PROPERTY_GETSET(synthio_note_amplitude_obj,
    (mp_obj_t)&synthio_note_get_amplitude_obj,
    (mp_obj_t)&synthio_note_set_amplitude_obj);

static mp_obj_t synthio_note_get_bend(mp_obj_t self_in) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_note_get_bend(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_note_get_bend_obj, synthio_note_get_bend);

static mp_obj_t synthio_note_set_bend(mp_obj_t self_in, mp_obj_t arg) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_note_set_bend(self, arg);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_note_set_bend_obj, synthio_note_set_bend);
MP_PROPERTY_GETSET(synthio_note_bend_obj,
    (mp_obj_t)&synthio_note_get_bend_obj,
    (mp_obj_t)&synthio_note_set_bend_obj);

static mp_obj_t synthio_note_get_waveform(mp_obj_t self_in) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_note_get_waveform_obj(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_note_get_waveform_obj, synthio_note_get_waveform);

static mp_obj_t synthio_note_set_waveform(mp_obj_t self_in, mp_obj_t arg) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_note_set_waveform(self, arg);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_note_set_waveform_obj, synthio_note_set_waveform);
MP_PROPERTY_GETSET(synthio_note_waveform_obj,
    (mp_obj_t)&synthio_note_get_waveform_obj,
    (mp_obj_t)&synthio_note_set_waveform_obj);

static mp_obj_t synthio_note_get_waveform_loop_start(mp_obj_t self_in) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_note_get_waveform_loop_start(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_note_get_waveform_loop_start_obj, synthio_note_get_waveform_loop_start);

static mp_obj_t synthio_note_set_waveform_loop_start(mp_obj_t self_in, mp_obj_t arg) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_note_set_waveform_loop_start(self, arg);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_note_set_waveform_loop_start_obj, synthio_note_set_waveform_loop_start);
MP_PROPERTY_GETSET(synthio_note_waveform_loop_start_obj,
    (mp_obj_t)&synthio_note_get_waveform_loop_start_obj,
    (mp_obj_t)&synthio_note_set_waveform_loop_start_obj);

static mp_obj_t synthio_note_get_waveform_loop_end(mp_obj_t self_in) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_note_get_waveform_loop_end(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_note_get_waveform_loop_end_obj, synthio_note_get_waveform_loop_end);

static mp_obj_t synthio_note_set_waveform_loop_end(mp_obj_t self_in, mp_obj_t arg) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_note_set_waveform_loop_end(self, arg);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_note_set_waveform_loop_end_obj, synthio_note_set_waveform_loop_end);
MP_PROPERTY_GETSET(synthio_note_waveform_loop_end_obj,
    (mp_obj_t)&synthio_note_get_waveform_loop_end_obj,
    (mp_obj_t)&synthio_note_set_waveform_loop_end_obj);

static mp_obj_t synthio_note_get_envelope(mp_obj_t self_in) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_note_get_envelope_obj(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_note_get_envelope_obj, synthio_note_get_envelope);

static mp_obj_t synthio_note_set_envelope(mp_obj_t self_in, mp_obj_t arg) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_note_set_envelope(self, arg);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_note_set_envelope_obj, synthio_note_set_envelope);
MP_PROPERTY_GETSET(synthio_note_envelope_obj,
    (mp_obj_t)&synthio_note_get_envelope_obj,
    (mp_obj_t)&synthio_note_set_envelope_obj);

static mp_obj_t synthio_note_get_ring_frequency(mp_obj_t self_in) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_float(common_hal_synthio_note_get_ring_frequency(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_note_get_ring_frequency_obj, synthio_note_get_ring_frequency);

static mp_obj_t synthio_note_set_ring_frequency(mp_obj_t self_in, mp_obj_t arg) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_note_set_ring_frequency(self, mp_obj_get_float(arg));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_note_set_ring_frequency_obj, synthio_note_set_ring_frequency);
MP_PROPERTY_GETSET(synthio_note_ring_frequency_obj,
    (mp_obj_t)&synthio_note_get_ring_frequency_obj,
    (mp_obj_t)&synthio_note_set_ring_frequency_obj);

static mp_obj_t synthio_note_get_ring_bend(mp_obj_t self_in) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_note_get_ring_bend(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_note_get_ring_bend_obj, synthio_note_get_ring_bend);

static mp_obj_t synthio_note_set_ring_bend(mp_obj_t self_in, mp_obj_t arg) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_note_set_ring_bend(self, arg);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_note_set_ring_bend_obj, synthio_note_set_ring_bend);
MP_PROPERTY_GETSET(synthio_note_ring_bend_obj,
    (mp_obj_t)&synthio_note_get_ring_bend_obj,
    (mp_obj_t)&synthio_note_set_ring_bend_obj);

static mp_obj_t synthio_note_get_ring_waveform(mp_obj_t self_in) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_note_get_ring_waveform_obj(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_note_get_ring_waveform_obj, synthio_note_get_ring_waveform);

static mp_obj_t synthio_note_set_ring_waveform(mp_obj_t self_in, mp_obj_t arg) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_note_set_ring_waveform(self, arg);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_note_set_ring_waveform_obj, synthio_note_set_ring_waveform);
MP_PROPERTY_GETSET(synthio_note_ring_waveform_obj,
    (mp_obj_t)&synthio_note_get_ring_waveform_obj,
    (mp_obj_t)&synthio_note_set_ring_waveform_obj);

static mp_obj_t synthio_note_get_ring_waveform_loop_start(mp_obj_t self_in) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_note_get_ring_waveform_loop_start(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_note_get_ring_waveform_loop_start_obj, synthio_note_get_ring_waveform_loop_start);

static mp_obj_t synthio_note_set_ring_waveform_loop_start(mp_obj_t self_in, mp_obj_t arg) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_note_set_ring_waveform_loop_start(self, arg);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_note_set_ring_waveform_loop_start_obj, synthio_note_set_ring_waveform_loop_start);
MP_PROPERTY_GETSET(synthio_note_ring_waveform_loop_start_obj,
    (mp_obj_t)&synthio_note_get_ring_waveform_loop_start_obj,
    (mp_obj_t)&synthio_note_set_ring_waveform_loop_start_obj);

static mp_obj_t synthio_note_get_ring_waveform_loop_end(mp_obj_t self_in) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_note_get_ring_waveform_loop_end(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_note_get_ring_waveform_loop_end_obj, synthio_note_get_ring_waveform_loop_end);

static mp_obj_t synthio_note_set_ring_waveform_loop_end(mp_obj_t self_in, mp_obj_t arg) {
    synthio_note_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_note_set_ring_waveform_loop_end(self, arg);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_note_set_ring_waveform_loop_end_obj, synthio_note_set_ring_waveform_loop_end);
MP_PROPERTY_GETSET(synthio_note_ring_waveform_loop_end_obj,
    (mp_obj_t)&synthio_note_get_ring_waveform_loop_end_obj,
    (mp_obj_t)&synthio_note_set_ring_waveform_loop_end_obj);

static void note_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    properties_print_helper(print, self_in, note_properties, MP_ARRAY_SIZE(note_properties));
}

static const mp_rom_map_elem_t synthio_note_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_frequency), MP_ROM_PTR(&synthio_note_frequency_obj) },
    { MP_ROM_QSTR(MP_QSTR_filter), MP_ROM_PTR(&synthio_note_filter_obj) },
    { MP_ROM_QSTR(MP_QSTR_panning), MP_ROM_PTR(&synthio_note_panning_obj) },
    { MP_ROM_QSTR(MP_QSTR_waveform), MP_ROM_PTR(&synthio_note_waveform_obj) },
    { MP_ROM_QSTR(MP_QSTR_waveform_loop_start), MP_ROM_PTR(&synthio_note_waveform_loop_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_waveform_loop_end), MP_ROM_PTR(&synthio_note_waveform_loop_end_obj) },
    { MP_ROM_QSTR(MP_QSTR_envelope), MP_ROM_PTR(&synthio_note_envelope_obj) },
    { MP_ROM_QSTR(MP_QSTR_amplitude), MP_ROM_PTR(&synthio_note_amplitude_obj) },
    { MP_ROM_QSTR(MP_QSTR_bend), MP_ROM_PTR(&synthio_note_bend_obj) },
    { MP_ROM_QSTR(MP_QSTR_ring_frequency), MP_ROM_PTR(&synthio_note_ring_frequency_obj) },
    { MP_ROM_QSTR(MP_QSTR_ring_bend), MP_ROM_PTR(&synthio_note_ring_bend_obj) },
    { MP_ROM_QSTR(MP_QSTR_ring_waveform), MP_ROM_PTR(&synthio_note_ring_waveform_obj) },
    { MP_ROM_QSTR(MP_QSTR_ring_waveform_loop_start), MP_ROM_PTR(&synthio_note_ring_waveform_loop_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_ring_waveform_loop_end), MP_ROM_PTR(&synthio_note_ring_waveform_loop_end_obj) },
};
static MP_DEFINE_CONST_DICT(synthio_note_locals_dict, synthio_note_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    synthio_note_type,
    MP_QSTR_Note,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, synthio_note_make_new,
    attr, cp_compat_attr,
    locals_dict, &synthio_note_locals_dict,
    print, note_print
    );
