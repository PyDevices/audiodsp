// Ported from CircuitPython's shared-bindings/audiomixer/MixerVoice.h and
// shared-module/audiomixer/MixerVoice.h (upstream repo:
// https://github.com/adafruit/circuitpython, MIT), merged into one file
// (this port doesn't keep CP's shared-bindings/shared-module split).
//
// Deviation from upstream: CP guards level/panning as synthio_block_slot_t
// behind `#if CIRCUITPY_SYNTHIO` (some CP boards omit synthio). This port
// always has synthio (tier 2), so the block-input path is unconditional and
// the plain-float fallback fields/branches are dropped.
//
// SPDX-FileCopyrightText: Copyright (c) 2018 DeanM for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

#include "audiomixer/Mixer.h"
#include "synthio/block.h"

// Not upstream: a level move walking in a straight line (Mixer.level_ramp).
// `lo` and `hi` are the level now, Q15 shifted up 15 more bits so a step
// smaller than one Q15 unit still adds up; `left` counts the steps to go, one
// per packed word the mix-down scales.
typedef struct {
    int32_t lo;
    int32_t hi;
    int32_t step_lo;
    int32_t step_hi;
    int32_t target_lo;
    int32_t target_hi;
    uint32_t left;
} audiomixer_level_ramp_t;

typedef struct {
    mp_obj_base_t base;
    audiomixer_mixer_obj_t *parent;
    mp_obj_t sample;
    bool loop;
    bool more_data;
    uint32_t *remaining_buffer;
    uint32_t buffer_length;
    synthio_block_slot_t level;
    synthio_block_slot_t panning;
    // The level this voice last ACTUALLY rendered at, per channel. A change to
    // level or panning waits for a zero crossing (CircuitPython 10.3.0), so it
    // cannot step the waveform mid-cycle and click.
    int32_t active_lo_level;
    int32_t active_hi_level;
    // Not upstream: the walk a level move takes when the Mixer's level_ramp
    // is set. `ramp_primed` is false until the voice has rendered once with
    // the ramp on, so its first level lands as CircuitPython's does.
    audiomixer_level_ramp_t ramp;
    bool ramp_primed;
} audiomixer_mixervoice_obj_t;

extern const mp_obj_type_t audiomixer_mixervoice_type;

void common_hal_audiomixer_mixervoice_construct(audiomixer_mixervoice_obj_t *self);
void common_hal_audiomixer_mixervoice_set_parent(audiomixer_mixervoice_obj_t *self, audiomixer_mixer_obj_t *parent);
void common_hal_audiomixer_mixervoice_play(audiomixer_mixervoice_obj_t *self, mp_obj_t sample, bool loop);
void common_hal_audiomixer_mixervoice_stop(audiomixer_mixervoice_obj_t *self);
void common_hal_audiomixer_mixervoice_reset(audiomixer_mixervoice_obj_t *self);
void common_hal_audiomixer_mixervoice_end(audiomixer_mixervoice_obj_t *self);
mp_obj_t common_hal_audiomixer_mixervoice_get_level(audiomixer_mixervoice_obj_t *self);
void common_hal_audiomixer_mixervoice_set_level(audiomixer_mixervoice_obj_t *self, mp_obj_t gain);
mp_obj_t common_hal_audiomixer_mixervoice_get_panning(audiomixer_mixervoice_obj_t *self);
void common_hal_audiomixer_mixervoice_set_panning(audiomixer_mixervoice_obj_t *self, mp_obj_t value);

bool common_hal_audiomixer_mixervoice_get_playing(audiomixer_mixervoice_obj_t *self);

void common_hal_audiomixer_mixervoice_set_loop(audiomixer_mixervoice_obj_t *self, bool loop);
bool common_hal_audiomixer_mixervoice_get_loop(audiomixer_mixervoice_obj_t *self);
