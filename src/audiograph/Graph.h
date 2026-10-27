// audiograph.Graph -- MicroPython binding over shared/audiodsp_graph.c.
//
// The runner that renders a whole patch in one call. The CircuitPython twin
// is src/circuitpython_spike/shared-bindings/audiograph/, the CPython one the
// Graph type in src/cpython/_audiodsp.c; the three take the same arguments.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "audiocore/__init__.h"
#include "py/obj.h"
#include "shared/audiodsp_graph.h"

// A graph is an audiosample: an output, a Mixer voice or audiopump.spawn()
// pulls it, and each pull renders one block into one of the two output
// blocks the graph owns. set(), reset() and enable() reach the renderer
// through the graph's queue (shared/audiodsp_graph.h), whichever thread
// renders.
typedef struct {
    audiosample_base_t base;
    audiodsp_graph_t graph;
    uint32_t next_buffer;
    // Objects a node borrows memory from (wow tables), kept alive here.
    mp_obj_t keep;
    // Two output blocks of frames * channel_count samples; a pull renders
    // into the one the last pull did not hand out.
    int16_t *out;
    uint8_t which;
    // A pull has come from the pump's own thread. While that pump runs,
    // the calls that would render or rewire beside it refuse.
    volatile bool pumped;
} audiograph_graph_obj_t;

extern const mp_obj_type_t audiograph_graph_type;
