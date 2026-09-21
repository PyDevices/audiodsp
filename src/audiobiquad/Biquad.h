// audiobiquad.Biquad -- MicroPython bindings over the runtime-neutral float
// biquad in shared/audiodsp_filter_f32.c.
//
// Not a CircuitPython port and not from vstaudio: audiodsp adds this one.
// `audiofilters.Filter` over `synthio.Biquad` already filters, and its
// recursion is integer with fixed points it can park on -- see
// shared/audiodsp_filter_f32.h for the measurement, and docs/upstream-diff.md
// for where this module sits among the rest. A new module rather than an
// argument on `audiofilters`, because an argument added to this port's copy
// of a CircuitPython module would not exist on a stock board.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "audiocore/__init__.h"
#include "py/obj.h"
#include "shared/audiodsp_filter_f32.h"
#include "synthio/__init__.h"
#include "synthio/block.h"

typedef struct {
    audiosample_base_t base;
    // The signal. When it runs dry the node hands out silence, the way every
    // other effect in the palette does.
    mp_obj_t source;
    // Read once per processed chunk, exactly where audiofilters/Filter.c
    // reads its own -- so an LFO on `frequency` modulates at the same rate
    // it would on the ported node, and the CPython twin reads them at the
    // same point with the same values.
    synthio_block_slot_t frequency;
    synthio_block_slot_t Q;
    synthio_block_slot_t gain_db;
    synthio_block_slot_t mix;
    audiodsp_biquad_f32_config_t config;
    audiodsp_biquad_f32_state_t state;
    int16_t buffer[AUDIODSP_FILTER_F32_FRAMES * 2];
    // Source frames fetched but not yet consumed, carried across output
    // blocks.
    const int16_t *pending;
    uint32_t pending_frames;
} audiobiquad_biquad_obj_t;

extern const mp_obj_type_t audiobiquad_biquad_type;
