// Compat shim for audiodsp. See context_manager_helpers.h.
//
// Ported verbatim from CircuitPython's
// shared/runtime/context_manager_helpers.c (upstream repo:
// https://github.com/adafruit/circuitpython, MIT).
//
// SPDX-FileCopyrightText: Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include "cp_compat/context_manager_helpers.h"

#include "py/obj.h"
#include "py/runtime.h"

static mp_obj_t default___exit__(size_t n_args, const mp_obj_t *args) {
    mp_obj_t dest[2];
    mp_load_method(args[0], MP_QSTR_deinit, dest);
    mp_call_method_n_kw(0, 0, dest);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(default___exit___obj, 4, 4, default___exit__);
