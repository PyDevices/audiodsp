// audiobiquad module table. New code (not a port) -- see audiomixer/module.c
// for why this port uses a single registration file per module.
//
// Named apart from CircuitPython's `audiofilters` on purpose, and for the
// reason `audioecho` is named apart from `audiodelays`: this is not an
// extension of `audiofilters.Filter` or `audiofilters.Phaser` but a second
// pair of kernels whose state is float and whose tail therefore reaches
// exact zero. Adding that to the ported classes would have made audiodsp's
// copy of a CircuitPython module diverge from the one on a stock board,
// which is the one thing apply_cp_patches.sh is built to avoid.
//
// The mode constants are here rather than on the class so that
// `audiobiquad.NOTCH` reads the way `synthio.FilterMode.NOTCH` does; the
// numbering is the same, deliberately, so either can be handed to either.
//
// SPDX-License-Identifier: MIT

#include "audiobiquad/AllPass.h"
#include "audiobiquad/Biquad.h"

#include "cp_compat/audiodsp_build.h"

#include "py/obj.h"
#include "shared/audiodsp_filter_f32.h"

// The mode numbers as a tuple, so a caller can walk them the way the CPython
// twin's audiobiquad.MODES is walked.
static const mp_rom_obj_tuple_t audiobiquad_modes_tuple = {
    { &mp_type_tuple },
    7,
    {
        MP_ROM_INT(AUDIODSP_BIQUAD_F32_LOW_PASS),
        MP_ROM_INT(AUDIODSP_BIQUAD_F32_HIGH_PASS),
        MP_ROM_INT(AUDIODSP_BIQUAD_F32_BAND_PASS),
        MP_ROM_INT(AUDIODSP_BIQUAD_F32_NOTCH),
        MP_ROM_INT(AUDIODSP_BIQUAD_F32_PEAKING_EQ),
        MP_ROM_INT(AUDIODSP_BIQUAD_F32_LOW_SHELF),
        MP_ROM_INT(AUDIODSP_BIQUAD_F32_HIGH_SHELF),
    },
};

static const mp_rom_map_elem_t audiobiquad_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiobiquad) },
    AUDIODSP_BUILD_GLOBALS,
    { MP_ROM_QSTR(MP_QSTR_Biquad), MP_ROM_PTR(&audiobiquad_biquad_type) },
    { MP_ROM_QSTR(MP_QSTR_AllPass), MP_ROM_PTR(&audiobiquad_allpass_type) },
    { MP_ROM_QSTR(MP_QSTR_LOW_PASS),
      MP_ROM_INT(AUDIODSP_BIQUAD_F32_LOW_PASS) },
    { MP_ROM_QSTR(MP_QSTR_HIGH_PASS),
      MP_ROM_INT(AUDIODSP_BIQUAD_F32_HIGH_PASS) },
    { MP_ROM_QSTR(MP_QSTR_BAND_PASS),
      MP_ROM_INT(AUDIODSP_BIQUAD_F32_BAND_PASS) },
    { MP_ROM_QSTR(MP_QSTR_NOTCH), MP_ROM_INT(AUDIODSP_BIQUAD_F32_NOTCH) },
    { MP_ROM_QSTR(MP_QSTR_PEAKING_EQ),
      MP_ROM_INT(AUDIODSP_BIQUAD_F32_PEAKING_EQ) },
    { MP_ROM_QSTR(MP_QSTR_LOW_SHELF),
      MP_ROM_INT(AUDIODSP_BIQUAD_F32_LOW_SHELF) },
    { MP_ROM_QSTR(MP_QSTR_HIGH_SHELF),
      MP_ROM_INT(AUDIODSP_BIQUAD_F32_HIGH_SHELF) },
    { MP_ROM_QSTR(MP_QSTR_FRAMES),
      MP_ROM_INT(AUDIODSP_FILTER_F32_FRAMES) },
    { MP_ROM_QSTR(MP_QSTR_MAX_STAGES),
      MP_ROM_INT(AUDIODSP_FILTER_F32_MAX_STAGES) },
    { MP_ROM_QSTR(MP_QSTR_MODES), MP_ROM_PTR(&audiobiquad_modes_tuple) },
};
static MP_DEFINE_CONST_DICT(audiobiquad_module_globals,
    audiobiquad_module_globals_table);

const mp_obj_module_t audiobiquad_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audiobiquad_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audiobiquad, audiobiquad_module);
