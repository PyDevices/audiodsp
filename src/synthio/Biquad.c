// Ported from CircuitPython's shared-bindings/synthio/Biquad.c and
// shared-module/synthio/Biquad.c (upstream repo:
// https://github.com/adafruit/circuitpython, MIT), merged into one file.
// Docstrings dropped. See docs/upstream-diff.md: every type using
// MP_PROPERTY_GETTER/GETSET needs `attr, cp_compat_attr` wired in.
//
// SPDX-FileCopyrightText: Copyright (c) 2023 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "cp_compat/enum.h"
#include "cp_compat/objproperty.h"
#include "cp_compat/util.h"
#include "shared/audiodsp_biquad.h"
#include "synthio/Biquad.h"
#include "synthio/__init__.h"

#include "py/runtime.h"

// --- from shared-module/synthio/Biquad.c ----------------------------------

mp_obj_t common_hal_synthio_biquad_new(synthio_filter_mode mode) {
    synthio_biquad_t *self = mp_obj_malloc(synthio_biquad_t, &synthio_biquad_type_obj);
    self->mode = mode;
    // Every path into the filter ticks first, which fills these in; a Biquad
    // that somehow reaches the DSP untouched should be a pass-through rather
    // than a shift of zero.
    audiodsp_biquad_cp_init(&self->coefficients);
    return MP_OBJ_FROM_PTR(self);
}

synthio_filter_mode common_hal_synthio_biquad_get_mode(synthio_biquad_t *self) {
    return self->mode;
}

mp_obj_t common_hal_synthio_biquad_get_Q(synthio_biquad_t *self) {
    return self->Q.obj;
}

void common_hal_synthio_biquad_set_Q(synthio_biquad_t *self, mp_obj_t Q) {
    synthio_block_assign_slot(Q, &self->Q, MP_QSTR_Q);
}

mp_obj_t common_hal_synthio_biquad_get_A(synthio_biquad_t *self) {
    return self->A.obj;
}

void common_hal_synthio_biquad_set_A(synthio_biquad_t *self, mp_obj_t A) {
    synthio_block_assign_slot(A, &self->A, MP_QSTR_A);
}

mp_obj_t common_hal_synthio_biquad_get_frequency(synthio_biquad_t *self) {
    return self->f0.obj;
}

void common_hal_synthio_biquad_set_frequency(synthio_biquad_t *self, mp_obj_t frequency) {
    synthio_block_assign_slot(frequency, &self->f0, MP_QSTR_frequency);
}

static int float_equal_or_update(
    mp_float_t *cached,
    mp_float_t new) {
    // uses memcmp to avoid error about equality float comparison
    if (memcmp(cached, &new, sizeof(mp_float_t))) {
        *cached = new;
        return false;
    }
    return true;
}

void common_hal_synthio_biquad_tick(mp_obj_t self_in) {
    synthio_biquad_t *self = MP_OBJ_TO_PTR(self_in);

    mp_float_t frequency = synthio_block_slot_get(&self->f0);
    mp_float_t W0 = frequency * synthio_global_W_scale;
    mp_float_t Q = synthio_block_slot_get(&self->Q);
    mp_float_t A =
        (self->mode >= SYNTHIO_PEAKING_EQ) ? synthio_block_slot_get(&self->A) : 0;

    // n.b., assumes that the `mode` field is read-only
    // n.b., use of `&` is deliberate, avoids short-circuiting behavior
    if (float_equal_or_update(&self->cached_W0, W0)
        & float_equal_or_update(&self->cached_Q, Q)
        & float_equal_or_update(&self->cached_A, A)) {
        return;
    }

    // CircuitPython's own Q15 arithmetic, from the shared kernel so all three
    // targets run one copy (audiodsp#77). `synthio.Biquad` is a node
    // CircuitPython also has, so it renders CircuitPython's bytes; the widened
    // kernel beside it in audiodsp_biquad.c is used only by `audiobiquad`,
    // which is ours.
    audiodsp_biquad_cp_configure(&self->coefficients, self->mode, W0, Q, A);
}

