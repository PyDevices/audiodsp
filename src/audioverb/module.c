// audioverb module table. New code (not a port) -- see audiomixer/module.c for
// why this port uses a single registration file per module.
//
// Named apart from CircuitPython's `audiofreeverb` on purpose: this is not an
// extension of `Freeverb` but a second reverberator, whose line lengths and
// output taps are handed in from Python rather than compiled in. Adding those
// to `Freeverb` would have made audiodsp's copy of a CircuitPython module
// diverge from the one on a stock board, which is the one thing
// apply_cp_patches.sh is built to avoid.
//
// SPDX-License-Identifier: MIT

#include "audioverb/Tank.h"

#include "cp_compat/audiodsp_build.h"

#include "py/obj.h"

static const mp_rom_map_elem_t audioverb_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audioverb) },
    AUDIODSP_BUILD_GLOBALS,
    { MP_ROM_QSTR(MP_QSTR_Tank), MP_ROM_PTR(&audioverb_tank_type) },
};
static MP_DEFINE_CONST_DICT(audioverb_module_globals,
    audioverb_module_globals_table);

const mp_obj_module_t audioverb_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audioverb_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audioverb, audioverb_module);
