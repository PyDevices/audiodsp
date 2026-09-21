// Ported from CircuitPython's shared-bindings/synthio/Note.h and
// shared-module/synthio/Note.h (upstream repo:
// https://github.com/adafruit/circuitpython, MIT), merged into one file.
// `synthio_note_playing()` is declared upstream but never defined or
// called anywhere in CircuitPython itself (checked directly) -- dropped
// here rather than porting a dead declaration.
//
// SPDX-FileCopyrightText: Copyright (c) 2023 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2021 Artyom Skrobov
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

#include "synthio/Biquad.h"
#include "synthio/LFO.h"
#include "synthio/__init__.h"

#define SYNTHIO_NOTE_MAX_FILTER_STAGES (4)

typedef struct synthio_note_obj {
    mp_obj_base_t base;

    synthio_block_slot_t panning, bend, amplitude, ring_bend;

    mp_float_t frequency, ring_frequency;
    mp_obj_t waveform_obj, envelope_obj, ring_waveform_obj;
    mp_obj_t filter_obj;

    // Serial filter cascade (audiodsp extension, issue #11): Note.filter
    // accepts a Biquad or a tuple/list of up to
    // SYNTHIO_NOTE_MAX_FILTER_STAGES Biquads applied in series, each
    // stage with its own state. Stock CircuitPython accepts one Biquad;
    // a single filter behaves identically there.
    biquad_filter_state filter_state[SYNTHIO_NOTE_MAX_FILTER_STAGES];

    int32_t sample_rate;

    int32_t frequency_scaled;
    int32_t ring_frequency_scaled, ring_frequency_bent;

    mp_buffer_info_t waveform_buf;
    synthio_block_slot_t waveform_loop_start, waveform_loop_end;
    mp_buffer_info_t ring_waveform_buf;
    synthio_block_slot_t ring_waveform_loop_start, ring_waveform_loop_end;
    synthio_envelope_definition_t envelope_def;
} synthio_note_obj_t;

extern const mp_obj_type_t synthio_note_type;
// Deviation from upstream: dropped a redundant `typedef enum
// synthio_bend_mode_e synthio_bend_mode_t;` here -- upstream's own
// shared-module/synthio/Note.h repeats a typedef synthio/__init__.h
// already fully defines (same "two files never in one translation unit
// upstream" story as synthio_synth_t; see docs/upstream-diff.md). This
// file already includes `synthio/__init__.h` above, so the type is
// already complete here; the second typedef added nothing and failed
// under emscripten's strict `-std=c99 -Werror` (phase 8d).

mp_float_t common_hal_synthio_note_get_frequency(synthio_note_obj_t *self);
void common_hal_synthio_note_set_frequency(synthio_note_obj_t *self, mp_float_t value);

mp_obj_t common_hal_synthio_note_get_filter_obj(synthio_note_obj_t *self);
void common_hal_synthio_note_set_filter(synthio_note_obj_t *self, mp_obj_t biquad);

mp_obj_t common_hal_synthio_note_get_panning(synthio_note_obj_t *self);
void common_hal_synthio_note_set_panning(synthio_note_obj_t *self, mp_obj_t value);

mp_obj_t common_hal_synthio_note_get_amplitude(synthio_note_obj_t *self);
void common_hal_synthio_note_set_amplitude(synthio_note_obj_t *self, mp_obj_t value);

mp_obj_t common_hal_synthio_note_get_bend(synthio_note_obj_t *self);
void common_hal_synthio_note_set_bend(synthio_note_obj_t *self, mp_obj_t value);

mp_obj_t common_hal_synthio_note_get_waveform_obj(synthio_note_obj_t *self);
void common_hal_synthio_note_set_waveform(synthio_note_obj_t *self, mp_obj_t value);

mp_obj_t common_hal_synthio_note_get_waveform_loop_start(synthio_note_obj_t *self);
void common_hal_synthio_note_set_waveform_loop_start(synthio_note_obj_t *self, mp_obj_t value);

mp_obj_t common_hal_synthio_note_get_waveform_loop_end(synthio_note_obj_t *self);
void common_hal_synthio_note_set_waveform_loop_end(synthio_note_obj_t *self, mp_obj_t value);

mp_float_t common_hal_synthio_note_get_ring_frequency(synthio_note_obj_t *self);
void common_hal_synthio_note_set_ring_frequency(synthio_note_obj_t *self, mp_float_t value);

mp_obj_t common_hal_synthio_note_get_ring_bend(synthio_note_obj_t *self);
void common_hal_synthio_note_set_ring_bend(synthio_note_obj_t *self, mp_obj_t value);

mp_obj_t common_hal_synthio_note_get_ring_waveform_obj(synthio_note_obj_t *self);
void common_hal_synthio_note_set_ring_waveform(synthio_note_obj_t *self, mp_obj_t value);

mp_obj_t common_hal_synthio_note_get_ring_waveform_loop_start(synthio_note_obj_t *self);
void common_hal_synthio_note_set_ring_waveform_loop_start(synthio_note_obj_t *self, mp_obj_t value);

mp_obj_t common_hal_synthio_note_get_ring_waveform_loop_end(synthio_note_obj_t *self);
void common_hal_synthio_note_set_ring_waveform_loop_end(synthio_note_obj_t *self, mp_obj_t value);

mp_obj_t common_hal_synthio_note_get_envelope_obj(synthio_note_obj_t *self);
void common_hal_synthio_note_set_envelope(synthio_note_obj_t *self, mp_obj_t value);

void synthio_note_recalculate(synthio_note_obj_t *self, int32_t sample_rate);
uint32_t synthio_note_step(synthio_note_obj_t *self, int32_t sample_rate, int16_t dur, int16_t loudness[2]);
void synthio_note_start(synthio_note_obj_t *self, int32_t sample_rate);
