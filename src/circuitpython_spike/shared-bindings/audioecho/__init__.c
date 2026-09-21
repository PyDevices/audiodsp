// audioecho module table for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/audioecho/__init__.h"
#include "shared-bindings/audioecho/FeedbackDelay.h"

//| """A delay with a filter inside its feedback loop
//|
//| The `audioecho` module is a delay line whose feedback path carries a
//| low-pass, a high-pass, a soft-clip, per-sample delay modulation and a
//| cross-feed between the channels. It is not part of CircuitPython upstream;
//| it comes from PyDevices' audiodsp.
//|
//| `audiodelays.Echo` is the plain version of the same idea: its feedback
//| path is the echo times a decay and nothing else. Tape, analog and true
//| ping-pong delays are all named after what happens in that path, which is
//| why they need a different module rather than another argument.
//|
//| """

static const mp_rom_map_elem_t audioecho_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audioecho) },
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
