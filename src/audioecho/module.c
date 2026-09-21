// audioecho module table. New code (not a port) -- see audiomixer/module.c
// for why this port uses a single registration file per module.
//
// Named apart from CircuitPython's `audiodelays` on purpose: this is not an
// extension of `audiodelays.Echo` but a second delay that has a filter, a
// soft-clip and a cross-feed inside its feedback loop. Adding those to
// `Echo` would have made audiodsp's copy of a CircuitPython module diverge
// from the one on a stock board, which is the one thing apply_cp_patches.sh
// is built to avoid.
//
// SPDX-License-Identifier: MIT

#include "audioecho/FeedbackDelay.h"

#include "cp_compat/audiodsp_build.h"

#include "py/obj.h"

static const mp_rom_map_elem_t audioecho_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audioecho) },
    AUDIODSP_BUILD_GLOBALS,
    { MP_ROM_QSTR(MP_QSTR_FeedbackDelay),
      MP_ROM_PTR(&audioecho_feedback_delay_type) },
};
static MP_DEFINE_CONST_DICT(audioecho_module_globals,
    audioecho_module_globals_table);

const mp_obj_module_t audioecho_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audioecho_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audioecho, audioecho_module);
