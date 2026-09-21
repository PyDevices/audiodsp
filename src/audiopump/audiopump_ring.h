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
