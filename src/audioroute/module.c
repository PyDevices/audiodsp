// audioroute module table. New code (not a port) -- see audiomixer/module.c
// for why this port uses a single registration file per module.
//
// SplitterTap is exported for `isinstance` checks and repr readability only;
// it has no constructor, and taps come from `Splitter.tap(index)`.
//
// MidSide joins Splitter here rather than getting a module of its own
// because it is routing: it decides which signal reaches which channel,
// where Splitter decides which branch reaches which chain.
//
// Port is the third: it decides which signal reaches which CONSUMER, and it
// is the only one of the three a consumer is meant to hold forever. See
// Port.h.
//
// SPDX-License-Identifier: MIT

#include "audioroute/MidSide.h"
#include "audioroute/Port.h"
#include "audioroute/Splitter.h"
#include "audioroute/SplitterTap.h"

#include "cp_compat/audioif_build.h"

#include "py/obj.h"

static const mp_rom_map_elem_t audioroute_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audioroute) },
    AUDIOIF_BUILD_GLOBALS,
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
