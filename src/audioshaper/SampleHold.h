// audioshaper.SampleHold -- MicroPython bindings over the runtime-neutral
// sample-and-hold in shared/audioif_samplehold.c.
//
// In `audioshaper` rather than `audiospeed`, and that is the whole decision.
// `audiospeed` is a CircuitPython port: its `SpeedChanger` rate is 16.16 fixed
// point on a stock board, an exact-rational rate added here would not exist
// there, and a class written against it would silently be a different effect.
// The same rule that keeps the waveshaper out of `audiofilters.Distortion`
// keeps this out of `SpeedChanger` -- a new module (or a new node in one of
// ours) installs whole, or is absent and says so on import. `audiospeed` is
// byte-identical to what it was before this node existed.
//
// This module is where it belongs for a second reason: `audioshaper/module.c`
// already carries the argument for a lo-fi primitive that is audioif's own,
// and a zero-order hold is the other half of what a bitcrusher is. The
// waveshaper quantises the value; this quantises the time.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "audiocore/__init__.h"
#include "py/obj.h"
#include "shared/audioif_samplehold.h"

typedef struct {
    audiosample_base_t base;
    mp_obj_t source;
    audioif_samplehold_config_t config;
    audioif_samplehold_state_t state;
    //: Bytes in one frame of the source's format, fixed at construction. The
    //: node copies frames and never looks inside a sample, so this is all it
    //: needs to know about the format it is carrying.
    uint8_t frame_bytes;
    //: Source frames fetched but not yet consumed, carried across output
    //: blocks, and the source's own end-of-stream flags. One frame in, one
    //: frame out: this node ends where its source ends rather than
    //: manufacturing silence, because the pair of `SpeedChanger`s it replaces
    //: did the same and a class must not have to care which it was given.
    const uint8_t *pending;
    uint32_t pending_frames;
    bool source_done;
    bool source_exhausted;
    uint8_t buffer[AUDIOIF_SAMPLEHOLD_FRAMES *
        AUDIOIF_SAMPLEHOLD_MAX_FRAME_BYTES];
} audioshaper_samplehold_obj_t;

extern const mp_obj_type_t audioshaper_samplehold_type;
