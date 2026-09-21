// Compat shim for audiodsp: CircuitPython's declarative property
// macros (`MP_PROPERTY_GETTER`, `MP_PROPERTY_GETSET`), adapted from
// CircuitPython's py/objproperty.h (upstream repo:
// https://github.com/adafruit/circuitpython, MIT) to mainline
// MicroPython's real property object.
//
// Mainline's py/objproperty.c keeps its `mp_obj_property_t` layout private
// (only `mp_type_property` and `mp_obj_property_get()` are public), but the
// layout is stable and documented by that accessor: a plain
// `mp_obj_base_t` followed by `mp_obj_t proxy[3]` (getter, setter,
// deleter). CircuitPython's flash-tagged variant of these macros isn't
// used here (that's CP's own linker-section trick for its read-only
// property tables; ordinary const data works fine for a usermod).
//
// Declaring a property this way is necessary but not sufficient: mainline
// has no built-in code path that *invokes* a property found in a native
// type's locals_dict (CircuitPython patches that into its own
// py/runtime.c). Every ported type with a property must also wire
// `cp_compat_attr` (objproperty.c) as its own `attr` type slot -- see that
// file for why, and any ported *.c using MP_PROPERTY_GETTER/GETSET for a
// worked example of the `MP_DEFINE_CONST_OBJ_TYPE(..., attr, cp_compat_attr,
// ...)` wiring.
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

typedef struct {
    mp_obj_base_t base;
    mp_obj_t proxy[3]; // getter, setter, deleter -- must match py/objproperty.c
} cp_compat_property_t;

#define MP_PROPERTY_GETTER(P, G) \
    const cp_compat_property_t P = { .base = { &mp_type_property }, .proxy = { G, MP_ROM_NONE, MP_ROM_NONE } }

#define MP_PROPERTY_GETSET(P, G, S) \
    const cp_compat_property_t P = { .base = { &mp_type_property }, .proxy = { G, S, MP_ROM_NONE } }

// See objproperty.c. Wire as the `attr` slot of any MP_DEFINE_CONST_OBJ_TYPE
// that declares MP_PROPERTY_GETTER/GETSET members in its locals_dict.
void cp_compat_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest);
