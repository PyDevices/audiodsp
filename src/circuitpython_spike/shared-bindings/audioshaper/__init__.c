// audioshaper module table for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/audioshaper/__init__.h"
#include "shared-bindings/audioshaper/SampleHold.h"
#include "shared-bindings/audioshaper/Waveshaper.h"

//| """A waveshaper whose curve is data, shaped above the sample rate
//|
//| The `audioshaper` module reads a nonlinearity out of a table you supply
//| and applies it at two, four or eight times the sample rate, between a
//| matched pair of polyphase all-pass half-band filters. It is not part of
//| CircuitPython upstream; it comes from PyDevices' audioif.
//|
//| `audiofilters.Distortion` is the plain version of the same idea: one of
//| four fixed curves, at the base rate. The curve is where a drive circuit
//| lives -- diodes in a feedback loop, diodes to ground, a biased germanium
//| pair are each a different shape -- and a nonlinearity at the base rate
//| folds the harmonics it makes above Nyquist straight back onto the signal.
//|
//| `SampleHold` is the module's other node, and lo-fi's other half: the
//| waveshaper quantises the value, the hold quantises the time. It holds at
//| an exact rational ratio, which is what a pair of `audiospeed.SpeedChanger`
//| nodes at 16.16 rates cannot do.
//|
//| """

static const mp_rom_map_elem_t audioshaper_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audioshaper) },
    { MP_ROM_QSTR(MP_QSTR_Waveshaper),
      MP_ROM_PTR(&audioshaper_waveshaper_type) },
    { MP_ROM_QSTR(MP_QSTR_SampleHold),
      MP_ROM_PTR(&audioshaper_samplehold_type) },
};

static MP_DEFINE_CONST_DICT(audioshaper_module_globals,
    audioshaper_module_globals_table);

const mp_obj_module_t audioshaper_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audioshaper_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audioshaper, audioshaper_module);
