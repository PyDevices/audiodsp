// audiosampler.Sampler for CircuitPython: the buffer plumbing around
// shared/audiodsp_sampler.c. See Sampler.h.
//
// SPDX-License-Identifier: MIT

#include "shared-module/audiosampler/Sampler.h"

void audiosampler_sampler_reset_buffer(audiosampler_sampler_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    (void)self;
    (void)single_channel_output;
    (void)channel;
    // A host reset keeps every voice as it is, as audiomixer's does: a
    // sampler has no source to rewind, and a note that was sounding when the
    // output restarted should still be sounding afterwards.
}

audioio_get_buffer_result_t audiosampler_sampler_get_buffer(
    audiosampler_sampler_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    audiodsp_sampler_render_s16(&self->sampler, self->buffer);
    // A voice that ran out this block lets go of its buffer here, so a
    // one-shot drum sample is reclaimable without anyone calling stop().
    for (uint32_t v = 0; v < self->sampler.voice_count; v++) {
        audiosampler_samplervoice_obj_t *voice =
            MP_OBJ_TO_PTR(self->voice[v]);
        if (!audiodsp_sampler_playing(&self->sampler, v)) {
            voice->sample = mp_const_none;
        }
        if (!audiodsp_sampler_fading(&self->sampler, v)) {
            voice->tail_sample = mp_const_none;
        }
    }
    *buffer = (uint8_t *)self->buffer;
    *buffer_length = self->base.max_buffer_length;
    return GET_BUFFER_MORE_DATA;
}
