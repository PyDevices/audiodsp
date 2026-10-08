// audioverb.Tank for CircuitPython: the buffer plumbing around
// shared/audiodsp_tank.c. See Tank.h.
//
// SPDX-License-Identifier: MIT

#include "shared-module/audioverb/Tank.h"

#include <string.h>

void audioverb_tank_reset_buffer(audioverb_tank_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    // A host reset clears what this node holds of its own and keeps the
    // source frames it has already taken, the rule CircuitPython's effects
    // follow and the one clear() always had: the source was not reset, so
    // dropping them would skip that much of it.
    // Unlike audiodynamics, all of its own state goes. A reverberation tail is
    // entirely state: a chain restarted with the old tail still in the lines
    // plays the previous take underneath the new one.
    audiodsp_tank_reset(&self->state, &self->config);
}

audioio_get_buffer_result_t audioverb_tank_get_buffer(
    audioverb_tank_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    uint32_t produced = 0;
    while (produced < AUDIODSP_TANK_FRAMES) {
        if (self->pending_frames == 0) {
            // The buffer that came with GET_BUFFER_DONE was the source's
            // last. Let go of it, as audiodelays' effects do, and ring the
            // tail out on silence below (audiodsp#180).
            if (self->source_done) {
                self->source = MP_OBJ_NULL;
                self->source_done = false;
            }
            if (self->source == MP_OBJ_NULL) {
                break;
            }
            uint8_t *raw = NULL;
            uint32_t raw_bytes = 0;
            audioio_get_buffer_result_t result = audiosample_get_buffer(
                self->source, false, 0, &raw, &raw_bytes);
            const uint32_t width = 2u * self->base.channel_count;
            if (result == GET_BUFFER_ERROR || raw == NULL || raw_bytes < width) {
                // A source that ends with nothing in hand is done with too.
                // One that has nothing this time is asked again next block.
                if (result == GET_BUFFER_DONE) {
                    self->source = MP_OBJ_NULL;
                }
                break;
            }
            self->pending = (const int16_t *)(const void *)raw;
            self->pending_frames = raw_bytes / width;
            self->source_done = (result == GET_BUFFER_DONE);
        }
        uint32_t run = AUDIODSP_TANK_FRAMES - produced;
        if (run > self->pending_frames) {
            run = self->pending_frames;
        }
        audiodsp_tank_process_s16(&self->config, &self->state,
            &self->buffer[produced * self->base.channel_count], self->pending,
            run);
        self->pending += run * self->base.channel_count;
        self->pending_frames -= run;
        produced += run;
    }
    // Whatever the source did not fill is rendered from silence, so the block
    // is always full and the tail rings out after the source ends instead of
    // freezing until it comes back (audiodsp#180). Once it has ended, the node
    // rests and silence costs nothing. A node in the middle of a live graph
    // never reports itself finished.
    if (produced < AUDIODSP_TANK_FRAMES) {
        audiodsp_tank_process_silence(&self->config, &self->state,
            &self->buffer[produced * self->base.channel_count],
            AUDIODSP_TANK_FRAMES - produced);
        produced = AUDIODSP_TANK_FRAMES;
    }
    *buffer = (uint8_t *)self->buffer;
    *buffer_length = produced * 2u * self->base.channel_count;
    return GET_BUFFER_MORE_DATA;
}
