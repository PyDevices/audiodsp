// audiograph.Graph for CircuitPython: the memory around
// shared/audiodsp_graph.c. See Graph.h.
//
// SPDX-License-Identifier: MIT

#include "shared-module/audiograph/Graph.h"

#include "py/runtime.h"

void common_hal_audiograph_graph_construct(audiograph_graph_obj_t *self,
    uint32_t sample_rate, uint32_t frames, uint32_t channel_count,
    uint32_t buffers, uint32_t nodes) {
    int16_t *bufs = m_new0(int16_t, (size_t)buffers * (size_t)frames);
    audiodsp_graph_node_t *table = m_new0(audiodsp_graph_node_t, (size_t)nodes);
    audiodsp_graph_init(&self->graph, sample_rate, frames, channel_count,
        bufs, buffers, table, nodes);
    self->next_buffer = 0;
    self->keep = mp_obj_new_list(0, NULL);
}

int common_hal_audiograph_graph_add(audiograph_graph_obj_t *self,
    audiodsp_graph_kind_t kind, uint32_t group, const int16_t *inputs,
    uint32_t ninputs, int16_t output, float arg) {
    size_t size = audiodsp_graph_node_data_size(kind, self->graph.sample_rate,
        arg);
    // On the heap the graph's table points into, so the collector keeps it
    // for as long as the graph.
    void *data = size ? m_new0(uint8_t, size) : NULL;
    int index = audiodsp_graph_add(&self->graph, kind, group, inputs, ninputs,
        output, data, arg);
    if (index < 0 && data != NULL) {
        m_del(uint8_t, data, size);
    }
    return index;
}
