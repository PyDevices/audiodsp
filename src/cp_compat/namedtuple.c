// Compat shim for audiodsp. See namedtuple.h.
//
// namedtuple_print/namedtuple_attr/namedtuple_make_new below are verbatim
// copies of the `static` functions of the same name in mainline
// MicroPython's py/objnamedtuple.c (MIT, Damien P. George / Paul
// Sokolovsky), just made non-static so a usermod can reference them from a
// const type struct (mainline itself only wires them into a
// dynamically-built type via mp_obj_new_namedtuple_type -- see that
// function in the mainline file for the runtime-construction equivalent of
// what NAMEDTUPLE_TYPE_BASE_AND_SLOTS_MAKE_NEW does at compile time).
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "cp_compat/namedtuple.h"
#include "py/objtuple.h"
#include "py/runtime.h"

void namedtuple_print(const mp_print_t *print, mp_obj_t o_in, mp_print_kind_t kind) {
    (void)kind;
    mp_obj_namedtuple_t *o = MP_OBJ_TO_PTR(o_in);
    mp_printf(print, "%q", (qstr)o->tuple.base.type->name);
    const qstr *fields = ((mp_obj_namedtuple_type_t *)o->tuple.base.type)->fields;
    mp_obj_attrtuple_print_helper(print, fields, &o->tuple);
}

void namedtuple_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] == MP_OBJ_NULL) {
        // load attribute
        mp_obj_namedtuple_t *self = MP_OBJ_TO_PTR(self_in);
        size_t id = mp_obj_namedtuple_find_field((mp_obj_namedtuple_type_t *)self->tuple.base.type, attr);
        if (id == (size_t)-1) {
            return;
        }
        dest[0] = self->tuple.items[id];
    } else {
        // delete/store attribute
        mp_raise_msg(&mp_type_AttributeError, MP_ERROR_TEXT("can't set attribute"));
    }
}

mp_obj_t namedtuple_make_new(const mp_obj_type_t *type_in, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    const mp_obj_namedtuple_type_t *type = (const mp_obj_namedtuple_type_t *)type_in;
    size_t num_fields = type->n_fields;
    if (n_args + n_kw != num_fields) {
        mp_raise_msg_varg(&mp_type_TypeError,
            MP_ERROR_TEXT("function takes %d positional arguments but %d were given"),
            num_fields, n_args + n_kw);
    }

    // Create a namedtuple with explicit malloc. Calling mp_obj_new_tuple
    // with num_fields=0 returns a read-only object.
    mp_obj_tuple_t *tuple = mp_obj_malloc_var(mp_obj_tuple_t, items, mp_obj_t, num_fields, type_in);
    tuple->len = num_fields;

    // Copy the positional args into the first slots of the namedtuple
    memcpy(&tuple->items[0], args, sizeof(mp_obj_t) * n_args);

    // Fill in the remaining slots with the keyword args
    memset(&tuple->items[n_args], 0, sizeof(mp_obj_t) * n_kw);
    for (size_t i = n_args; i < n_args + 2 * n_kw; i += 2) {
        qstr kw = mp_obj_str_get_qstr(args[i]);
        size_t id = mp_obj_namedtuple_find_field(type, kw);
        if (id == (size_t)-1) {
            mp_raise_msg_varg(&mp_type_TypeError, MP_ERROR_TEXT("unexpected keyword argument '%q'"), kw);
        }
        if (tuple->items[id] != MP_OBJ_NULL) {
            mp_raise_msg_varg(&mp_type_TypeError,
                MP_ERROR_TEXT("function got multiple values for argument '%q'"), kw);
        }
        tuple->items[id] = args[i + 1];
    }

    return MP_OBJ_FROM_PTR(tuple);
}
