// audioroute.Splitter for CircuitPython: the source pull. See Splitter.h.
//
// SPDX-License-Identifier: MIT

#include "shared-module/audioroute/Splitter.h"

void audioroute_splitter_pull(audioroute_splitter_obj_t *self) {
    if (self->deinited || self->source == MP_OBJ_NULL) {
        return;
    }
    const uint32_t width = 2u * self->state.channel_count;
    // WHAT THE LAST PULL COULD NOT FIT COMES FIRST. A source hands back what
    // it has -- a RawSample over a 9600-frame table returns all 9600 in one
    // call -- and the ring holds 8192. Writing the lot laps every cursor
    // including the one about to read, so the head is destroyed unseen and the
    // stream has a seam at 8192. The ring takes one ring's worth, this holds
    // the rest, and the source is not asked again until it is gone. Same shape
    // as MixerVoice's remaining_buffer. audioif#87.
    if (self->pending_frames == 0) {
        uint8_t *raw = NULL;
        uint32_t raw_bytes = 0;
        audioio_get_buffer_result_t result = audiosample_get_buffer(
            self->source, false, 0, &raw, &raw_bytes);
        if (result == GET_BUFFER_ERROR || raw == NULL) {
            return;
        }
        if (raw_bytes % width != 0) {
            return;
        }
        self->pending = raw;
        self->pending_frames = raw_bytes / width;
        if (self->pending_frames == 0) {
            return;
        }
    }
    const uint32_t taken = audioif_splitter_write(&self->state,
        (const int16_t *)self->pending, self->pending_frames);
    self->pending += (size_t)taken * width;
    self->pending_frames -= taken;
}
