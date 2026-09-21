// audioroute.SplitterTap -- one branch's view of a Splitter's ring. Taps are
// built by the Splitter and handed out by `tap(index)`; there is no
// constructor.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "audiocore/__init__.h"
#include "audioroute/Splitter.h"
#include "py/obj.h"

typedef struct {
    audiosample_base_t base;
    // A real object reference, not the raw C pointer the original kept: a tap
    // handed to a Mixer routinely outlives every other name for its Splitter,
    // and the collector has to be able to see that the ring is still in use.
    mp_obj_t owner;
    uint32_t index;
    int16_t mono[AUDIODSP_SPLITTER_CHUNK_FRAMES];
} audioroute_splitter_tap_obj_t;

extern const mp_obj_type_t audioroute_splitter_tap_type;
