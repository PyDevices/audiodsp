// audioroute module table for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/audioroute/__init__.h"
#include "shared-bindings/audioroute/MidSide.h"
#include "shared-bindings/audioroute/Port.h"
#include "shared-bindings/audioroute/Splitter.h"
#include "shared-bindings/audioroute/SplitterTap.h"

//| """Route audio between chains and between channels
//|
//| The `audioroute` module lets a single source feed several effect chains at
//| once - an exciter, a Haas widener, multiband splits - which a `audiomixer`
//| Mixer then sums back together, and it scales the difference between a
//| stereo pair's channels so a drive can work on the middle of an image
//| without smearing its sides. It is not part of CircuitPython upstream; it
//| comes from PyDevices' audiodsp, which in turn took `Splitter` from
//| micropython-vst3's audio engine. `MidSide` is audiodsp's own, and so is
//| `Port` - the wire an effect hands out so the thing holding its output
//| never has to be told the graph behind it changed.
//|
//| """

static const mp_rom_map_elem_t audioroute_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audioroute) },
    { MP_ROM_QSTR(MP_QSTR_MidSide), MP_ROM_PTR(&audioroute_midside_type) },
    { MP_ROM_QSTR(MP_QSTR_Port), MP_ROM_PTR(&audioroute_port_type) },
    { MP_ROM_QSTR(MP_QSTR_Splitter), MP_ROM_PTR(&audioroute_splitter_type) },
    { MP_ROM_QSTR(MP_QSTR_SplitterTap),
      MP_ROM_PTR(&audioroute_splitter_tap_type) },
};

static MP_DEFINE_CONST_DICT(audioroute_module_globals,
    audioroute_module_globals_table);

const mp_obj_module_t audioroute_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audioroute_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audioroute, audioroute_module);
