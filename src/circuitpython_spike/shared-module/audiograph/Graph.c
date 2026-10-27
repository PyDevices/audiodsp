// audiograph.Graph for CircuitPython: the memory around
// shared/audiodsp_graph.c. See Graph.h.
//
// SPDX-License-Identifier: MIT

#include "shared-module/audiograph/Graph.h"

#include "py/runtime.h"

void common_hal_audiograph_graph_construct(audiograph_graph_obj_t *self,
    uint32_t sample_rate, uint32_t frames, uint32_t channel_count,
    uint32_t buffers, uint32_t nodes, uint32_t queue) {
    int16_t *bufs = m_new0(int16_t, (size_t)buffers * (size_t)frames);
    audiodsp_graph_node_t *table = m_new0(audiodsp_graph_node_t, (size_t)nodes);
    audiodsp_graph_init(&self->graph, sample_rate, frames, channel_count,
        bufs, buffers, table, nodes);
    audiodsp_graph_queue_init(&self->graph,
        m_new0(audiodsp_graph_record_t, (size_t)queue), queue,
        m_new0(float, (size_t)nodes * AUDIODSP_GRAPH_MAX_PARAMS));
    self->out = m_new0(int16_t, 2u * (size_t)frames * channel_count);
    self->which = 0;
    self->base.sample_rate = sample_rate;
    self->base.max_buffer_length = frames * channel_count * 2u;
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)channel_count;
    self->base.samples_signed = true;
    self->base.single_buffer = false;
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

void audiograph_graph_reset_buffer(audiograph_graph_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    (void)self;
    (void)single_channel_output;
    (void)channel;
}

audioio_get_buffer_result_t audiograph_graph_get_buffer(
    audiograph_graph_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    const uint32_t samples = self->graph.frames * self->graph.channel_count;
    if (!single_channel_output || channel == 0) {
        self->which ^= 1u;
        audiodsp_graph_render(&self->graph, self->out + self->which * samples);
    }
    int16_t *block = self->out + self->which * samples;
    if (single_channel_output && channel < self->graph.channel_count) {
        block += channel;
    }
    *buffer = (uint8_t *)block;
    *buffer_length = samples * 2u;
    return GET_BUFFER_MORE_DATA;
}
