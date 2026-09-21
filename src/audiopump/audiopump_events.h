// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"

extern const mp_obj_type_t audiopump_events_type;

// The closed set. Everything here is a few stores at apply time; everything
// that could refuse, allocate or raise happened when the event was scheduled.
enum {
    AUDIOPUMP_OP_PRESS = 1,       // Synthesizer, a Note or a MIDI number
    AUDIOPUMP_OP_RELEASE = 2,     // the same
    AUDIOPUMP_OP_RELEASE_ALL = 3, // Synthesizer
    AUDIOPUMP_OP_PLAY = 4,        // MixerVoice, a sample that is already built
    AUDIOPUMP_OP_STOP = 5,        // MixerVoice
    AUDIOPUMP_OP_LEVEL = 6,       // MixerVoice, a level boxed at schedule time
};

// Called by the pump at the top of a block, INSIDE the pump's own lock.
//
// `now` is the frame the block about to be pulled starts at and `block_frames`
// is how long that block is, so every event whose frame falls inside it is
// applied, in schedule order. An event already behind `now` is applied too,
// and counted late.
//
// Allocation-free and raise-free by construction rather than by argument: no
// call below this one can do either. Returns how many it applied.
uint32_t audiopump_events_apply(mp_obj_t queue, uint32_t now,
    uint32_t block_frames);
