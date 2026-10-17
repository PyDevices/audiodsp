// audiosampler module table for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/audiosampler/__init__.h"
#include "shared-bindings/audiosampler/Sampler.h"
#include "shared-bindings/audiosampler/SamplerVoice.h"

//| """Play recorded notes at any pitch
//|
//| The `audiosampler` module plays N voices, each reading a mono 16-bit
//| buffer at its own pitch through a 4-point Hermite interpolator under a
//| per-sample DAHDSR envelope, mixed in float and quantised once. It is not
//| part of CircuitPython upstream; it comes from PyDevices' audiodsp.
//|
//| `audiomixer.Mixer` plays sources at their own rate and `synthio` plays
//| single-cycle waveforms. Neither can take a recorded note, play it a
//| fourth up, loop a region of it while the key is held and let the rest of
//| it out when the key lifts. `Sampler` does that, which is what a SoundFont
//| instrument and every drum machine need.
//|
//| The three loop modes are SoundFont's ``sampleModes`` under their own
//| numbers: `LOOP_NONE` (0) plays to the end and stops, `LOOP_FOREVER` (1)
//| loops until `SamplerVoice.release()` or `SamplerVoice.stop()`, and
//| `LOOP_UNTIL_RELEASE` (3) loops while the key is held, then plays on past
//| the loop to the end of the buffer.
//|
//| """

//| FRAMES: int
//| """Frames in one output block by default (256)."""
//|
//| MAX_FRAMES: int
//| """The most frames a block may carry (1024)."""
//|
//| MAX_VOICES: int
//| """The most voices one Sampler may carry (64)."""
//|
//| LOOP_NONE: int
//| """Play to the end of the buffer and stop (SoundFont sampleModes 0)."""
//|
//| LOOP_FOREVER: int
//| """Loop ``[loop_start, loop_end)`` until released or stopped (1)."""
//|
//| LOOP_UNTIL_RELEASE: int
//| """Loop while held; on release, play on past the loop to the end (3)."""
//|

static const mp_rom_map_elem_t audiosampler_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiosampler) },
    { MP_ROM_QSTR(MP_QSTR_Sampler), MP_ROM_PTR(&audiosampler_sampler_type) },
    { MP_ROM_QSTR(MP_QSTR_SamplerVoice),
      MP_ROM_PTR(&audiosampler_samplervoice_type) },
    { MP_ROM_QSTR(MP_QSTR_FRAMES), MP_ROM_INT(AUDIODSP_SAMPLER_FRAMES) },
    { MP_ROM_QSTR(MP_QSTR_MAX_FRAMES),
      MP_ROM_INT(AUDIODSP_SAMPLER_MAX_FRAMES) },
    { MP_ROM_QSTR(MP_QSTR_MAX_VOICES),
      MP_ROM_INT(AUDIODSP_SAMPLER_MAX_VOICES) },
    { MP_ROM_QSTR(MP_QSTR_LOOP_NONE), MP_ROM_INT(AUDIODSP_SAMPLER_LOOP_NONE) },
    { MP_ROM_QSTR(MP_QSTR_LOOP_FOREVER),
      MP_ROM_INT(AUDIODSP_SAMPLER_LOOP_FOREVER) },
    { MP_ROM_QSTR(MP_QSTR_LOOP_UNTIL_RELEASE),
      MP_ROM_INT(AUDIODSP_SAMPLER_LOOP_UNTIL_RELEASE) },
};

static MP_DEFINE_CONST_DICT(audiosampler_module_globals,
    audiosampler_module_globals_table);

const mp_obj_module_t audiosampler_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audiosampler_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audiosampler, audiosampler_module);
