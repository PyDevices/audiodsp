// audioecho.FeedbackDelay for CircuitPython: the buffer plumbing around
// shared/audiodsp_feedback_delay.c. See FeedbackDelay.h.
//
// SPDX-License-Identifier: MIT

#include "shared-module/audioecho/FeedbackDelay.h"

#include <string.h>

void audioecho_feedback_delay_reset_buffer(
    audioecho_feedback_delay_obj_t *self, bool single_channel_output,
    uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    self->pending = NULL;
    self->pending_frames = 0;
    // Unlike audiodynamics, everything goes. A delay's whole state is
    // audible: a chain restarted with the old repeats still in the line
    // plays the previous take over the new one.
    audiodsp_feedback_delay_reset(&self->state, &self->config);
}

audioio_get_buffer_result_t audioecho_feedback_delay_get_buffer(
    audioecho_feedback_delay_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    uint32_t produced = 0;
    while (produced < AUDIODSP_FEEDBACK_DELAY_FRAMES) {
        if (self->pending_frames == 0) {
            if (self->source == MP_OBJ_NULL) {
                break;
            }
            uint8_t *raw = NULL;
            uint32_t raw_bytes = 0;
            audioio_get_buffer_result_t result = audiosample_get_buffer(
                self->source, false, 0, &raw, &raw_bytes);
            const uint32_t width = 2u * self->base.channel_count;
            if (result == GET_BUFFER_ERROR || raw == NULL || raw_bytes < width) {
                break;
            }
            self->pending = (const int16_t *)raw;
            self->pending_frames = raw_bytes / width;
        }
        uint32_t run = AUDIODSP_FEEDBACK_DELAY_FRAMES - produced;
        if (run > self->pending_frames) {
            run = self->pending_frames;
        }
        audiodsp_feedback_delay_process_s16(&self->config, &self->state,
            &self->buffer[produced * self->base.channel_count],
            self->pending, run);
        self->pending += run * self->base.channel_count;
        self->pending_frames -= run;
        produced += run;
    }
    // A starved chain gets silence rather than a short block: this node sits
    // in the middle of a live graph and never reports itself finished. Note
    // that the tail stops with the source -- the line is only advanced by
    // frames that arrive, so repeats do not ring on into silence. That
    // matches audiodelays.Echo, whose users expect it.
    if (produced == 0) {
        memset(self->buffer, 0, sizeof(self->buffer));
        produced = AUDIODSP_FEEDBACK_DELAY_FRAMES;
    }
    *buffer = (uint8_t *)self->buffer;
    *buffer_length = produced * 2u * self->base.channel_count;
    return GET_BUFFER_MORE_DATA;
}
