// Ported from CircuitPython's shared-bindings/synthio/Math.c and
// shared-module/synthio/Math.c (upstream repo:
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
#include "cp_compat/enum.h"
#include "cp_compat/objproperty.h"
#include "cp_compat/util.h"
#include "synthio/Math.h"
#include "synthio/__init__.h"

#include "py/obj.h"
#include "py/runtime.h"

// --- from shared-module/synthio/Math.c ------------------------------------

mp_obj_t common_hal_synthio_math_get_input_obj(synthio_math_obj_t *self, size_t i) {
    return self->inputs[i].obj;
}

void common_hal_synthio_math_set_input_obj(synthio_math_obj_t *self, size_t i, mp_obj_t arg, qstr argname) {
    assert(i < MP_ARRAY_SIZE(self->inputs));
    synthio_block_assign_slot(arg, &self->inputs[i], argname);
}

synthio_math_operation_t common_hal_synthio_math_get_operation(synthio_math_obj_t *self) {
    return self->operation;
}

void common_hal_synthio_math_set_operation(synthio_math_obj_t *self, synthio_math_operation_t arg) {
    self->operation = arg;
}

#define ZERO (MICROPY_FLOAT_CONST(0.))

mp_float_t common_hal_synthio_math_get_value(synthio_math_obj_t *self) {
    return self->base.value;
}

mp_float_t common_hal_synthio_math_tick(mp_obj_t self_in) {
    synthio_math_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_float_t a = synthio_block_slot_get(&self->inputs[0]);

    if (self->operation == OP_ABS) {
        return MICROPY_FLOAT_C_FUN(fabs)(a);
    }

    mp_float_t b = synthio_block_slot_get(&self->inputs[1]);
    mp_float_t c = synthio_block_slot_get(&self->inputs[2]);

    switch (self->operation) {
        case OP_SUM:
            return a + b + c;
        case OP_ADD_SUB:
            return a + b - c;
        case OP_PRODUCT:
            return a * b * c;
        case OP_MUL_DIV:
            if (c == 0) {
                return 0;
            }
            return a * b / c;
        case OP_SCALE_OFFSET:
            return a * b + c;
        case OP_OFFSET_SCALE:
            return (a + b) * c;
        case OP_CONSTRAINED_LERP:
            c = MIN(1, MAX(0, c));
            MP_FALLTHROUGH;
        case OP_LERP:
            return a * (1 - c) + b * c;
        case OP_DIV_ADD:
            if (b == 0) {
                return ZERO;
            }
            return a / b + c;
        case OP_ADD_DIV:
            if (c == 0) {
                return ZERO;
            }
            return (a + b) / c;
        case OP_MID:
            if (a < b) {
                if (b < c) {
                    return b;
                }
                if (a < c) {
                    return c;
                }
                return a;
            }
            if (a < c) {
                return a;
            }
            if (c < b) {
                return b;
            }
            return c;
        case OP_MIN:
            return MIN(a, MIN(b, c));
        case OP_MAX:
            return MAX(a, MAX(b, c));
        case OP_ABS:
            break;
    }
    return ZERO;
}

// --- from shared-bindings/synthio/Math.c ----------------------------------

static const mp_arg_t math_properties[4];
static mp_obj_t synthio_math_make_new_common(mp_arg_val_t args[MP_ARRAY_SIZE(math_properties)]);

