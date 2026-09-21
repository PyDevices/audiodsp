// audioladder module table. New code (not a port) -- see audiomixer/module.c
// for why this port uses a single registration file per module.
//
// Named apart from CircuitPython's `audiofilters` on purpose: this is not an
// extension of `audiofilters.Filter` but a filter of a different kind, whose
// character is what a nonlinearity inside a feedback loop does and which a
// cascade of linear biquads therefore cannot be. Adding it to `Filter` would
// have made audiodsp's copy of a CircuitPython module diverge from the one on
// a stock board, which is the one thing apply_cp_patches.sh is built to
// avoid.
//
// SPDX-License-Identifier: MIT

#include "audioladder/Ladder.h"

#include "cp_compat/audiodsp_build.h"

#include "py/obj.h"

static const mp_rom_map_elem_t audioladder_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audioladder) },
    AUDIODSP_BUILD_GLOBALS,
    { MP_ROM_QSTR(MP_QSTR_Ladder), MP_ROM_PTR(&audioladder_ladder_type) },
};
static MP_DEFINE_CONST_DICT(audioladder_module_globals,
    audioladder_module_globals_table);

const mp_obj_module_t audioladder_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audioladder_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audioladder, audioladder_module);
