// Compat shim for audiodsp: small CircuitPython binding helpers
// used pervasively across shared-bindings sources (deinit-guard errors,
// generic __repr__ and make_new construction for the common "settable
// properties" pattern), ported from CircuitPython's shared-bindings/util.h
// (upstream repo: https://github.com/adafruit/circuitpython, MIT).
//
// `path_exists()` is intentionally not ported: it wraps
// `common_hal_os_stat`, CircuitPython's port-specific os HAL, which is out
// of scope here and unused by anything in the audio/synthio call graph.
//
// SPDX-FileCopyrightText: Copyright (c) 2017 Dan Halbert for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#pragma once

#include "py/mpprint.h"
#include "py/runtime.h"

MP_NORETURN void raise_deinited_error(void);
void properties_print_helper(const mp_print_t *print, mp_obj_t self_in, const mp_arg_t *properties, size_t n_properties);
void properties_construct_helper(mp_obj_t self_in, const mp_arg_t *args, const mp_arg_val_t *vals, size_t n_properties);
