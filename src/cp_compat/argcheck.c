// Compat shim for audiodsp. See argcheck.h.
//
// Ported from CircuitPython's py/argcheck.c and py/runtime.c (upstream
// repo: https://github.com/adafruit/circuitpython, MIT), with the varg
// raisers rewritten against mainline's public
// mp_obj_new_exception_msg_vlist() + nlr_raise() instead of CP's private
// mp_raise_msg_vlist() (mainline doesn't expose that name).
//
// SPDX-FileCopyrightText: Copyright (c) 2013, 2014 Damien P. George
// SPDX-FileCopyrightText: Copyright (c) 2014-2018 Paul Sokolovsky
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include <stdarg.h>

#include "cp_compat/argcheck.h"
#include "py/nlr.h"

static MP_NORETURN void raise_msg_varg(const mp_obj_type_t *exc_type, mp_rom_error_text_t fmt, va_list args) {
    mp_obj_t exc = mp_obj_new_exception_msg_vlist(exc_type, fmt, args);
    nlr_raise(exc);
}

MP_NORETURN void mp_raise_ValueError_varg(mp_rom_error_text_t fmt, ...) {
    va_list argptr;
    va_start(argptr, fmt);
    raise_msg_varg(&mp_type_ValueError, fmt, argptr);
    va_end(argptr);
}

MP_NORETURN void mp_raise_TypeError_varg(mp_rom_error_text_t fmt, ...) {
    va_list argptr;
    va_start(argptr, fmt);
    raise_msg_varg(&mp_type_TypeError, fmt, argptr);
    va_end(argptr);
}

MP_NORETURN void mp_raise_IndexError_varg(mp_rom_error_text_t fmt, ...) {
    va_list argptr;
    va_start(argptr, fmt);
    raise_msg_varg(&mp_type_IndexError, fmt, argptr);
    va_end(argptr);
}

mp_int_t mp_arg_validate_int(mp_int_t i, mp_int_t required_i, qstr arg_name) {
    if (i != required_i) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be %d"), arg_name, required_i);
    }
    return i;
}

mp_int_t mp_arg_validate_int_min(mp_int_t i, mp_int_t min, qstr arg_name) {
    if (i < min) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be >= %d"), arg_name, min);
    }
    return i;
}

mp_int_t mp_arg_validate_int_max(mp_int_t i, mp_int_t max, qstr arg_name) {
    if (i > max) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be <= %d"), arg_name, max);
    }
    return i;
}

mp_int_t mp_arg_validate_int_range(mp_int_t i, mp_int_t min, mp_int_t max, qstr arg_name) {
    if (i < min || i > max) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be %d-%d"), arg_name, min, max);
    }
    return i;
}

mp_float_t mp_arg_validate_type_float(mp_obj_t obj, qstr arg_name) {
    mp_float_t a_float;
    if (!mp_obj_get_float_maybe(obj, &a_float)) {
        mp_raise_TypeError_varg(MP_ERROR_TEXT("%q must be of type %q, not %q"), arg_name, MP_QSTR_float, mp_obj_get_type(obj)->name);
    }
    return a_float;
}

mp_float_t mp_arg_validate_obj_float_range(mp_obj_t float_in, mp_int_t min, mp_int_t max, qstr arg_name) {
    const mp_float_t f = mp_arg_validate_type_float(float_in, arg_name);
    return mp_arg_validate_float_range(f, min, max, arg_name);
}

mp_float_t mp_arg_validate_float_range(mp_float_t f, mp_int_t min, mp_int_t max, qstr arg_name) {
    if (f < (mp_float_t)min || f > (mp_float_t)max) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be %d-%d"), arg_name, min, max);
    }
    return f;
}

mp_float_t mp_arg_validate_obj_float_non_negative(mp_obj_t float_in, mp_float_t default_for_null, qstr arg_name) {
    const mp_float_t f = (float_in == MP_OBJ_NULL)
        ? default_for_null
        : mp_arg_validate_type_float(float_in, arg_name);
    if (f < (mp_float_t)0.0) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be >= %d"), arg_name, 0);
    }
    return f;
}

mp_uint_t mp_arg_validate_length_range(mp_uint_t length, mp_uint_t min, mp_uint_t max, qstr arg_name) {
    if (length < min || length > max) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q length must be %d-%d"), arg_name, min, max);
    }
    return length;
}

mp_uint_t mp_arg_validate_length_min(mp_uint_t length, mp_uint_t min, qstr arg_name) {
    if (length < min) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q length must be >= %d"), arg_name, min);
    }
    return length;
}

mp_uint_t mp_arg_validate_length_max(mp_uint_t length, mp_uint_t max, qstr arg_name) {
    if (length > max) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q length must be <= %d"), arg_name, max);
    }
    return length;
}

mp_uint_t mp_arg_validate_length(mp_uint_t length, mp_uint_t required_length, qstr arg_name) {
    if (length != required_length) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q length must be %d"), arg_name, required_length);
    }
    return length;
}

// int instead of uint because an index can be negative in some cases.
mp_int_t mp_arg_validate_index_range(mp_int_t index, mp_int_t min, mp_int_t max, qstr arg_name) {
    if (index < min || index > max) {
        mp_raise_IndexError_varg(MP_ERROR_TEXT("%q out of range"), arg_name, min, max);
    }
    return index;
}

mp_obj_t mp_arg_validate_type(mp_obj_t obj, const mp_obj_type_t *type, qstr arg_name) {
    if (!mp_obj_is_type(obj, type)) {
        mp_raise_TypeError_varg(MP_ERROR_TEXT("%q must be of type %q, not %q"), arg_name, type->name, mp_obj_get_type(obj)->name);
    }
    return obj;
}

mp_obj_t mp_arg_validate_type_in(mp_obj_t obj, const mp_obj_type_t *type, qstr arg_name) {
    if (!mp_obj_is_type(obj, type)) {
        mp_raise_TypeError_varg(MP_ERROR_TEXT("%q in %q must be of type %q, not %q"), MP_QSTR_object, arg_name, type->name, mp_obj_get_type(obj)->name);
    }
    return obj;
}

mp_obj_t mp_arg_validate_type_or_none(mp_obj_t obj, const mp_obj_type_t *type, qstr arg_name) {
    if (obj != mp_const_none && !mp_obj_is_type(obj, type)) {
        mp_raise_TypeError_varg(MP_ERROR_TEXT("%q must be of type %q or %q, not %q"), arg_name, type->name, MP_QSTR_None, mp_obj_get_type(obj)->name);
    }
    return obj;
}

mp_obj_t mp_arg_validate_type_string(mp_obj_t obj, qstr arg_name) {
    if (!mp_obj_is_str(obj)) {
        mp_raise_TypeError_varg(MP_ERROR_TEXT("%q must be of type %q, not %q"), arg_name, MP_QSTR_str, mp_obj_get_type(obj)->name);
    }
    return obj;
}

mp_int_t mp_arg_validate_type_int(mp_obj_t obj, qstr arg_name) {
    mp_int_t an_int;
    if (!mp_obj_get_int_maybe(obj, &an_int)) {
        mp_raise_TypeError_varg(MP_ERROR_TEXT("%q must be of type %q, not %q"), arg_name, MP_QSTR_int, mp_obj_get_type(obj)->name);
    }
    return an_int;
}

MP_NORETURN void mp_arg_error_invalid(qstr arg_name) {
    mp_raise_ValueError_varg(MP_ERROR_TEXT("Invalid %q"), arg_name);
}
