// audiopump module table for CircuitPython.
//
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices

#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/audiopump/__init__.h"
#include "shared-bindings/audiopump/Ring.h"

//| """A stream sample your code fills, played by CircuitPython's audio outputs
//|
//| The `audiopump` module has `Ring`: an audio sample you write PCM into from
//| Python, which `audiobusio`, `audioio`, `audiopwmio` and `audiomixer` pull
//| in C as they would any other sample. It lets audio computed in Python, or
//| rendered by a graph of audio nodes, play without a gap. It is not part of
//| CircuitPython upstream; it comes from PyDevices' audiodsp.
//|
//| On MicroPython, audiodsp's `audiopump` also runs the pull loop itself.
//| CircuitPython's outputs already pull, so here the module carries only
//| `Ring`, and code written against `audiopump.Ring` runs on both.
//| """

static const mp_rom_map_elem_t audiopump_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiopump) },
    { MP_ROM_QSTR(MP_QSTR_Ring), MP_ROM_PTR(&audiopump_ring_type) },
};
static MP_DEFINE_CONST_DICT(audiopump_module_globals, audiopump_module_globals_table);

const mp_obj_module_t audiopump_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audiopump_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audiopump, audiopump_module);
