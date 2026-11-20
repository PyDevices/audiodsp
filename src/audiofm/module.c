// audiofm module table. New code (not a port) -- see audiomixer/module.c
// for why this port uses a single registration file per module. The kernel
// it binds is adapted from Music Synthesizer for Android; see
// shared/audiodsp_fm.h.
//
// SPDX-License-Identifier: MIT

#include "audiofm/Patch.h"
#include "audiofm/Synth.h"

#include "cp_compat/audiodsp_build.h"

#include "py/obj.h"

static const mp_rom_map_elem_t audiofm_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiofm) },
    AUDIODSP_BUILD_GLOBALS,
    { MP_ROM_QSTR(MP_QSTR_Synth), MP_ROM_PTR(&audiofm_synth_type) },
    { MP_ROM_QSTR(MP_QSTR_Patch), MP_ROM_PTR(&audiofm_patch_type) },
    { MP_ROM_QSTR(MP_QSTR_Operator), MP_ROM_PTR(&audiofm_operator_type) },
    { MP_ROM_QSTR(MP_QSTR_FRAMES), MP_ROM_INT(AUDIODSP_FM_FRAMES) },
    { MP_ROM_QSTR(MP_QSTR_MAX_VOICES), MP_ROM_INT(AUDIODSP_FM_MAX_VOICES) },
};
static MP_DEFINE_CONST_DICT(audiofm_module_globals,
    audiofm_module_globals_table);

const mp_obj_module_t audiofm_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audiofm_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audiofm, audiofm_module);
