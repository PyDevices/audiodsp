// audiobiquad.AllPass -- MicroPython bindings over the runtime-neutral
// first-order all-pass cascade in shared/audiodsp_filter_f32.c.
//
// Not a CircuitPython port and not from vstaudio. `audiofilters.Phaser` is
// the same topology in int16 with a feedback clamped to 0.1..0.9
// (audiofilters/Phaser.c:211), so neither the exact-zero tail nor the
// feedback-free null a script phaser makes is reachable on it. See
// shared/audiodsp_filter_f32.h and docs/upstream-diff.md.
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
    mp_obj_t source;
    synthio_block_slot_t frequency;
    synthio_block_slot_t feedback;
    synthio_block_slot_t mix;
    audiodsp_allpass_f32_config_t config;
    audiodsp_allpass_f32_state_t state;
    int16_t buffer[AUDIODSP_FILTER_F32_FRAMES * 2];
    const int16_t *pending;
    uint32_t pending_frames;
} audiobiquad_allpass_obj_t;

extern const mp_obj_type_t audiobiquad_allpass_type;
