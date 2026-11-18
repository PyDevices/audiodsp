// audioroute.SplitterTap bindings for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "shared-bindings/audioroute/SplitterTap.h"
#include "shared-bindings/audiocore/__init__.h"

#include "shared/runtime/context_manager_helpers.h"
#include "py/runtime.h"

//| class SplitterTap:
//|     """One branch's view of a `Splitter`'s stream.
//|
//|     Taps are built by the Splitter and handed out by `Splitter.tap`; there
//|     is no constructor."""
//|
//|
// `deinit()` releases what this binding holds and marks the node
// deinitialised, so the guarded getters raise afterwards. The MicroPython
// binding of this same type gained it on 2026-09-09 (audiodsp#58, #60, #63) and
// this copy did not, which is audiodsp#75: the two bindings are hand-written and
// nothing held them to each other. Same fields, same order, deliberately.
static mp_obj_t audioroute_splitter_tap_deinit(mp_obj_t self_in) {
    audioroute_splitter_tap_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    self->owner = mp_const_none;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audioroute_splitter_tap_deinit_obj, audioroute_splitter_tap_deinit);

static const mp_rom_map_elem_t audioroute_splitter_tap_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audioroute_splitter_tap_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audioroute_splitter_tap_locals_dict,
    audioroute_splitter_tap_locals_dict_table);

static const audiosample_p_t audioroute_splitter_tap_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)
        audioroute_splitter_tap_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)
        audioroute_splitter_tap_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audioroute_splitter_tap_type,
    MP_QSTR_SplitterTap,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    locals_dict, &audioroute_splitter_tap_locals_dict,
    protocol, &audioroute_splitter_tap_proto
    );
