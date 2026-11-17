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
// audiocore's audiosample_get_buffer/reset_buffer dispatch and
// every audiosample-shaped type after it (Synthesizer, Mixer, effects, …).
//
// CircuitPython's MICROPY_UNSAFE_PROTO variant (skips the name field
// entirely) is intentionally not ported: upstream's own comment on it says
// "not used in CircuitPython or tested," and no port this repository builds
// for defines it.
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"
#include "py/stream.h"

#define MP_PROTOCOL_HEAD \
    uint16_t name;
#define MP_PROTO_IMPLEMENT(n) .name = n,

const void *mp_proto_get(uint16_t name, mp_const_obj_t obj);
const void *mp_proto_get_or_throw(uint16_t name, mp_const_obj_t obj);

// mp_get_stream_raise() for an argument that must be a file. Mainline
// MicroPython keeps no name on a stream's protocol, so it takes an audio
// object's named protocol (audiosample, synthio_block) for a stream and the
// caller reads through it: a RawSample passed as a file died with a
// segmentation fault (audiodsp#428). CircuitPython names stream protocols
// too, so it refuses these with the same OSError this raises.
const mp_stream_p_t *audiodsp_get_stream_raise(mp_obj_t obj, int flags);
