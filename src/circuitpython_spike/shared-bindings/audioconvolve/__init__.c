// audioconvolve module table for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/audioconvolve/__init__.h"
#include "shared-bindings/audioconvolve/Convolver.h"

//| """Convolution with an impulse response
//|
//| The `audioconvolve` module applies a measured or synthesized impulse
//| response to an audio stream, by uniform-partitioned overlap-save FFT
//| convolution. It is not part of CircuitPython upstream; it comes from
//| PyDevices' audiodsp.
//|
//| It is the one effect the rest of the palette cannot approximate.
//| `audiofreeverb` is a network of delays that *sounds like* a room;
//| convolving with a room's recorded impulse *is* that room. The same node is
//| a hall, a guitar cabinet, a spring tank or a telephone, depending only on
//| which impulse it was handed.
//|
//| Mind what it costs. Each 256-tap partition is about 2 KB and the
//| arithmetic grows with the count, so a one-second impulse is over a
//| megabyte and belongs on a board with PSRAM. A cabinet is a thousand taps
//| and runs comfortably on much less.
//|
//| """

static const mp_rom_map_elem_t audioconvolve_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audioconvolve) },
    { MP_ROM_QSTR(MP_QSTR_Convolver),
      MP_ROM_PTR(&audioconvolve_convolver_type) },
    // The block size and the ceiling, so Python-side code can size a
    // convolver in partitions without hard-coding either. The CPython
    // wrapper exports the same two names.
    { MP_ROM_QSTR(MP_QSTR_FRAMES), MP_ROM_INT(AUDIODSP_CONVOLVE_FRAMES) },
    { MP_ROM_QSTR(MP_QSTR_MAX_PARTITIONS),
      MP_ROM_INT(AUDIODSP_CONVOLVE_MAX_PARTITIONS) },
};

static MP_DEFINE_CONST_DICT(audioconvolve_module_globals,
    audioconvolve_module_globals_table);

const mp_obj_module_t audioconvolve_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audioconvolve_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audioconvolve, audioconvolve_module);
