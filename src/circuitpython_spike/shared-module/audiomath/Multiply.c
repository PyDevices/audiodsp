// audiomath.Multiply for CircuitPython: the buffer plumbing around
// shared/audiodsp_multiply.c. See Multiply.h.
//
// SPDX-License-Identifier: MIT

#include "shared-module/audiomath/Multiply.h"

#include <string.h>

void audiomath_multiply_reset_buffer(audiomath_multiply_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    self->pending_source = NULL;
    self->pending_source_frames = 0;
    // The modulator's cursor goes too, so a chain restarted mid-cycle begins
    // at the top of the table rather than wherever it happened to stop.
    self->pending_modulator = NULL;
    self->pending_modulator_frames = 0;
}

audioio_get_buffer_result_t audiomath_multiply_get_buffer(
    audiomath_multiply_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    uint32_t produced = 0;
    while (produced < AUDIODSP_MULTIPLY_FRAMES) {
        if (self->pending_source_frames == 0) {
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
            self->pending_source = (const int16_t *)raw;
            self->pending_source_frames = raw_bytes / width;
        }
        if (self->pending_modulator_frames == 0 &&
            self->modulator != MP_OBJ_NULL) {
            uint8_t *raw = NULL;
            uint32_t raw_bytes = 0;
            audioio_get_buffer_result_t result = audiosample_get_buffer(
                self->modulator, false, 0, &raw, &raw_bytes);
            // A modulator is normally a short looping table, which hands back
            // its whole length every time it is asked; one that has genuinely
            // stopped leaves the signal alone rather than muting it.
            const uint32_t width = 2u * self->base.channel_count;
            if (result != GET_BUFFER_ERROR && raw != NULL && raw_bytes >= width) {
                self->pending_modulator = (const int16_t *)raw;
                self->pending_modulator_frames = raw_bytes / width;
            }
        }
        uint32_t run = AUDIODSP_MULTIPLY_FRAMES - produced;
        if (run > self->pending_source_frames) {
            run = self->pending_source_frames;
        }
        if (self->pending_modulator_frames > 0) {
            if (run > self->pending_modulator_frames) {
                run = self->pending_modulator_frames;
            }
            audiodsp_multiply_process_s16(&self->config,
                &self->buffer[produced * self->base.channel_count],
                self->pending_source,
                self->pending_modulator, run);
            self->pending_modulator += run * self->base.channel_count;
            self->pending_modulator_frames -= run;
        } else {
            memcpy(&self->buffer[produced * self->base.channel_count],
                self->pending_source,
                (size_t)run * self->base.channel_count * sizeof(int16_t));
        }
        self->pending_source += run * self->base.channel_count;
        self->pending_source_frames -= run;
        produced += run;
    }
    // A starved chain gets silence rather than a short block: this node sits
    // in the middle of a live graph and never reports itself finished.
    if (produced == 0) {
        memset(self->buffer, 0, sizeof(self->buffer));
        produced = AUDIODSP_MULTIPLY_FRAMES;
    }
    *buffer = (uint8_t *)self->buffer;
    *buffer_length = produced * 2u * self->base.channel_count;
    return GET_BUFFER_MORE_DATA;
}
