// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// audiopump.Ring -- the push side of the one mechanism.
//
// `audiopump.Input` makes a microphone a source a graph can be built on.
// `Ring` does the same for anything Python has the bytes of: earful's
// stream, a WAV a prefetcher is reading, usbif's audio, a `PCMOutput`
// write. Python is the single producer, the pump's thread the single
// consumer, and because the whole thing is an audiosample the pushed
// stream can be the pump's tail by itself or sit behind a Mixer next to a
// synth with a chain of effects on it.

#pragma once

#include "py/obj.h"

extern const mp_obj_type_t audiopump_ring_type;

// A C producer's write, for another module that moves packets into a Ring
// from its own task (a USB host's isochronous IN stream): `ctx` is the
// Ring's pointer (MP_OBJ_TO_PTR). Takes whole frames, as many as fit, and
// returns the bytes taken; never blocks, allocates or raises. One producer
// at a time: while a C producer feeds the Ring, don't also write() to it.
#define AUDIOPUMP_RING_HAS_SINK_WRITE (1)
uint32_t audiopump_ring_sink_write(void *ctx, const uint8_t *src, uint32_t len);
