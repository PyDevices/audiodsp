// audioroute.SplitterTap for CircuitPython. See SplitterTap.h.
//
// SPDX-License-Identifier: MIT

#include "shared-module/audioroute/SplitterTap.h"

#include <string.h>

void audioroute_splitter_tap_reset_buffer(audioroute_splitter_tap_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    // Deliberately nothing. The cursors belong to the Splitter and the other
    // taps are still reading from them; rewinding one branch mid-stream would
    // desynchronise the rest.
    (void)self;
    (void)single_channel_output;
    (void)channel;
}

audioio_get_buffer_result_t audioroute_splitter_tap_get_buffer(
    audioroute_splitter_tap_obj_t *tap, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    audioroute_splitter_obj_t *self = MP_OBJ_TO_PTR(tap->owner);
    if (audiodsp_splitter_starved(&self->state, tap->index)) {
        audioroute_splitter_pull(self);
    }
    uint32_t start = 0;
    const uint32_t run = audiodsp_splitter_take(&self->state, tap->index,
        &start);
    if (run == 0) {
        // Still nothing: the source is dry, or another tap has already read
        // ahead of what one pull could supply. Hand out silence and let the
        // branch stay in step rather than stalling the graph.
        memset(self->silence, 0, sizeof(self->silence));
        *buffer = (uint8_t *)self->silence;
        *buffer_length = AUDIODSP_SPLITTER_CHUNK_FRAMES * 2u *
            tap->base.channel_count;
        return GET_BUFFER_MORE_DATA;
    }
    if (tap->base.channel_count == 2u) {
        *buffer = (uint8_t *)&self->state.ring[start * 2u];
        *buffer_length = run * 4u;
        return GET_BUFFER_MORE_DATA;
    }
    for (uint32_t frame = 0; frame < run; ++frame) {
        tap->mono[frame] = self->state.ring[
            ((start + frame) % AUDIODSP_SPLITTER_RING_FRAMES) * 2u];
    }
    *buffer = (uint8_t *)tap->mono;
    *buffer_length = run * 2u;
    return GET_BUFFER_MORE_DATA;
}
