// audiovorbis module table. New code (not a port) -- see
// audiomixer/module.c for why this port uses a single registration file
// per module. CircuitPython has no audiovorbis; this module is PyDevices'.
//
// SPDX-License-Identifier: MIT

#include "audiovorbis/VorbisDecoder.h"
#include "cp_compat/audiodsp_build.h"

#include "py/obj.h"

static const mp_rom_map_elem_t audiovorbis_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiovorbis) },
    { MP_ROM_QSTR(MP_QSTR_VorbisDecoder), MP_ROM_PTR(&audiovorbis_vorbisdecoder_type) },
    AUDIODSP_BUILD_GLOBALS,
};
static MP_DEFINE_CONST_DICT(audiovorbis_module_globals, audiovorbis_module_globals_table);

const mp_obj_module_t audiovorbis_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audiovorbis_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audiovorbis, audiovorbis_module);
