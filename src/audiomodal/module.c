// audiomodal module table. New code (not a port) -- see audiomixer/module.c
// for why this port uses a single registration file per module.
//
// One class. The name is the physics rather than the instrument, because the
// same node is a drum, a marimba bar, a bell or a wine glass depending only
// on which mode table it was handed.
//
// SPDX-License-Identifier: MIT

#include "audiomodal/Bank.h"

#include "cp_compat/audioif_build.h"

#include "py/obj.h"

static const mp_rom_map_elem_t audiomodal_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiomodal) },
    AUDIOIF_BUILD_GLOBALS,
    { MP_ROM_QSTR(MP_QSTR_Bank), MP_ROM_PTR(&audiomodal_bank_type) },
    // The block size, the ceiling and the decay bounds, so Python-side code
    // can size a bank and clamp a mode table without hard-coding any of
    // them. The CPython wrapper exports the same four names.
    //
    // The two decay bounds are in MILLISECONDS and are integers, where
    // `set_mode` takes seconds as a float. That looks inconsistent and is
    // deliberate: a float in a module table needs a `mp_obj_float_t` in ROM,
    // and both bounds are exact integers of milliseconds, so the conversion
    // has no rounding to get wrong and the constant costs nothing.
    { MP_ROM_QSTR(MP_QSTR_FRAMES), MP_ROM_INT(AUDIOIF_MODAL_FRAMES) },
    { MP_ROM_QSTR(MP_QSTR_MAX_MODES), MP_ROM_INT(AUDIOIF_MODAL_MAX_MODES) },
    { MP_ROM_QSTR(MP_QSTR_MIN_DECAY_MS), MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_MAX_DECAY_MS), MP_ROM_INT(30000) },
};
static MP_DEFINE_CONST_DICT(audiomodal_module_globals,
    audiomodal_module_globals_table);

const mp_obj_module_t audiomodal_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audiomodal_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audiomodal, audiomodal_module);
