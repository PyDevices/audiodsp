// audioconvolve module table. New code (not a port) -- see audiomixer/module.c
// for why this port uses a single registration file per module.
//
// One class. The name is the operation rather than the effect, because the
// same node is a reverb, a cabinet, a spring tank or a filter depending only
// on which impulse it was handed.
//
// SPDX-License-Identifier: MIT

#include "audioconvolve/Convolver.h"

#include "cp_compat/audiodsp_build.h"

#include "py/obj.h"

static const mp_rom_map_elem_t audioconvolve_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audioconvolve) },
    AUDIODSP_BUILD_GLOBALS,
    { MP_ROM_QSTR(MP_QSTR_Convolver), MP_ROM_PTR(&audioconvolve_convolver_type) },
    // The block size and the ceiling, so Python-side code can size a
    // convolver in partitions without hard-coding either. The CPython
    // wrapper exports the same two names.
    { MP_ROM_QSTR(MP_QSTR_FRAMES), MP_ROM_INT(AUDIODSP_CONVOLVE_FRAMES) },
    { MP_ROM_QSTR(MP_QSTR_MAX_PARTITIONS),
      MP_ROM_INT(AUDIODSP_CONVOLVE_MAX_PARTITIONS) },
};
static MP_DEFINE_CONST_DICT(audioconvolve_module_globals,
    audioconvolve_module_globals_table);

const mp_obj_module_t audioconvolve_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audioconvolve_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audioconvolve, audioconvolve_module);
