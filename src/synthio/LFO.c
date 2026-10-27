// Ported from CircuitPython's shared-bindings/synthio/LFO.c and
// shared-module/synthio/LFO.c (upstream repo:
// https://github.com/adafruit/circuitpython, MIT), merged into one file.
// Docstrings dropped. See docs/how-it-works/upstream-diff.md: every type using
// MP_PROPERTY_GETTER/GETSET needs `attr, cp_compat_attr` wired in.
//
// SPDX-FileCopyrightText: Copyright (c) 2023 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2021 Artyom Skrobov
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include <math.h>

#include "cp_compat/argcheck.h"
#include "cp_compat/objproperty.h"
#include "cp_compat/util.h"
#include "synthio/LFO.h"
#include "synthio/__init__.h"

#include "py/obj.h"
#include "py/runtime.h"

// --- from shared-module/synthio/LFO.c -------------------------------------

#define ONE (MICROPY_FLOAT_CONST(1.))
#define ZERO (MICROPY_FLOAT_CONST(0.))

#define ALMOST_ONE (MICROPY_FLOAT_CONST(32767.) / 32768)

mp_float_t common_hal_synthio_lfo_tick(mp_obj_t self_in) {
    synthio_lfo_obj_t *lfo = MP_OBJ_TO_PTR(self_in);

    mp_float_t rate = synthio_block_slot_get(&lfo->rate) * synthio_global_rate_scale;
    mp_float_t phase_offset = synthio_block_slot_get(&lfo->phase_offset);

    mp_float_t accum = lfo->accum + rate + phase_offset;

    if (lfo->once) {
        if (rate > 0) {
            if (accum > ALMOST_ONE) {
                accum = ALMOST_ONE;
            }
        } else if (rate < 0 && accum < ZERO) {
            accum = ZERO;
        }
    } else {
        accum = accum - MICROPY_FLOAT_C_FUN(floor)(accum);
    }
    lfo->accum = accum - phase_offset;

    int len = lfo->waveform_bufinfo.len;

    mp_float_t scaled_accum = accum * (len - lfo->once);
    size_t idx = (size_t)MICROPY_FLOAT_C_FUN(floor)(scaled_accum);
    assert(idx < lfo->waveform_bufinfo.len);

    int16_t *waveform = lfo->waveform_bufinfo.buf;
    mp_float_t value = waveform[idx];

    if (lfo->interpolate) {

        mp_float_t frac = scaled_accum - idx;

        size_t idxp1 = idx + 1;
        if (idxp1 == lfo->waveform_bufinfo.len) {
            idxp1 = lfo->once ? idx : 0;
        }
        value = value * (1 - frac) + waveform[idxp1] * frac;
    }

    mp_float_t scale = synthio_block_slot_get(&lfo->scale);
    mp_float_t offset = synthio_block_slot_get(&lfo->offset);
    value = MICROPY_FLOAT_C_FUN(ldexp)(value, -15) * scale + offset;

    return value;
}

mp_obj_t common_hal_synthio_lfo_get_waveform_obj(synthio_lfo_obj_t *self) {
    return self->waveform_obj;
}

mp_obj_t common_hal_synthio_lfo_get_rate_obj(synthio_lfo_obj_t *self) {
    return self->rate.obj;
}

void common_hal_synthio_lfo_set_rate_obj(synthio_lfo_obj_t *self, mp_obj_t arg) {
    synthio_block_assign_slot(arg, &self->rate, MP_QSTR_rate);
}

mp_obj_t common_hal_synthio_lfo_get_scale_obj(synthio_lfo_obj_t *self) {
    return self->scale.obj;
}
void common_hal_synthio_lfo_set_scale_obj(synthio_lfo_obj_t *self, mp_obj_t arg) {
    synthio_block_assign_slot(arg, &self->scale, MP_QSTR_scale);
}

mp_obj_t common_hal_synthio_lfo_get_phase_offset_obj(synthio_lfo_obj_t *self) {
    return self->phase_offset.obj;
}
void common_hal_synthio_lfo_set_phase_offset_obj(synthio_lfo_obj_t *self, mp_obj_t arg) {
    synthio_block_assign_slot(arg, &self->phase_offset, MP_QSTR_phase_offset);
}

mp_obj_t common_hal_synthio_lfo_get_offset_obj(synthio_lfo_obj_t *self) {
    return self->offset.obj;
}
void common_hal_synthio_lfo_set_offset_obj(synthio_lfo_obj_t *self, mp_obj_t arg) {
    synthio_block_assign_slot(arg, &self->offset, MP_QSTR_offset);
}

bool common_hal_synthio_lfo_get_once(synthio_lfo_obj_t *self) {
    return self->once;
}
void common_hal_synthio_lfo_set_once(synthio_lfo_obj_t *self, bool arg) {
    self->once = arg;
}

bool common_hal_synthio_lfo_get_interpolate(synthio_lfo_obj_t *self) {
    return self->interpolate;
}
void common_hal_synthio_lfo_set_interpolate(synthio_lfo_obj_t *self, bool arg) {
    self->interpolate = arg;
}

