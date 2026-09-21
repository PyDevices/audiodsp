// audiobiquad module table for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/audiobiquad/__init__.h"
#include "shared-bindings/audiobiquad/AllPass.h"
#include "shared-bindings/audiobiquad/Biquad.h"

//| """Filters whose tails reach exact zero
//|
//| The `audiobiquad` module is an RBJ biquad and a first-order all-pass
//| cascade, both with float state. It is not part of CircuitPython upstream;
//| it comes from PyDevices' audiodsp.
//|
//| `audiofilters.Filter` (over `synthio.Biquad`) and `audiofilters.Phaser`
//| are the integer versions of these two, and both can settle on a non-zero
//| state and hold it: fed silence after a burst, they can output a constant
//| of one to a few LSB for ever. Anything that has to be silent when its
//| input is silent needs arithmetic that can reach zero, which is what this
//| module is for.
//|
//| The all-pass carries one more thing `audiofilters.Phaser` cannot: its
//| feedback is not clamped to 0.1..0.9, so zero is zero and the notches of a
//| feedback-free phaser are true nulls.
//|
//| """

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
    { MP_ROM_QSTR(MP_QSTR_FRAMES), MP_ROM_INT(AUDIODSP_FILTER_F32_FRAMES) },
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
