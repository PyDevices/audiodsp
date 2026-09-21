// audiomath.SubOctave for CircuitPython: the buffer plumbing around
// shared/audiodsp_suboctave.c. See SubOctave.h.
//
// SPDX-License-Identifier: MIT

#include "shared-module/audiomath/SubOctave.h"

#include <string.h>

void audiomath_suboctave_reset_buffer(audiomath_suboctave_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    self->pending = NULL;
    self->pending_frames = 0;
    // The divider holds no audio, so unlike a delay this drops nothing
    // anybody can hear; what it stops is a restarted chain beginning on the
    // inverted half of the count.
    audiodsp_suboctave_reset(&self->state);
}

audioio_get_buffer_result_t audiomath_suboctave_get_buffer(
    audiomath_suboctave_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    const uint32_t width = 2u * self->base.channel_count;
    uint32_t produced = 0;
    while (produced < AUDIODSP_SUBOCTAVE_FRAMES) {
        if (self->pending_frames == 0) {
            if (self->source == MP_OBJ_NULL) {
                break;
            }
            uint8_t *raw = NULL;
            uint32_t raw_bytes = 0;
            audioio_get_buffer_result_t result = audiosample_get_buffer(
                self->source, false, 0, &raw, &raw_bytes);
            if (result == GET_BUFFER_ERROR || raw == NULL || raw_bytes < width) {
                break;
            }
            self->pending = (const int16_t *)raw;
            self->pending_frames = raw_bytes / width;
        }
        uint32_t run = AUDIODSP_SUBOCTAVE_FRAMES - produced;
        if (run > self->pending_frames) {
            run = self->pending_frames;
        }
        audiodsp_suboctave_process_s16(&self->config, &self->state,
            &self->buffer[produced * self->base.channel_count],
            self->pending, run);
        self->pending += run * self->base.channel_count;
        self->pending_frames -= run;
        produced += run;
    }
    // A starved chain gets silence rather than a short block: this node sits
    // in the middle of a live graph and never reports itself finished.
    if (produced == 0) {
        memset(self->buffer, 0, sizeof(self->buffer));
        produced = AUDIODSP_SUBOCTAVE_FRAMES;
    }
    *buffer = (uint8_t *)self->buffer;
    *buffer_length = produced * width;
    return GET_BUFFER_MORE_DATA;
}
