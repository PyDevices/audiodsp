// audioladder module table for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/audioladder/__init__.h"
#include "shared-bindings/audioladder/Ladder.h"

//| """A transistor ladder filter
//|
//| The `audioladder` module is four one-pole stages round a global feedback
//| loop with an odd saturator inside the loop. It is not part of
//| CircuitPython upstream; it comes from PyDevices' audiodsp.
//|
//| `audiofilters.Filter` is the linear version of the same shape, and a
//| better one at what it does: a cascade of biquads tracks a resonant
//| low-pass to a fraction of a decibel. What it cannot do is what this
//| module is for. At a feedback of 4 a ladder sustains a tone of its own
//| from silence, at its cutoff; the saturator inside the loop is what stops
//| that tone growing without bound, and is also why the filter's character
//| follows how hard the input hits it. None of that exists in a linear
//| filter at any setting.
//|
//| """

static const mp_rom_map_elem_t audioladder_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audioladder) },
    { MP_ROM_QSTR(MP_QSTR_Ladder), MP_ROM_PTR(&audioladder_ladder_type) },
};

static MP_DEFINE_CONST_DICT(audioladder_module_globals,
    audioladder_module_globals_table);

const mp_obj_module_t audioladder_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audioladder_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audioladder, audioladder_module);