MAKE_ENUM_VALUE(synthio_math_operation_type, math_op, SUM, OP_SUM);
MAKE_ENUM_VALUE(synthio_math_operation_type, math_op, ADD_SUB, OP_ADD_SUB);
MAKE_ENUM_VALUE(synthio_math_operation_type, math_op, PRODUCT, OP_PRODUCT);
MAKE_ENUM_VALUE(synthio_math_operation_type, math_op, MUL_DIV, OP_MUL_DIV);
MAKE_ENUM_VALUE(synthio_math_operation_type, math_op, SCALE_OFFSET, OP_SCALE_OFFSET);
MAKE_ENUM_VALUE(synthio_math_operation_type, math_op, OFFSET_SCALE, OP_OFFSET_SCALE);
MAKE_ENUM_VALUE(synthio_math_operation_type, math_op, LERP, OP_LERP);
MAKE_ENUM_VALUE(synthio_math_operation_type, math_op, CONSTRAINED_LERP, OP_CONSTRAINED_LERP);
MAKE_ENUM_VALUE(synthio_math_operation_type, math_op, DIV_ADD, OP_DIV_ADD);
MAKE_ENUM_VALUE(synthio_math_operation_type, math_op, ADD_DIV, OP_ADD_DIV);
MAKE_ENUM_VALUE(synthio_math_operation_type, math_op, MID, OP_MID);
MAKE_ENUM_VALUE(synthio_math_operation_type, math_op, MAX, OP_MAX);
MAKE_ENUM_VALUE(synthio_math_operation_type, math_op, MIN, OP_MIN);
MAKE_ENUM_VALUE(synthio_math_operation_type, math_op, ABS, OP_ABS);

MAKE_ENUM_MAP(synthio_math_operation) {
    MAKE_ENUM_MAP_ENTRY(math_op, SUM),
    MAKE_ENUM_MAP_ENTRY(math_op, ADD_SUB),
    MAKE_ENUM_MAP_ENTRY(math_op, PRODUCT),
    MAKE_ENUM_MAP_ENTRY(math_op, MUL_DIV),
    MAKE_ENUM_MAP_ENTRY(math_op, SCALE_OFFSET),
    MAKE_ENUM_MAP_ENTRY(math_op, OFFSET_SCALE),
    MAKE_ENUM_MAP_ENTRY(math_op, LERP),
    MAKE_ENUM_MAP_ENTRY(math_op, CONSTRAINED_LERP),
    MAKE_ENUM_MAP_ENTRY(math_op, DIV_ADD),
    MAKE_ENUM_MAP_ENTRY(math_op, ADD_DIV),
    MAKE_ENUM_MAP_ENTRY(math_op, MID),
    MAKE_ENUM_MAP_ENTRY(math_op, MAX),
    MAKE_ENUM_MAP_ENTRY(math_op, MIN),
    MAKE_ENUM_MAP_ENTRY(math_op, ABS),
};

static mp_obj_t mathop_call(mp_obj_t fun, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_val_t args[4];
    args[0].u_obj = fun;
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(math_properties) - 1, math_properties + 1, &args[1]);
    return synthio_math_make_new_common(args);
}

static MP_DEFINE_CONST_DICT(synthio_math_operation_locals_dict, synthio_math_operation_locals_table);
MAKE_PRINTER(synthio, synthio_math_operation);
MAKE_ENUM_TYPE(synthio, MathOperation, synthio_math_operation,
    call, mathop_call
    );

static const mp_arg_t math_properties[] = {
    { MP_QSTR_operation, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = NULL } },
    { MP_QSTR_a, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = NULL } },
    { MP_QSTR_b, MP_ARG_OBJ, {.u_obj = MP_ROM_INT(0) } },
    { MP_QSTR_c, MP_ARG_OBJ, {.u_obj = MP_ROM_INT(1) } },
};

static mp_obj_t synthio_math_make_new(const mp_obj_type_t *type_in, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_val_t args[MP_ARRAY_SIZE(math_properties)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(math_properties), math_properties, args);

    return synthio_math_make_new_common(args);
}

static mp_obj_t synthio_math_make_new_common(mp_arg_val_t args[MP_ARRAY_SIZE(math_properties)]) {
    synthio_math_obj_t *self = mp_obj_malloc(synthio_math_obj_t, &synthio_math_type);

    self->base.last_tick = synthio_global_tick;

    mp_obj_t self_obj = MP_OBJ_FROM_PTR(self);
    properties_construct_helper(self_obj, math_properties, args, MP_ARRAY_SIZE(math_properties));

    return self_obj;
};

static mp_obj_t synthio_math_get_a(mp_obj_t self_in) {
    synthio_math_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_math_get_input_obj(self, 0);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_math_get_a_obj, synthio_math_get_a);

