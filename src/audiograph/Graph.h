// audiograph.Graph -- MicroPython binding over shared/audiodsp_graph.c.
//
// The runner that renders a whole patch in one call. The CircuitPython twin
// is src/circuitpython_spike/shared-bindings/audiograph/, the CPython one the
// Graph type in src/cpython/_audiodsp.c; the three take the same arguments.
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

extern const mp_obj_type_t audiograph_graph_type;
