// audiooscillator module table for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/audiooscillator/__init__.h"
#include "shared-bindings/audiooscillator/Oscillator.h"

//| """A band-limited oscillator driven at audio rate
//|
//| The `audiooscillator` module has one oscillator - saw, pulse, triangle and
//| sine from one phase - whose pitch, linear FM, pulse width and hard sync
//| are audio streams, read sample by sample. It is not part of CircuitPython
//| upstream; it comes from PyDevices' audiodsp.
//|
//| `synthio`'s oscillator is a wavetable whose pitch moves once per block,
//| with no band limiting, no sync and no linear FM.
//|
//| """
//|
//| SAW: int
//| PULSE: int
//| TRIANGLE: int
//| SINE: int

static const mp_rom_map_elem_t audiooscillator_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiooscillator) },
    { MP_ROM_QSTR(MP_QSTR_Oscillator),
      MP_ROM_PTR(&audiooscillator_oscillator_type) },
    { MP_ROM_QSTR(MP_QSTR_SAW), MP_ROM_INT(AUDIODSP_OSCILLATOR_SAW) },
    { MP_ROM_QSTR(MP_QSTR_PULSE), MP_ROM_INT(AUDIODSP_OSCILLATOR_PULSE) },
    { MP_ROM_QSTR(MP_QSTR_TRIANGLE),
      MP_ROM_INT(AUDIODSP_OSCILLATOR_TRIANGLE) },
    { MP_ROM_QSTR(MP_QSTR_SINE), MP_ROM_INT(AUDIODSP_OSCILLATOR_SINE) },
};
static MP_DEFINE_CONST_DICT(audiooscillator_module_globals,
    audiooscillator_module_globals_table);

const mp_obj_module_t audiooscillator_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audiooscillator_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audiooscillator, audiooscillator_module);
