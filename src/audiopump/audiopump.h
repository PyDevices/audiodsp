// The two things the platform driver needs from the engine.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// The traffic goes the other way almost everywhere: the engine asks the driver
// for a thread, a mutex, a clock and a sink through shared/audioif_port.h. These
// two go this way because the state they touch is the engine's.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"

#ifdef __cplusplus
extern "C" {
#endif

// Arm the soft-reset guard. Anything that takes ownership of hardware calls
// this, because a soft reset frees the heap out from under whatever is
// holding it and the guard's finaliser is how the teardown gets run at all --
// the esp32 port has no hook a user C module can register in its soft-reset
// path, but gc_sweep_all() runs __del__ on every object that has one. A
// driver that opens an I2S channel without a pump spawned still needs that
// channel closed.
void audiopump_arm_guard(void);

// True while a pump is adopted and its loop has not finished. Asked by a
// driver entry point that cannot share the hardware with a running pump --
// two owners of one I2S channel is the failure that sounds like silence.
bool audiopump_is_running(void);

// --- driving the pump from C ----------------------------------------------
//
// The same seven things `audiopump.spawn/stop/join/park/unpark` do from
// Python, for a binding that presents the pump as a DEVICE rather than as a
// pump -- `audiobusio.I2SOut(...).play(sample)` is the first one. They exist
// as C rather than as `mp_load_attr(audiopump, ...)` for two reasons, and the
// second is the one that bites: a stale `audiopump.py` anywhere on sys.path
// shadows the built-in module (audiodev/pump.py carries a comment about
// exactly this), and a device binding that silently drove a different object
// would be very hard to see. These do not go through sys.modules at all.
//
// Every one of them runs on the interpreter thread. `audiopump_c_spawn`
// raises, so it needs an nlr handler above it like any other call that can.
// A source that is not already what the sink wants has to be CONVERTED, and
// the conversion belongs on the PUMP's thread: it is a few integer
// operations a sample, and the interpreter cannot be relied on to arrive in
// time. When it was the interpreter's job -- a ring topped up from a
// scheduler node -- CircuitPython's own `I2SOut` docstring example, whose
// sine is unsigned, starved 67 blocks in 2 s on the P4.
//
// Hand spawn() one of these and every block is converted into `scratch`
// between the pull and the sink, as signed 16-bit stereo. NULL means the
// source already is that and nothing is copied.
//
// `scratch` must be writable and must outlive the pump -- the loop holds the
// raw pointer, so spawn roots it -- with room for one pulled block expanded
// to stereo signed 16-bit: `max_buffer_length / in_frame * 4` bytes.
typedef struct _audiopump_convert_t {
    mp_obj_t scratch;
    uint8_t bits;      // source bits per sample: 8 or 16
    uint8_t channels;  // source channel count: 1 or 2
    bool is_signed;    // source samples signed
} audiopump_convert_t;

int audiopump_c_spawn(mp_obj_t sample, mp_obj_t status, uint64_t blocks,
    mp_obj_t sink, bool loop, bool pace, int core, uint32_t timeout_ms,
    const audiopump_convert_t *convert);
void audiopump_c_stop(void);
bool audiopump_c_join(uint32_t timeout_ms);
bool audiopump_c_park(uint32_t timeout_us);
void audiopump_c_unpark(void);
bool audiopump_c_parked(void);
// Layout of the status bytearray: 32 x uint64_t, little endian, written by
// the loop and read by Python with struct.unpack_from. Here as well as in
// audiopump.c because a C caller has to size the buffer and name a word.
#define AUDIOPUMP_STATUS_WORDS (32)
#define AUDIOPUMP_STATUS_BYTES (AUDIOPUMP_STATUS_WORDS * 8)

enum {
    AUDIOPUMP_STATUS_BLOCKS = 0,      // blocks pulled
    AUDIOPUMP_STATUS_BYTES_SEEN = 1,  // bytes seen
    AUDIOPUMP_STATUS_DIGEST = 2,      // FNV-1a 64 over every byte, in order
    AUDIOPUMP_STATUS_LAST_RESULT = 3, // last audioif_buffer_result_t
    AUDIOPUMP_STATUS_RUNNING = 4,     // 1 while the loop is running
    AUDIOPUMP_STATUS_ERROR = 5,       // 0 none, 1 buffer, 2 null, 3 done
    AUDIOPUMP_STATUS_SINK_TIMEOUTS = 14,
    AUDIOPUMP_STATUS_FAULT = 24,      // why a pull gave up
};

// One of the status words the loop publishes; 0 for an index past the end.
uint64_t audiopump_c_status(unsigned index);

// True on a port where spawn() could not make a thread -- WebAssembly, and
// any build whose driver did not bind. There the loop advances only when
// somebody calls this, so a binding that polls `playing` has to drive it or
// the poll never ends.
bool audiopump_c_service_mode(void);
void audiopump_c_service(uint64_t budget);

#ifdef __cplusplus
}
#endif
