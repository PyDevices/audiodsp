// audiomp3 module table. New code (not a port) -- see
// audiomixer/module.c for why this port uses a single registration file
// per module.
//
// SPDX-License-Identifier: MIT

#include "audiomp3/MP3Decoder.h"
#include "cp_compat/audiodsp_build.h"

#include "py/obj.h"

static const mp_rom_map_elem_t audiomp3_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiomp3) },
    AUDIODSP_BUILD_GLOBALS,
    { MP_ROM_QSTR(MP_QSTR_MP3Decoder), MP_ROM_PTR(&audiomp3_mp3file_type) },
};
static MP_DEFINE_CONST_DICT(audiomp3_module_globals, audiomp3_module_globals_table);

const mp_obj_module_t audiomp3_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audiomp3_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audiomp3, audiomp3_module);
