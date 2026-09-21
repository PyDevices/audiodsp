// audiodynamics module table. New code (not a port) -- see audiomixer/module.c
// for why this port uses a single registration file per module.
//
// The mode constants keep the DYN_ prefix and the integer values they had as
// `vstaudio.DYN_*`: they are what the effects library passes, and what any
// saved plugin state already holds.
//
// SPDX-License-Identifier: MIT

#include "audiodynamics/Dynamics.h"

#include "cp_compat/audiodsp_build.h"

#include "py/obj.h"

static const mp_rom_map_elem_t audiodynamics_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiodynamics) },
    AUDIODSP_BUILD_GLOBALS,
    { MP_ROM_QSTR(MP_QSTR_Dynamics), MP_ROM_PTR(&audiodynamics_dynamics_type) },

    { MP_ROM_QSTR(MP_QSTR_DYN_COMPRESS),
      MP_ROM_INT(AUDIODSP_DYNAMICS_COMPRESS) },
    { MP_ROM_QSTR(MP_QSTR_DYN_LIMIT), MP_ROM_INT(AUDIODSP_DYNAMICS_LIMIT) },
    { MP_ROM_QSTR(MP_QSTR_DYN_EXPAND), MP_ROM_INT(AUDIODSP_DYNAMICS_EXPAND) },
    { MP_ROM_QSTR(MP_QSTR_DYN_GATE), MP_ROM_INT(AUDIODSP_DYNAMICS_GATE) },
    { MP_ROM_QSTR(MP_QSTR_DYN_TRANSIENT),
      MP_ROM_INT(AUDIODSP_DYNAMICS_TRANSIENT) },
};
static MP_DEFINE_CONST_DICT(audiodynamics_module_globals,
    audiodynamics_module_globals_table);

const mp_obj_module_t audiodynamics_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audiodynamics_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audiodynamics, audiodynamics_module);