void synthio_biquad_filter_reset(biquad_filter_state *st) {
    // All four state words, not upstream 10.3.0's `x` only. Fixed on
    // CircuitPython main by 8a3deace5c; see audiodsp_biquad.c.
    audiodsp_biquad_reset(st);
}

void synthio_biquad_filter_samples(mp_obj_t self_in, biquad_filter_state *st, int32_t *buffer, size_t n_samples) {
    synthio_biquad_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_biquad_cp_process(&self->coefficients, st, buffer, n_samples);
}

int32_t synthio_biquad_filter_sample(mp_obj_t self_in, biquad_filter_state *st, int32_t input) {
    synthio_biquad_t *self = MP_OBJ_TO_PTR(self_in);
    return audiodsp_biquad_cp_sample(&self->coefficients, st, input);
}

// --- from shared-bindings/synthio/Biquad.c --------------------------------

MAKE_ENUM_VALUE(synthio_filter_mode_type, mode, LOW_PASS, SYNTHIO_LOW_PASS);
MAKE_ENUM_VALUE(synthio_filter_mode_type, mode, HIGH_PASS, SYNTHIO_HIGH_PASS);
MAKE_ENUM_VALUE(synthio_filter_mode_type, mode, BAND_PASS, SYNTHIO_BAND_PASS);
MAKE_ENUM_VALUE(synthio_filter_mode_type, mode, NOTCH, SYNTHIO_NOTCH);
MAKE_ENUM_VALUE(synthio_filter_mode_type, mode, LOW_SHELF, SYNTHIO_LOW_SHELF);
MAKE_ENUM_VALUE(synthio_filter_mode_type, mode, HIGH_SHELF, SYNTHIO_HIGH_SHELF);
MAKE_ENUM_VALUE(synthio_filter_mode_type, mode, PEAKING_EQ, SYNTHIO_PEAKING_EQ);

MAKE_ENUM_MAP(synthio_filter_mode) {
    MAKE_ENUM_MAP_ENTRY(mode, LOW_PASS),
    MAKE_ENUM_MAP_ENTRY(mode, HIGH_PASS),
    MAKE_ENUM_MAP_ENTRY(mode, BAND_PASS),
    MAKE_ENUM_MAP_ENTRY(mode, NOTCH),
    MAKE_ENUM_MAP_ENTRY(mode, LOW_SHELF),
    MAKE_ENUM_MAP_ENTRY(mode, HIGH_SHELF),
    MAKE_ENUM_MAP_ENTRY(mode, PEAKING_EQ),
};

static MP_DEFINE_CONST_DICT(synthio_filter_mode_locals_dict, synthio_filter_mode_locals_table);

MAKE_PRINTER(synthio, synthio_filter_mode);

MAKE_ENUM_TYPE(synthio, FilterMode, synthio_filter_mode);

static synthio_filter_mode validate_synthio_filter_mode(mp_obj_t obj, qstr arg_name) {
    return cp_enum_value(&synthio_filter_mode_type, obj, arg_name);
}

static const mp_arg_t biquad_properties[] = {
    { MP_QSTR_mode, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL } },
    { MP_QSTR_frequency, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL } },
    { MP_QSTR_Q, MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL } },
    { MP_QSTR_A, MP_ARG_OBJ, {.u_obj = MP_ROM_NONE } },
};

static mp_obj_t synthio_biquad_make_new(const mp_obj_type_t *type_in, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_mode, ARG_frequency, ARG_Q };

    mp_arg_val_t args[MP_ARRAY_SIZE(biquad_properties)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(biquad_properties), biquad_properties, args);

    if (args[ARG_Q].u_obj == MP_OBJ_NULL) {
        args[ARG_Q].u_obj = mp_obj_new_float(MICROPY_FLOAT_CONST(0.7071067811865475));
    }

    synthio_filter_mode mode = validate_synthio_filter_mode(args[ARG_mode].u_obj, MP_QSTR_mode);
    mp_obj_t result = common_hal_synthio_biquad_new(mode);
    properties_construct_helper(result, biquad_properties + 1, args + 1, MP_ARRAY_SIZE(biquad_properties) - 1);
    return result;
}

