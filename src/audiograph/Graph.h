// audiograph.Graph -- MicroPython binding over shared/audiodsp_graph.c.
//
// The runner that renders a whole patch in one call. MicroPython only for
// now; the CPython and CircuitPython bindings are on the roadmap.
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
