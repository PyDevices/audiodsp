// audiodynamics.Dynamics for CircuitPython: the buffer plumbing around
// shared/audiodsp_dynamics.c. See Dynamics.h.
//
// SPDX-License-Identifier: MIT

#include "shared-module/audiodynamics/Dynamics.h"

#include <string.h>

void audiodynamics_dynamics_reset_buffer(audiodynamics_dynamics_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    self->pending = NULL;
    self->pending_frames = 0;
    self->key_pending = NULL;
    self->key_pending_frames = 0;
    audiodsp_dynamics_reset(&self->state);
}

audioio_get_buffer_result_t audiodynamics_dynamics_get_buffer(
    audiodynamics_dynamics_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    uint32_t produced = 0;
    while (produced < AUDIODSP_DYNAMICS_FRAMES) {
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
        uint32_t run = AUDIODSP_DYNAMICS_FRAMES - produced;
        if (run > self->pending_frames) {
            run = self->pending_frames;
        }
        const int16_t *key = NULL;
        if (self->key_source != MP_OBJ_NULL) {
            if (self->key_pending_frames == 0) {
                uint8_t *raw = NULL;
                uint32_t raw_bytes = 0;
                audioio_get_buffer_result_t result = audiosample_get_buffer(
                    self->key_source, false, 0, &raw, &raw_bytes);
                const uint32_t width = 2u * self->base.channel_count;
                if (result == GET_BUFFER_ERROR || raw == NULL ||
                    raw_bytes < width) {
                    // A key that has run dry starves the node exactly the way
                    // an absent source does, rather than silently reverting
                    // the detector to the audio.
                    break;
                }
                self->key_pending = (const int16_t *)raw;
                self->key_pending_frames = raw_bytes / width;
            }
            if (run > self->key_pending_frames) {
                run = self->key_pending_frames;
            }
            key = self->key_pending;
            self->key_pending += run * self->base.channel_count;
            self->key_pending_frames -= run;
        }
        audiodsp_dynamics_process_s16_key(&self->config, &self->state,
            &self->buffer[produced * 2], self->pending, key, run);
        self->pending += run * self->base.channel_count;
        self->pending_frames -= run;
        produced += run;
    }
    // A starved chain gets silence rather than a short block: this node sits
    // in the middle of a live graph and never reports itself finished.
    if (produced == 0) {
        memset(self->buffer, 0, sizeof(self->buffer));
        produced = AUDIODSP_DYNAMICS_FRAMES;
    }
    *buffer = (uint8_t *)self->buffer;
    *buffer_length = produced * 2u * self->base.channel_count;
    return GET_BUFFER_MORE_DATA;
}
