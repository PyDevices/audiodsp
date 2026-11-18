// audioroute.SplitterTap for CircuitPython: one branch's view of a Splitter's
// ring. Taps are built by the Splitter and handed out by `tap(index)`.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/audiocore/__init__.h"
#include "shared-module/audioroute/Splitter.h"

typedef struct {
    audiosample_base_t base;
    // A real object reference, not the raw C pointer the original kept: a tap
    // handed to a Mixer routinely outlives every other name for its Splitter,
    // and the collector has to be able to see that the ring is still in use.
    mp_obj_t owner;
    uint32_t index;
    int16_t mono[AUDIODSP_SPLITTER_CHUNK_FRAMES];
} audioroute_splitter_tap_obj_t;

void audioroute_splitter_tap_reset_buffer(audioroute_splitter_tap_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audioroute_splitter_tap_get_buffer(
    audioroute_splitter_tap_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length);
