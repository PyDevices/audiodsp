// audiodynamics module table for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/audiodynamics/__init__.h"
#include "shared-bindings/audiodynamics/Dynamics.h"

//| """Envelope-follower dynamics processing
//|
//| The `audiodynamics` module provides compression, limiting, downward
//| expansion, gating and transient shaping for audio chains. It is not part of
//| CircuitPython upstream; it comes from PyDevices' audiodsp, which in turn took
//| it from micropython-vst3's audio engine.
//|
//| """
//|
//| DYN_COMPRESS: int
//| """Reduce level above the threshold by ``ratio``, with a soft ``knee_db``."""
//|
//| DYN_LIMIT: int
//| """Hold the level at the threshold."""
//|
//| DYN_EXPAND: int
//| """Push level below the threshold further down, to a floor of -60 dB
//| unless ``depth_db`` names another one."""
//|
//| DYN_GATE: int
//| """Close hard below the threshold, to a floor of -80 dB unless
//| ``depth_db`` names another one. With ``hold_ms`` set it closes through a
//| four-stage envelope instead."""
//|
//| DYN_TRANSIENT: int
//| """Shape attacks and sustains by comparing a fast and a slow detector."""

static const mp_rom_map_elem_t audiodynamics_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiodynamics) },
    { MP_ROM_QSTR(MP_QSTR_Dynamics), MP_ROM_PTR(&audiodynamics_dynamics_type) },

    { MP_ROM_QSTR(MP_QSTR_DYN_COMPRESS),
      MP_ROM_INT(AUDIODSP_DYNAMICS_COMPRESS) },
    { MP_ROM_QSTR(MP_QSTR_DYN_LIMIT), MP_ROM_INT(AUDIODSP_DYNAMICS_LIMIT) },
    { MP_ROM_QSTR(MP_QSTR_DYN_EXPAND), MP_ROM_INT(AUDIODSP_DYNAMICS_EXPAND) },
    { MP_ROM_QSTR(MP_QSTR_DYN_GATE), MP_ROM_INT(AUDIODSP_DYNAMICS_GATE) },
    { MP_ROM_QSTR(MP_QSTR_DYN_TRANSIENT),
      MP_ROM_INT(AUDIODSP_DYNAMICS_TRANSIENT) },
};

static MP_DEFINE_CONST_DICT(audiodynamics_module_globals,
    audiodynamics_module_globals_table);

const mp_obj_module_t audiodynamics_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audiodynamics_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audiodynamics, audiodynamics_module);
