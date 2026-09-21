// audiomodal.Bank for CircuitPython: the buffer plumbing around
// shared/audiodsp_modal.c. See Bank.h.
//
// SPDX-License-Identifier: MIT

#include "shared-module/audiomodal/Bank.h"

#include <string.h>

void audiomodal_bank_reset_buffer(audiomodal_bank_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    self->pending = NULL;
    self->pending_frames = 0;
    // Everything goes, for the reverberation tank's reason: a bank restarted
    // with the last take's partials still ringing plays the previous hit over
    // the new one.
    audiodsp_modal_reset(&self->state);
}

audioio_get_buffer_result_t audiomodal_bank_get_buffer(
    audiomodal_bank_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    audiodsp_modal_config_finish(&self->config);
    uint32_t produced = 0;
    while (produced < AUDIODSP_MODAL_FRAMES) {
        if (self->pending_frames == 0) {
            if (self->source == MP_OBJ_NULL) {
                break;
            }
            uint8_t *raw = NULL;
            uint32_t raw_bytes = 0;
            audioio_get_buffer_result_t result = audiosample_get_buffer(
                self->source, false, 0, &raw, &raw_bytes);
            const uint32_t width = 2u * self->base.channel_count;
            if (result == GET_BUFFER_ERROR || raw == NULL ||
                raw_bytes < width) {
                break;
            }
            self->pending = (const int16_t *)raw;
            self->pending_frames = raw_bytes / width;
        }
        uint32_t run = AUDIODSP_MODAL_FRAMES - produced;
        if (run > self->pending_frames) {
            run = self->pending_frames;
        }
        audiodsp_modal_process_s16(&self->config, &self->state,
            &self->buffer[produced * self->base.channel_count],
            self->pending, run);
        self->pending += run * self->base.channel_count;
        self->pending_frames -= run;
        produced += run;
    }
    // A starved chain gets silence *through the bank* rather than a short
    // block. Unlike the delay and the tank, the tail does keep ringing when
    // the source stops: that is the whole behaviour of a struck object, and a
    // drum whose decay ended the instant the stick left would be the one thing
    // this node exists not to be. Feeding zeros is what rings it out, so the
    // silent frames are pushed through the recursion rather than written over
    // the top of it.
    if (produced < AUDIODSP_MODAL_FRAMES) {
        static const int16_t quiet[AUDIODSP_MODAL_FRAMES * 2] = { 0 };
        uint32_t run = AUDIODSP_MODAL_FRAMES - produced;
        audiodsp_modal_process_s16(&self->config, &self->state,
            &self->buffer[produced * self->base.channel_count], quiet, run);
        produced = AUDIODSP_MODAL_FRAMES;
    }
    *buffer = (uint8_t *)self->buffer;
    *buffer_length = produced * 2u * self->base.channel_count;
    return GET_BUFFER_MORE_DATA;
}
