// audioshaper.SampleHold for CircuitPython: the buffer plumbing around
// shared/audioif_samplehold.c. See SampleHold.h.
//
// SPDX-License-Identifier: MIT

#include "shared-module/audioshaper/SampleHold.h"

#include <string.h>

void audioshaper_samplehold_reset_buffer(audioshaper_samplehold_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    if (self->source != MP_OBJ_NULL && self->source != mp_const_none) {
        audiosample_reset_buffer(self->source, false, 0);
    }
    self->pending = NULL;
    self->pending_frames = 0;
    self->source_done = false;
    self->source_exhausted = false;
    audioif_samplehold_reset(&self->state, &self->config);
}

audioio_get_buffer_result_t audioshaper_samplehold_get_buffer(
    audioshaper_samplehold_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    const uint32_t width = self->frame_bytes;
    uint32_t produced = 0;
    while (produced < AUDIOIF_SAMPLEHOLD_FRAMES) {
        if (self->pending_frames == 0) {
            if (self->source == MP_OBJ_NULL || self->source_exhausted ||
                self->source_done) {
                self->source_exhausted = true;
                break;
            }
            uint8_t *raw = NULL;
            uint32_t raw_bytes = 0;
            audioio_get_buffer_result_t result = audiosample_get_buffer(
                self->source, false, 0, &raw, &raw_bytes);
            if (result == GET_BUFFER_ERROR || raw == NULL ||
                raw_bytes < width) {
                self->source_exhausted = true;
                break;
            }
            self->pending = raw;
            self->pending_frames = raw_bytes / width;
            self->source_done = (result == GET_BUFFER_DONE);
        }
        uint32_t run = AUDIOIF_SAMPLEHOLD_FRAMES - produced;
        if (run > self->pending_frames) {
            run = self->pending_frames;
        }
        audioif_samplehold_process(&self->config, &self->state,
            &self->buffer[produced * width], self->pending, run, width);
        self->pending += run * width;
        self->pending_frames -= run;
        produced += run;
    }
    // One frame in, one frame out, and the source's own ending. A node that
    // manufactured silence here would move where a chain ends, and the pair of
    // `SpeedChanger`s this replaces did not.
    *buffer = self->buffer;
    *buffer_length = produced * width;
    if (produced == 0 || self->source_exhausted) {
        return GET_BUFFER_DONE;
    }
    return GET_BUFFER_MORE_DATA;
}
