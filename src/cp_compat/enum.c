// Compat shim for audiodsp. See enum.h.
//
// Ported verbatim from CircuitPython's py/enum.c (upstream repo:
// https://github.com/adafruit/circuitpython, MIT). mp_arg_validate_type
// comes from cp_compat/argcheck.h.
//
// SPDX-FileCopyrightText: Copyright (c) 2020 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include "cp_compat/argcheck.h"
#include "cp_compat/enum.h"
#include "py/runtime.h"

mp_obj_t cp_enum_find(const mp_obj_type_t *type, int value) {
    const mp_obj_dict_t *dict = MP_OBJ_TYPE_GET_SLOT(type, locals_dict);
    for (size_t i = 0; i < dict->map.used; i++) {
        const cp_enum_obj_t *v = MP_OBJ_TO_PTR(dict->map.table[i].value);
        if (v->value == value) {
            return (mp_obj_t)v;
        }
    }
    return mp_const_none;
}

int cp_enum_value(const mp_obj_type_t *type, mp_obj_t obj, qstr arg_name) {
    (void)mp_arg_validate_type(obj, type, arg_name);
    return ((cp_enum_obj_t *)MP_OBJ_TO_PTR(obj))->value;
}

void cp_enum_obj_print_helper(uint16_t module, const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    cp_enum_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "%q.%q.%q", module, self->base.type->name, self->name);
}
