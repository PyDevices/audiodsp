// audiooscillator module table. New code (not a port) -- see
// audiomixer/module.c for why this port uses a single registration file per
// module.
//
// A module of its own rather than an addition to synthio: synthio is
// CircuitPython's, and an oscillator added to audiodsp's copy of it would not
// exist on a stock board.
//
// SPDX-License-Identifier: MIT

#include "audiooscillator/Oscillator.h"

#include "cp_compat/audiodsp_build.h"

#include "py/obj.h"

static const mp_rom_map_elem_t audiooscillator_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiooscillator) },
    AUDIODSP_BUILD_GLOBALS,
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