static mp_obj_t synthio_biquad_get_mode(mp_obj_t self_in) {
    synthio_biquad_t *self = MP_OBJ_TO_PTR(self_in);
    return cp_enum_find(&synthio_filter_mode_type, common_hal_synthio_biquad_get_mode(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_biquad_get_mode_obj, synthio_biquad_get_mode);

MP_PROPERTY_GETTER(synthio_biquad_mode_obj,
    (mp_obj_t)&synthio_biquad_get_mode_obj);

static mp_obj_t synthio_biquad_get_frequency(mp_obj_t self_in) {
    synthio_biquad_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_biquad_get_frequency(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_biquad_get_frequency_obj, synthio_biquad_get_frequency);

static mp_obj_t synthio_biquad_set_frequency(mp_obj_t self_in, mp_obj_t arg) {
    synthio_biquad_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_biquad_set_frequency(self, arg);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_biquad_set_frequency_obj, synthio_biquad_set_frequency);
MP_PROPERTY_GETSET(synthio_biquad_frequency_obj,
    (mp_obj_t)&synthio_biquad_get_frequency_obj,
    (mp_obj_t)&synthio_biquad_set_frequency_obj);

static mp_obj_t synthio_biquad_get_Q(mp_obj_t self_in) {
    synthio_biquad_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_biquad_get_Q(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_biquad_get_Q_obj, synthio_biquad_get_Q);

static mp_obj_t synthio_biquad_set_Q(mp_obj_t self_in, mp_obj_t arg) {
    synthio_biquad_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_biquad_set_Q(self, arg);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_biquad_set_Q_obj, synthio_biquad_set_Q);
MP_PROPERTY_GETSET(synthio_biquad_Q_obj,
    (mp_obj_t)&synthio_biquad_get_Q_obj,
    (mp_obj_t)&synthio_biquad_set_Q_obj);

static mp_obj_t synthio_biquad_get_A(mp_obj_t self_in) {
    synthio_biquad_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_biquad_get_A(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_biquad_get_A_obj, synthio_biquad_get_A);

static mp_obj_t synthio_biquad_set_A(mp_obj_t self_in, mp_obj_t arg) {
    synthio_biquad_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_biquad_set_A(self, arg);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_biquad_set_A_obj, synthio_biquad_set_A);
MP_PROPERTY_GETSET(synthio_biquad_A_obj,
    (mp_obj_t)&synthio_biquad_get_A_obj,
    (mp_obj_t)&synthio_biquad_set_A_obj);

static const mp_rom_map_elem_t synthio_biquad_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_mode), MP_ROM_PTR(&synthio_biquad_mode_obj) },
    { MP_ROM_QSTR(MP_QSTR_frequency), MP_ROM_PTR(&synthio_biquad_frequency_obj) },
    { MP_ROM_QSTR(MP_QSTR_Q), MP_ROM_PTR(&synthio_biquad_Q_obj) },
    { MP_ROM_QSTR(MP_QSTR_A), MP_ROM_PTR(&synthio_biquad_A_obj) },
};
static MP_DEFINE_CONST_DICT(synthio_biquad_locals_dict, synthio_biquad_locals_dict_table);

static void biquad_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    properties_print_helper(print, self_in, biquad_properties, MP_ARRAY_SIZE(biquad_properties));
}

MP_DEFINE_CONST_OBJ_TYPE(
    synthio_biquad_type_obj,
    MP_QSTR_Biquad,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, synthio_biquad_make_new,
    attr, cp_compat_attr,
    locals_dict, &synthio_biquad_locals_dict,
    print, biquad_print
    );