mp_float_t common_hal_synthio_lfo_get_value(synthio_lfo_obj_t *self) {
    return self->base.value;
}

mp_float_t common_hal_synthio_lfo_get_phase(synthio_lfo_obj_t *self) {
    return self->accum;
}

void common_hal_synthio_lfo_retrigger(synthio_lfo_obj_t *self) {
    self->accum = 0;
}

// --- from shared-bindings/synthio/LFO.c -----------------------------------

static const uint16_t triangle[] = {0, 32767, 0, -32767};

static const mp_arg_t lfo_properties[] = {
    { MP_QSTR_waveform, MP_ARG_OBJ, {.u_obj = MP_ROM_NONE } },
    { MP_QSTR_rate, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(1) } },
    { MP_QSTR_scale, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(1) } },
    { MP_QSTR_offset, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(0) } },
    { MP_QSTR_phase_offset, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(0) } },
    { MP_QSTR_once, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(0) } },
    { MP_QSTR_interpolate, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_ROM_INT(1) } },
};

static mp_obj_t synthio_lfo_make_new(const mp_obj_type_t *type_in, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_waveform }; // others never directly referred to by argument number

    mp_arg_val_t args[MP_ARRAY_SIZE(lfo_properties)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(lfo_properties), lfo_properties, args);

    synthio_lfo_obj_t *self = mp_obj_malloc(synthio_lfo_obj_t, &synthio_lfo_type);

    self->waveform_bufinfo = ((mp_buffer_info_t) {.buf = (void *)triangle, .len = MP_ARRAY_SIZE(triangle)});
    if (args[ARG_waveform].u_obj != mp_const_none) {
        synthio_synth_parse_waveform(&self->waveform_bufinfo, args[ARG_waveform].u_obj);
    }
    self->waveform_obj = args[ARG_waveform].u_obj;

    mp_obj_t self_obj = MP_OBJ_FROM_PTR(self);
    properties_construct_helper(self_obj, lfo_properties + 1, args + 1, MP_ARRAY_SIZE(lfo_properties) - 1);

    // Force computation of the LFO's initial output
    synthio_global_rate_scale = 0;
    self->base.last_tick = synthio_global_tick - 1;
    synthio_block_slot_t slot;
    synthio_block_assign_slot(self_obj, &slot, MP_QSTR_self);
    (void)synthio_block_slot_get(&slot);

    return self_obj;
};

