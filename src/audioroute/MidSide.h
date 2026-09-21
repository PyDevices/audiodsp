// audioroute.MidSide -- MicroPython bindings over the runtime-neutral mid/side
// matrix in shared/audiodsp_midside.c.
//
// In `audioroute` rather than a module of its own because it is routing, not
// arithmetic on streams: it changes which signal reaches which channel, the
// same job `Splitter` does across branches. See shared/audiodsp_midside.h for
// why the palette could not already do it, and docs/upstream-diff.md for
// where it sits among the rest.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "audiocore/__init__.h"
#include "py/obj.h"
#include "shared/audiodsp_midside.h"

typedef struct {
    audiosample_base_t base;
    // The only input. When it runs dry the node hands out silence, the way
    // every other effect in the palette does.
    mp_obj_t source;
    audiodsp_midside_config_t config;
    int16_t buffer[AUDIODSP_MIDSIDE_FRAMES * 2];
    // Source frames fetched but not yet consumed, carried across output
    // blocks.
    const int16_t *pending;
    uint32_t pending_frames;
} audioroute_midside_obj_t;

extern const mp_obj_type_t audioroute_midside_type;
