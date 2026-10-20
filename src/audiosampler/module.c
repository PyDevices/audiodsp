// audiosampler module table. New code (not a port) -- see audiomixer/module.c
// for why this port uses a single registration file per module.
//
// Two classes, as audiomixer has: the Sampler, which is the audio source, and
// its voices, which are what a note is played on. And a third, SampleBuffer:
// memory for the samples they read, outside the collected heap.
//
// SPDX-License-Identifier: MIT

#include "audiosampler/SampleBuffer.h"
#include "audiosampler/Sampler.h"
#include "audiosampler/SamplerVoice.h"

#include "cp_compat/audiodsp_build.h"

#include "py/obj.h"

static const mp_rom_map_elem_t audiosampler_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiosampler) },
    AUDIODSP_BUILD_GLOBALS,
    { MP_ROM_QSTR(MP_QSTR_Sampler), MP_ROM_PTR(&audiosampler_sampler_type) },
    { MP_ROM_QSTR(MP_QSTR_SamplerVoice),
      MP_ROM_PTR(&audiosampler_samplervoice_type) },
    { MP_ROM_QSTR(MP_QSTR_SampleBuffer),
      MP_ROM_PTR(&audiosampler_samplebuffer_type) },
    // The block size, the two ceilings and the three loop modes, so a reader
    // writes a SoundFont's sampleModes value straight in. The CPython wrapper
    // exports the same six names.
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
