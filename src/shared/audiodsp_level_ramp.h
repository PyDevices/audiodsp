// A gain that walks to a new value instead of stepping to it.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// A node that scales its signal by a control -- a mix, a post gain, a makeup
// gain -- and takes a new value at the next block steps its output there,
// and a step is a click. With one of these per gain the node walks to the new
// value in a straight line over `frames` frames instead, one step a frame,
// and the last step lands exactly on the value asked for.
//
// A node's first value is not walked: it is what the node was built at, and
// a fresh node renders its first block at it. A reset clears the ramp, so the
// first block after one lands the same way. A move during a walk starts a
// new walk from where the gain stands. `frames` 0 steps at once, which is
// the node before the ramp.
//
// The arithmetic is float and the same on every target: one subtraction and
// one division when a move arrives, one addition a frame while it runs.

#pragma once

#include <stdbool.h>
#include <stdint.h>

//: How long a level move takes when the node is not told otherwise, in
//: milliseconds: long enough not to click, short enough to sound like the
//: knob that moved.
#define AUDIODSP_LEVEL_RAMP_MS 20.0f

typedef struct {
    float current;
    float target;
    float step;
    uint32_t left;
    bool primed;
} audiodsp_level_ramp_t;

//: Frames in a `ramp_ms` walk at `sample_rate`, rounded to the nearest.
static inline uint32_t audiodsp_level_ramp_frames(uint32_t sample_rate,
    float ramp_ms) {
    if (!(ramp_ms > 0.0f)) {
        return 0u;
    }
    const float frames = (float)sample_rate * ramp_ms / 1000.0f + 0.5f;
    return frames < 1.0f ? 1u : (uint32_t)frames;
}

//: Forget the walk: the next `aim` lands at once, as a fresh node's does.
static inline void audiodsp_level_ramp_clear(audiodsp_level_ramp_t *ramp) {
    ramp->current = 0.0f;
    ramp->target = 0.0f;
    ramp->step = 0.0f;
    ramp->left = 0u;
    ramp->primed = false;
}

//: Hand the ramp the value the node is asked for now, once a block before
//: the block's frames are rendered.
static inline void audiodsp_level_ramp_aim(audiodsp_level_ramp_t *ramp,
    float target, uint32_t frames) {
    if (!ramp->primed || frames == 0u) {
        ramp->current = target;
        ramp->target = target;
        ramp->left = 0u;
        ramp->primed = true;
        return;
    }
    if (target == ramp->target) {
        return;
    }
    ramp->target = target;
    ramp->step = (target - ramp->current) / (float)frames;
    ramp->left = frames;
}

//: Whether a walk is under way: when it is not, `current` is the target and
//: a node may take its unramped path.
static inline bool audiodsp_level_ramp_moving(
    const audiodsp_level_ramp_t *ramp) {
    return ramp->left != 0u;
}

//: The gain for the next frame.
static inline float audiodsp_level_ramp_next(audiodsp_level_ramp_t *ramp) {
    if (ramp->left != 0u) {
        if (--ramp->left == 0u) {
            ramp->current = ramp->target;
        } else {
            ramp->current += ramp->step;
        }
    }
    return ramp->current;
}
