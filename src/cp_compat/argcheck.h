// Compat shim for audiodsp: CircuitPython-only argument-validation
// helpers (`mp_arg_validate_*`) and per-exception `_varg` raisers, ported
// from CircuitPython's `py/argcheck.h` / `py/runtime.h` (upstream repo:
// https://github.com/adafruit/circuitpython, MIT). Mainline MicroPython
// only has the generic `mp_raise_msg_varg`; this adds the convenience
// wrappers CP's shared-bindings/shared-module audio sources call directly,
// so those files can be ported without modification to their call sites.
//
// SPDX-FileCopyrightText: Copyright (c) 2013, 2014 Damien P. George
// SPDX-FileCopyrightText: Copyright (c) 2014-2018 Paul Sokolovsky
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"
#include "py/runtime.h"

// Per-exception varg raisers CP adds over mainline's generic mp_raise_msg_varg.
// Only the ones actually called by the ported audio/synthio sources are here;
// add more from CircuitPython's py/runtime.c as later tiers need them.
MP_NORETURN void mp_raise_ValueError_varg(mp_rom_error_text_t fmt, ...);
MP_NORETURN void mp_raise_TypeError_varg(mp_rom_error_text_t fmt, ...);
MP_NORETURN void mp_raise_IndexError_varg(mp_rom_error_text_t fmt, ...);

mp_int_t mp_arg_validate_int(mp_int_t i, mp_int_t required_i, qstr arg_name);
mp_int_t mp_arg_validate_int_min(mp_int_t i, mp_int_t min, qstr arg_name);
mp_int_t mp_arg_validate_int_max(mp_int_t i, mp_int_t max, qstr arg_name);
mp_int_t mp_arg_validate_int_range(mp_int_t i, mp_int_t min, mp_int_t max, qstr arg_name);

mp_float_t mp_arg_validate_type_float(mp_obj_t obj, qstr arg_name);
mp_float_t mp_arg_validate_obj_float_range(mp_obj_t float_in, mp_int_t min, mp_int_t max, qstr arg_name);
mp_float_t mp_arg_validate_float_range(mp_float_t f, mp_int_t min, mp_int_t max, qstr arg_name);
mp_float_t mp_arg_validate_obj_float_non_negative(mp_obj_t float_in, mp_float_t default_for_null, qstr arg_name);

mp_uint_t mp_arg_validate_length_range(mp_uint_t length, mp_uint_t min, mp_uint_t max, qstr arg_name);
mp_uint_t mp_arg_validate_length_min(mp_uint_t length, mp_uint_t min, qstr arg_name);
mp_uint_t mp_arg_validate_length_max(mp_uint_t length, mp_uint_t max, qstr arg_name);
mp_uint_t mp_arg_validate_length(mp_uint_t length, mp_uint_t required_length, qstr arg_name);

// int instead of uint because an index can be negative in some cases.
mp_int_t mp_arg_validate_index_range(mp_int_t index, mp_int_t min, mp_int_t max, qstr arg_name);

mp_obj_t mp_arg_validate_type(mp_obj_t obj, const mp_obj_type_t *type, qstr arg_name);
mp_obj_t mp_arg_validate_type_in(mp_obj_t obj, const mp_obj_type_t *type, qstr arg_name);
mp_obj_t mp_arg_validate_type_or_none(mp_obj_t obj, const mp_obj_type_t *type, qstr arg_name);
mp_obj_t mp_arg_validate_type_string(mp_obj_t obj, qstr arg_name);
mp_int_t mp_arg_validate_type_int(mp_obj_t obj, qstr arg_name);

MP_NORETURN void mp_arg_error_invalid(qstr arg_name);