static mp_obj_t synthio_lfo_get_waveform(mp_obj_t self_in) {
    synthio_lfo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_lfo_get_waveform_obj(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_lfo_get_waveform_obj, synthio_lfo_get_waveform);

MP_PROPERTY_GETTER(synthio_lfo_waveform_obj,
    (mp_obj_t)&synthio_lfo_get_waveform_obj);

static mp_obj_t synthio_lfo_get_rate(mp_obj_t self_in) {
    synthio_lfo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_lfo_get_rate_obj(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_lfo_get_rate_obj, synthio_lfo_get_rate);

static mp_obj_t synthio_lfo_set_rate(mp_obj_t self_in, mp_obj_t arg) {
    synthio_lfo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_lfo_set_rate_obj(self, arg);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_lfo_set_rate_obj, synthio_lfo_set_rate);
MP_PROPERTY_GETSET(synthio_lfo_rate_obj,
    (mp_obj_t)&synthio_lfo_get_rate_obj,
    (mp_obj_t)&synthio_lfo_set_rate_obj);

static mp_obj_t synthio_lfo_get_offset(mp_obj_t self_in) {
    synthio_lfo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_lfo_get_offset_obj(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_lfo_get_offset_obj, synthio_lfo_get_offset);

static mp_obj_t synthio_lfo_set_offset(mp_obj_t self_in, mp_obj_t arg) {
    synthio_lfo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_lfo_set_offset_obj(self, arg);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_lfo_set_offset_obj, synthio_lfo_set_offset);
MP_PROPERTY_GETSET(synthio_lfo_offset_obj,
    (mp_obj_t)&synthio_lfo_get_offset_obj,
    (mp_obj_t)&synthio_lfo_set_offset_obj);

static mp_obj_t synthio_lfo_get_phase_offset(mp_obj_t self_in) {
    synthio_lfo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_lfo_get_phase_offset_obj(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_lfo_get_phase_offset_obj, synthio_lfo_get_phase_offset);

static mp_obj_t synthio_lfo_set_phase_offset(mp_obj_t self_in, mp_obj_t arg) {
    synthio_lfo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_lfo_set_phase_offset_obj(self, arg);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_lfo_set_phase_offset_obj, synthio_lfo_set_phase_offset);
MP_PROPERTY_GETSET(synthio_lfo_phase_offset_obj,
    (mp_obj_t)&synthio_lfo_get_phase_offset_obj,
    (mp_obj_t)&synthio_lfo_set_phase_offset_obj);

static mp_obj_t synthio_lfo_get_scale(mp_obj_t self_in) {
    synthio_lfo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_lfo_get_scale_obj(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_lfo_get_scale_obj, synthio_lfo_get_scale);

static mp_obj_t synthio_lfo_set_scale(mp_obj_t self_in, mp_obj_t arg) {
    synthio_lfo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_lfo_set_scale_obj(self, arg);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_lfo_set_scale_obj, synthio_lfo_set_scale);
MP_PROPERTY_GETSET(synthio_lfo_scale_obj,
    (mp_obj_t)&synthio_lfo_get_scale_obj,
    (mp_obj_t)&synthio_lfo_set_scale_obj);

static mp_obj_t synthio_lfo_get_once(mp_obj_t self_in) {
    synthio_lfo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(common_hal_synthio_lfo_get_once(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_lfo_get_once_obj, synthio_lfo_get_once);

static mp_obj_t synthio_lfo_set_once(mp_obj_t self_in, mp_obj_t arg) {
    synthio_lfo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_lfo_set_once(self, mp_obj_is_true(arg));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_lfo_set_once_obj, synthio_lfo_set_once);
MP_PROPERTY_GETSET(synthio_lfo_once_obj,
    (mp_obj_t)&synthio_lfo_get_once_obj,
    (mp_obj_t)&synthio_lfo_set_once_obj);

static mp_obj_t synthio_lfo_get_interpolate(mp_obj_t self_in) {
    synthio_lfo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(common_hal_synthio_lfo_get_interpolate(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_lfo_get_interpolate_obj, synthio_lfo_get_interpolate);

static mp_obj_t synthio_lfo_set_interpolate(mp_obj_t self_in, mp_obj_t arg) {
    synthio_lfo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_lfo_set_interpolate(self, mp_obj_is_true(arg));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_lfo_set_interpolate_obj, synthio_lfo_set_interpolate);
MP_PROPERTY_GETSET(synthio_lfo_interpolate_obj,
    (mp_obj_t)&synthio_lfo_get_interpolate_obj,
    (mp_obj_t)&synthio_lfo_set_interpolate_obj);

static mp_obj_t synthio_lfo_get_phase(mp_obj_t self_in) {
    synthio_lfo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_float(common_hal_synthio_lfo_get_phase(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_lfo_get_phase_obj, synthio_lfo_get_phase);

MP_PROPERTY_GETTER(synthio_lfo_phase_obj,
    (mp_obj_t)&synthio_lfo_get_phase_obj);

static mp_obj_t synthio_lfo_get_value(mp_obj_t self_in) {
    synthio_lfo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_float(common_hal_synthio_lfo_get_value(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_lfo_get_value_obj, synthio_lfo_get_value);

MP_PROPERTY_GETTER(synthio_lfo_value_obj,
    (mp_obj_t)&synthio_lfo_get_value_obj);

static mp_obj_t synthio_lfo_retrigger(mp_obj_t self_in) {
    synthio_lfo_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_lfo_retrigger(self);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_lfo_retrigger_obj, synthio_lfo_retrigger);

static void lfo_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    properties_print_helper(print, self_in, lfo_properties, MP_ARRAY_SIZE(lfo_properties));
}

static const mp_rom_map_elem_t synthio_lfo_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_waveform), MP_ROM_PTR(&synthio_lfo_waveform_obj) },
    { MP_ROM_QSTR(MP_QSTR_rate), MP_ROM_PTR(&synthio_lfo_rate_obj) },
    { MP_ROM_QSTR(MP_QSTR_scale), MP_ROM_PTR(&synthio_lfo_scale_obj) },
    { MP_ROM_QSTR(MP_QSTR_offset), MP_ROM_PTR(&synthio_lfo_offset_obj) },
    { MP_ROM_QSTR(MP_QSTR_phase_offset), MP_ROM_PTR(&synthio_lfo_phase_offset_obj) },
    { MP_ROM_QSTR(MP_QSTR_once), MP_ROM_PTR(&synthio_lfo_once_obj) },
    { MP_ROM_QSTR(MP_QSTR_interpolate), MP_ROM_PTR(&synthio_lfo_interpolate_obj) },
    { MP_ROM_QSTR(MP_QSTR_value), MP_ROM_PTR(&synthio_lfo_value_obj) },
    { MP_ROM_QSTR(MP_QSTR_phase), MP_ROM_PTR(&synthio_lfo_phase_obj) },
    { MP_ROM_QSTR(MP_QSTR_retrigger), MP_ROM_PTR(&synthio_lfo_retrigger_obj) },
};
static MP_DEFINE_CONST_DICT(synthio_lfo_locals_dict, synthio_lfo_locals_dict_table);

static const synthio_block_proto_t lfo_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_synthio_block)
    .tick = common_hal_synthio_lfo_tick,
};

MP_DEFINE_CONST_OBJ_TYPE(
    synthio_lfo_type,
    MP_QSTR_LFO,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, synthio_lfo_make_new,
    attr, cp_compat_attr,
    locals_dict, &synthio_lfo_locals_dict,
    print, lfo_print,
    protocol, &lfo_proto
    );
