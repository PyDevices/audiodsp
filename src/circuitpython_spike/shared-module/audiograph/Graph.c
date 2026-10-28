// audiograph.Graph for CircuitPython: the memory around
// shared/audiodsp_graph.c. See Graph.h.
//
// SPDX-License-Identifier: MIT

#include "shared-module/audiograph/Graph.h"

#include "py/runtime.h"

// A table page's size: small enough to fit a fragmented heap.
#define AUDIOGRAPH_PAGE_BYTES 4096u

void common_hal_audiograph_graph_construct(audiograph_graph_obj_t *self,
    uint32_t sample_rate, uint32_t frames, uint32_t channel_count,
    uint32_t buffers, uint32_t nodes, uint32_t queue) {
    // Every table goes in pages of a few KB, so a large patch still fits a
    // fragmented heap. The GC keeps a block alive only through a pointer to
    // its start, so the first pointer into each page is that start.
    size_t per = AUDIOGRAPH_PAGE_BYTES / ((size_t)frames * sizeof(int16_t));
    per = per ? per : 1;
    int16_t **bufs = m_new(int16_t *, (size_t)buffers);
    for (uint32_t i = 0; i < buffers; i += (uint32_t)per) {
        size_t n = (size_t)(buffers - i) < per ? (size_t)(buffers - i) : per;
        int16_t *page = m_new0(int16_t, n * (size_t)frames);
        for (size_t k = 0; k < n; ++k) {
            bufs[i + k] = page + k * (size_t)frames;
        }
    }
    uint32_t node_pages = audiodsp_graph_pages(nodes, AUDIODSP_GRAPH_NODE_PAGE);
    audiodsp_graph_node_t **table = m_new(audiodsp_graph_node_t *, node_pages);
    float **shadow = m_new(float *, node_pages);
    for (uint32_t i = 0; i < node_pages; ++i) {
        table[i] = m_new0(audiodsp_graph_node_t, AUDIODSP_GRAPH_NODE_PAGE);
        shadow[i] = m_new0(float, AUDIODSP_GRAPH_SHADOW_PAGE);
    }
    uint32_t record_pages = audiodsp_graph_pages(queue, AUDIODSP_GRAPH_RECORD_PAGE);
    audiodsp_graph_record_t **records = m_new(audiodsp_graph_record_t *, record_pages);
    for (uint32_t i = 0; i < record_pages; ++i) {
        records[i] = m_new0(audiodsp_graph_record_t, AUDIODSP_GRAPH_RECORD_PAGE);
    }
    audiodsp_graph_init(&self->graph, sample_rate, frames, channel_count,
        bufs, buffers, table, nodes);
    audiodsp_graph_queue_init(&self->graph, records, queue, shadow);
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
