// audiomath module table for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/audiomath/__init__.h"
#include "shared-bindings/audiomath/Multiply.h"
#include "shared-bindings/audiomath/SubOctave.h"

//| """Arithmetic on audio streams
//|
//| The `audiomath` module multiplies one audio stream by another, which is
//| ring modulation and, with a modulator that does not cross zero, amplitude
//| modulation. It is not part of CircuitPython upstream; it comes from
//| PyDevices' audiodsp.
//|
//| `synthio` can ring a note against an oscillator, but that reaches only
//| synthesized notes - not a microphone, a sample, or the output of another
//| effect - and an LFO-driven parameter updates once per block, far below the
//| hundreds of hertz a ring modulator wants.
//|
//| It also divides one stream down in frequency, with a comparator into a
//| chain of flip-flops - the analog octave divider, as in a Boss OC-2. That
//| is arithmetic on the signal against its own zero crossings, which is why
//| it lives here rather than beside the granular pitch shifter it does not
//| resemble.
//|
//| """

static const mp_rom_map_elem_t audiomath_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiomath) },
    { MP_ROM_QSTR(MP_QSTR_Multiply), MP_ROM_PTR(&audiomath_multiply_type) },
    { MP_ROM_QSTR(MP_QSTR_SubOctave),
      MP_ROM_PTR(&audiomath_suboctave_type) },
};

static MP_DEFINE_CONST_DICT(audiomath_module_globals,
    audiomath_module_globals_table);

const mp_obj_module_t audiomath_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audiomath_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audiomath, audiomath_module);
