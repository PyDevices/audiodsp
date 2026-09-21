// Compat shim for audiodsp. See proto.h.
//
// Ported from CircuitPython's py/proto.c (upstream repo:
// https://github.com/adafruit/circuitpython, MIT), with the error path
// using `mp_obj_get_type(obj)->name` in place of CP's mp_obj_get_type_qstr
// (mainline doesn't have that helper; it's a one-line inline).
//
// SPDX-FileCopyrightText: Copyright (c) 2019 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include "cp_compat/argcheck.h"
#include "cp_compat/proto.h"
#include "py/runtime.h"

const void *mp_proto_get(uint16_t name, mp_const_obj_t obj) {
    const mp_obj_type_t *type = mp_obj_get_type(obj);
    const void *protocol = MP_OBJ_TYPE_GET_SLOT_OR_NULL(type, protocol);
    if (!protocol) {
        return NULL;
    }
    uint16_t proto_name = *(const uint16_t *)protocol;
    if (proto_name == name) {
        return protocol;
    }
    return NULL;
}

const void *mp_proto_get_or_throw(uint16_t name, mp_const_obj_t obj) {
    const void *proto = mp_proto_get(name, obj);
    if (proto) {
        return proto;
    }
    mp_raise_TypeError_varg(MP_ERROR_TEXT("'%q' object does not support '%q'"),
        mp_obj_get_type(obj)->name, name);
}