static mp_obj_t synthio_math_set_a(mp_obj_t self_in, mp_obj_t arg) {
    synthio_math_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_math_set_input_obj(self, 0, arg, MP_QSTR_a);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_math_set_a_obj, synthio_math_set_a);
MP_PROPERTY_GETSET(synthio_math_a_obj,
    (mp_obj_t)&synthio_math_get_a_obj,
    (mp_obj_t)&synthio_math_set_a_obj);

static mp_obj_t synthio_math_get_b(mp_obj_t self_in) {
    synthio_math_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_math_get_input_obj(self, 1);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_math_get_b_obj, synthio_math_get_b);

static mp_obj_t synthio_math_set_b(mp_obj_t self_in, mp_obj_t arg) {
    synthio_math_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_math_set_input_obj(self, 1, arg, MP_QSTR_b);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_math_set_b_obj, synthio_math_set_b);
MP_PROPERTY_GETSET(synthio_math_b_obj,
    (mp_obj_t)&synthio_math_get_b_obj,
    (mp_obj_t)&synthio_math_set_b_obj);

static mp_obj_t synthio_math_get_c(mp_obj_t self_in) {
    synthio_math_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_synthio_math_get_input_obj(self, 2);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_math_get_c_obj, synthio_math_get_c);

static mp_obj_t synthio_math_set_c(mp_obj_t self_in, mp_obj_t arg) {
    synthio_math_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_math_set_input_obj(self, 2, arg, MP_QSTR_c);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_math_set_c_obj, synthio_math_set_c);
MP_PROPERTY_GETSET(synthio_math_c_obj,
    (mp_obj_t)&synthio_math_get_c_obj,
    (mp_obj_t)&synthio_math_set_c_obj);

static mp_obj_t synthio_math_get_operation(mp_obj_t self_in) {
    synthio_math_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return cp_enum_find(&synthio_math_operation_type, common_hal_synthio_math_get_operation(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_math_get_operation_obj, synthio_math_get_operation);

static mp_obj_t synthio_math_set_operation(mp_obj_t self_in, mp_obj_t arg) {
    synthio_math_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_math_set_operation(self, cp_enum_value(&synthio_math_operation_type, arg, MP_QSTR_operation));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_math_set_operation_obj, synthio_math_set_operation);
MP_PROPERTY_GETSET(synthio_math_operation_obj,
    (mp_obj_t)&synthio_math_get_operation_obj,
    (mp_obj_t)&synthio_math_set_operation_obj);

static mp_obj_t synthio_math_get_value(mp_obj_t self_in) {
    synthio_math_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_float(common_hal_synthio_math_get_value(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_math_get_value_obj, synthio_math_get_value);

MP_PROPERTY_GETTER(synthio_math_value_obj,
    (mp_obj_t)&synthio_math_get_value_obj);

static void math_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    properties_print_helper(print, self_in, math_properties, MP_ARRAY_SIZE(math_properties));
}

static const mp_rom_map_elem_t synthio_math_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_a), MP_ROM_PTR(&synthio_math_a_obj) },
    { MP_ROM_QSTR(MP_QSTR_b), MP_ROM_PTR(&synthio_math_b_obj) },
    { MP_ROM_QSTR(MP_QSTR_c), MP_ROM_PTR(&synthio_math_c_obj) },
    { MP_ROM_QSTR(MP_QSTR_operation), MP_ROM_PTR(&synthio_math_operation_obj) },
    { MP_ROM_QSTR(MP_QSTR_value), MP_ROM_PTR(&synthio_math_value_obj) },
};
static MP_DEFINE_CONST_DICT(synthio_math_locals_dict, synthio_math_locals_dict_table);

static const synthio_block_proto_t math_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_synthio_block)
    .tick = common_hal_synthio_math_tick,
};

MP_DEFINE_CONST_OBJ_TYPE(
    synthio_math_type,
    MP_QSTR_Math,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, synthio_math_make_new,
    attr, cp_compat_attr,
    locals_dict, &synthio_math_locals_dict,
    print, math_print,
    protocol, &math_proto
    );
