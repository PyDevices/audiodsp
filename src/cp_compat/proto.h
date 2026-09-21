// Compat shim for audiodsp: CircuitPython's named-protocol system
// (`MP_PROTOCOL_HEAD`, `MP_PROTO_IMPLEMENT`, `mp_proto_get[_or_throw]`),
// adapted from CircuitPython's py/proto.h (upstream repo:
// https://github.com/adafruit/circuitpython, MIT).
//
// Mainline MicroPython's `protocol` type slot holds one untyped `const
// void *` (used by e.g. mp_stream_p_t). CircuitPython layers a qstr name
// onto whatever that pointer refers to, so a lookup can confirm it got the
// *specific* protocol it asked for (e.g. "protocol_audiosample") rather
// than blindly reinterpreting a stream vtable as something else. Used by
// audiocore's audiosample_get_buffer/reset_buffer dispatch (tier 1) and
// every audiosample-shaped type after it (Synthesizer, Mixer, effects, …).
//
// CircuitPython's MICROPY_UNSAFE_PROTO variant (skips the name field
// entirely) is intentionally not ported: upstream's own comment on it says
// "not used in CircuitPython or tested," and it isn't defined by any port
// in this workspace.
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

#define MP_PROTOCOL_HEAD \
    uint16_t name;
#define MP_PROTO_IMPLEMENT(n) .name = n,

const void *mp_proto_get(uint16_t name, mp_const_obj_t obj);
const void *mp_proto_get_or_throw(uint16_t name, mp_const_obj_t obj);
