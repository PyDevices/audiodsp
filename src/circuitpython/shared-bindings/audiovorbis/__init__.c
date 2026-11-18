// audiovorbis module table for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/audiovorbis/__init__.h"
#include "shared-bindings/audiovorbis/VorbisDecoder.h"

//| """Support for Ogg Vorbis audio files
//|
//| The `audiovorbis` module decodes Ogg Vorbis files for playback. It is not
//| part of CircuitPython upstream, which has no Vorbis module; it comes from
//| PyDevices' audiodsp. `VorbisDecoder` is shaped like
//| `audiomp3.MP3Decoder`, so either plays wherever the other does.
//|
//| The decoder is Xiph's Tremor, integer-only, under its own BSD-3-Clause
//| licence.
//| """

static const mp_rom_map_elem_t audiovorbis_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiovorbis) },
    { MP_ROM_QSTR(MP_QSTR_VorbisDecoder), MP_ROM_PTR(&audiovorbis_vorbisdecoder_type) },
};

static MP_DEFINE_CONST_DICT(audiovorbis_module_globals, audiovorbis_module_globals_table);

const mp_obj_module_t audiovorbis_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audiovorbis_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audiovorbis, audiovorbis_module);
