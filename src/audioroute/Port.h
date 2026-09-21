// audioroute.Port -- a wire whose identity never changes.
//
// Everything downstream of an effect takes its output ONCE. `Rack.__init__`
// reads `child.output` once, a user writes `mixer.voice[0].play(fx.output)`
// once, an audio output takes it once, and the C pump holds the tail it was
// handed for as long as it runs. But about twenty classes in `audioeffects`
// replace `self._output` when a Mix macro reaches 0 or leaves it,
// `Distortion._apply_macro` rebuilds its whole graph when Character crosses
// 0.5, and `Phaser._install_cascade` swaps the cascade node that IS its
// output on every Stages move. Nothing that already took the old one ever
// hears about it.
//
// So a Component ends in a Port and hands THAT out. Re-pointing the port is
// how a class changes what it plays; the object the consumer holds is the
// same object forever.
//
// What it costs is one function call per block and no bytes moved: the pull
// returns the source's own pointer, length and result unchanged. It is
// audioif's, in audioroute, because that is what this is -- routing decides
// which signal reaches which consumer, and a port is the smallest possible
// version of that decision. It is NOT in `audiocore`: that module is
// CircuitPython's, a node added to our copy would not exist on a stock
// board, and a class written against it would quietly be a different class
// there (the same rule that kept `SampleHold` out of `audiospeed`).
//
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "audiocore/__init__.h"
#include "py/obj.h"

typedef struct {
    audiosample_base_t base;
    //: What the port currently plays. One aligned word, so a pull either
    //: reads the whole old source or the whole new one -- and `play()` takes
    //: the pump lock anyway, because the format words beside it move with it.
    mp_obj_t source;
} audioroute_port_obj_t;

extern const mp_obj_type_t audioroute_port_type;
