// audiograph.Graph for CircuitPython. See audiodsp's src/audiograph/Graph.h
// for the MicroPython twin; the runner is the same shared/audiodsp_graph.c
// in both, built into CircuitPython by audiodsp's circuitpython.mk.
//
// Not an audiosample: a graph renders into a buffer its caller owns, one
// block per run(), and plays through whatever that caller hands the buffer
// to.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "py/obj.h"

#include "shared/audiodsp_graph.h"

typedef struct {
    mp_obj_base_t base;
    audiodsp_graph_t graph;
    uint32_t next_buffer;
    // Objects a node borrows memory from (wow tables), kept alive here.
    mp_obj_t keep;
} audiograph_graph_obj_t;

// Allocates the graph's block buffers and node table. The arguments are
// already checked.
void common_hal_audiograph_graph_construct(audiograph_graph_obj_t *self,
    uint32_t sample_rate, uint32_t frames, uint32_t channel_count,
    uint32_t buffers, uint32_t nodes);

// Appends a node with its kernel's memory. The node's index, or -1 when the
// runner refuses it (see audiodsp_graph_add).
int common_hal_audiograph_graph_add(audiograph_graph_obj_t *self,
    audiodsp_graph_kind_t kind, uint32_t group, const int16_t *inputs,
    uint32_t ninputs, int16_t output, float arg);
