// Compat shim for audiodsp: lets a usermod define a *compile-time
// const* namedtuple-shaped native type (used for synthio.Envelope), the way
// CircuitPython's py/objnamedtuple.h extension does.
//
// Mainline's py/objnamedtuple.c (MIT, Damien P. George / Paul Sokolovsky)
// implements everything needed -- make_new, print, and attr handlers for a
// namedtuple type -- but keeps them `static`, since mainline only exposes
// namedtuples through the Python-level `collections.namedtuple()` factory
// (`mp_obj_new_namedtuple_type`, which builds the type object at runtime,
// not as ROM-able const data). CircuitPython's own py/objnamedtuple.h adds
// public declarations for those three functions plus this
// NAMEDTUPLE_TYPE_BASE_AND_SLOTS_MAKE_NEW macro so a native binding can
// build a const type struct directly (that's the "CIRCUITPY-CHANGE" diff
// against mainline's file). We can't un-static mainline's copies (no edits
// to micropython/), so this reimplements the same three functions from
// usermod code -- mp_obj_namedtuple_find_field, mp_obj_tuple_unary_op/
// binary_op/subscr/getiter, and mp_obj_attrtuple_print_helper are all
// already public in mainline and reused as-is; see namedtuple.c.
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/objnamedtuple.h"

void namedtuple_print(const mp_print_t *print, mp_obj_t o_in, mp_print_kind_t kind);
void namedtuple_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest);
mp_obj_t namedtuple_make_new(const mp_obj_type_t *type_in, size_t n_args, size_t n_kw, const mp_obj_t *args);

#define NAMEDTUPLE_TYPE_BASE_AND_SLOTS_MAKE_NEW(type_name, make_new_fun) \
    .base = { \
        .base = { .type = &mp_type_type }, \
        .flags = MP_TYPE_FLAG_EQ_CHECKS_OTHER_TYPE, \
        .name = type_name, \
        .slot_index_make_new = 1, \
        .slot_index_print = 2, \
        .slot_index_unary_op = 3, \
        .slot_index_binary_op = 4, \
        .slot_index_attr = 5, \
        .slot_index_subscr = 6, \
        .slot_index_iter = 7, \
        .slot_index_parent = 8, \
    }, \
    .slots = { \
        make_new_fun, \
        namedtuple_print, \
        mp_obj_tuple_unary_op, \
        mp_obj_tuple_binary_op, \
        namedtuple_attr, \
        mp_obj_tuple_subscr, \
        mp_obj_tuple_getiter, \
        (void *)&mp_type_tuple, \
    }

#define NAMEDTUPLE_TYPE_BASE_AND_SLOTS(type_name) \
    NAMEDTUPLE_TYPE_BASE_AND_SLOTS_MAKE_NEW(type_name, namedtuple_make_new)
