// Compat shim for audiodsp, standing in for CircuitPython's
// supervisor/background_callback.h (upstream repo:
// https://github.com/adafruit/circuitpython, MIT) -- see
// docs/upstream-diff.md, "audiomp3: background_callback is synchronous
// here" for why this is a stub rather than a port.
//
// CircuitPython's own unix `coverage` build variant -- our parity oracle --
// already reduces the whole API to a same-thread synchronous call:
//
//   #if defined(MICROPY_UNIX_COVERAGE)
//   #define background_callback_add(buf, fn, arg) ((fn)((arg)))
//   #endif
//
// (shared-module/audiomp3/MP3Decoder.c). Mainline MicroPython has no
// supervisor, no background-task queue, and no interrupt-context input-buffer
// refill model to match CircuitPython's real (non-coverage) implementation
// against -- there is nothing to port here yet. This header makes that
// stand-in permanent and unconditional (not just "for the coverage build"),
// which is exactly the code path this port's own oracle-diff testing
// exercises. audiomp3's MP3Decoder is the only caller today.
//
// A real interrupt/scheduler-driven refill (to overlap decode with a
// `machine.I2S` DMA pump instead of decoding synchronously inside
// get_buffer) is a phase 8/10 concern for mcu output devices, not this one.
//
// SPDX-License-Identifier: MIT

#pragma once

#define background_callback_prevent() ((void)0)
#define background_callback_allow() ((void)0)
#define background_callback_add(cb, fn, arg) ((void)(cb), (fn)((arg)))

typedef struct background_callback {
    int unused;
} background_callback_t;
