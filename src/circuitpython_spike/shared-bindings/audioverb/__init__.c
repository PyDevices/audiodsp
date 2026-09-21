// audioverb module table for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/audioverb/__init__.h"
#include "shared-bindings/audioverb/Tank.h"

//| """An algorithmic reverberation tank whose network comes from Python
//|
//| The `audioverb` module is Dattorro's plate reverberator with its twelve
//| line lengths and its output taps handed in rather than compiled in. It is
//| not part of CircuitPython upstream; it comes from PyDevices' audiodsp.
//|
//| `audiofreeverb.Freeverb` is the fixed version of the same idea: one
//| Schroeder/Moorer comb-and-all-pass bank, its line lengths constants, no
//| modulated taps and no dispersive input chain. A plate, a hall, a room and
//| a chamber are different line-length sets and different tap positions, not
//| one topology at four mix settings, which is why they need a different
//| module rather than another argument.
//|
//| """

static const mp_rom_map_elem_t audioverb_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audioverb) },
    { MP_ROM_QSTR(MP_QSTR_Tank), MP_ROM_PTR(&audioverb_tank_type) },
};

static MP_DEFINE_CONST_DICT(audioverb_module_globals,
    audioverb_module_globals_table);

const mp_obj_module_t audioverb_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audioverb_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audioverb, audioverb_module);
