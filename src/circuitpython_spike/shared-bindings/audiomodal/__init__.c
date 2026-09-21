// audiomodal module table for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/audiomodal/__init__.h"
#include "shared-bindings/audiomodal/Bank.h"

//| """A bank of resonators, which is what a struck object is
//|
//| The `audiomodal` module rings a signal through N two-pole resonators in
//| parallel, summed in float and quantised once. It is not part of
//| CircuitPython upstream; it comes from PyDevices' audiodsp.
//|
//| Hit a drum head, a marimba bar, a bell or a wine glass and it rings as a
//| sum of decaying sinusoids at frequencies that are not harmonics of
//| anything. Hand `Bank` that list and a short noise burst for the stick, and
//| it is that object.
//|
//| Not a mode on `audiobiquad.Biquad`, for three reasons that are measured
//| rather than argued. It sums before the quantiser, so a mode 40 dB down is
//| not buried in its own quantisation noise - a six-mode kick built out of
//| `Biquad` nodes measures a 4113 Hz spectral centroid against 73.8 Hz summed
//| in float. It takes a 60 dB decay time rather than a Q, because
//| `audiobiquad` caps Q at 60 and a 3 kHz cymbal partial ringing for three
//| seconds is Q = 4093. And it skips modes that have finished ringing, which
//| is what lets a kit hold ten drums resident and pay only for the ones
//| sounding.
//|
//| """

static const mp_rom_map_elem_t audiomodal_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiomodal) },
    { MP_ROM_QSTR(MP_QSTR_Bank), MP_ROM_PTR(&audiomodal_bank_type) },
    { MP_ROM_QSTR(MP_QSTR_FRAMES), MP_ROM_INT(AUDIODSP_MODAL_FRAMES) },
    { MP_ROM_QSTR(MP_QSTR_MAX_MODES), MP_ROM_INT(AUDIODSP_MODAL_MAX_MODES) },
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
