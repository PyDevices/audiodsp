// audiograph.Graph for CircuitPython. See audiodsp's src/audiograph/Graph.h
// for the MicroPython twin; the runner is the same shared/audiodsp_graph.c
// in both, built into CircuitPython by audiodsp's circuitpython.mk.
//
// An audiosample: I2SOut.play(graph) or a Mixer voice pulls it, one block
// per pull, from CircuitPython's background tasks on the interpreter's own
// core. set(), reset() and enable() go through the graph's queue as on every
// other build; with no second thread, a full queue drains inline.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "py/obj.h"
#include "shared-module/audiocore/__init__.h"

#include "shared/audiodsp_graph.h"

typedef struct {
    audiosample_base_t base;
    audiodsp_graph_t graph;
    uint32_t next_buffer;
    // Objects a node borrows memory from (wow tables), kept alive here.
    mp_obj_t keep;
    // Two output blocks; a pull renders into the one the last did not hand out.
    int16_t *out;
    uint8_t which;
} audiograph_graph_obj_t;

// Allocates the graph's block buffers and node table. The arguments are
// already checked.
void common_hal_audiograph_graph_construct(audiograph_graph_obj_t *self,
    uint32_t sample_rate, uint32_t frames, uint32_t channel_count,
    uint32_t buffers, uint32_t nodes, uint32_t queue);

// Appends a node with its kernel's memory. The node's index, or -1 when the
// runner refuses it (see audiodsp_graph_add).
int common_hal_audiograph_graph_add(audiograph_graph_obj_t *self,
    audiodsp_graph_kind_t kind, uint32_t group, const int16_t *inputs,
    uint32_t ninputs, int16_t output, float arg);

// The audiosample protocol: one block per pull, never DONE.
void audiograph_graph_reset_buffer(audiograph_graph_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audiograph_graph_get_buffer(
    audiograph_graph_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length);
