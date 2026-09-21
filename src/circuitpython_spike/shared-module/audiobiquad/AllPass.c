// audiobiquad.AllPass for CircuitPython: the buffer plumbing around
// shared/audiodsp_filter_f32.c. See AllPass.h.
//
// SPDX-License-Identifier: MIT

#include "shared-module/audiobiquad/AllPass.h"

#include <string.h>

void audiobiquad_allpass_refresh(audiobiquad_allpass_obj_t *self) {
    audiodsp_allpass_f32_configure(&self->config,
        AUDIODSP_ALLPASS_F32_OPT_FREQUENCY,
        (float)synthio_block_slot_get(&self->frequency));
    audiodsp_allpass_f32_configure(&self->config,
        AUDIODSP_ALLPASS_F32_OPT_FEEDBACK,
        (float)synthio_block_slot_get(&self->feedback));
    audiodsp_allpass_f32_configure(&self->config, AUDIODSP_ALLPASS_F32_OPT_MIX,
        (float)synthio_block_slot_get(&self->mix));
    audiodsp_allpass_f32_config_finish(&self->config);
}

// One chunk of the block layer, then the values it produced.
static void audiobiquad_allpass_apply_blocks(audiobiquad_allpass_obj_t *self,
    uint32_t frames) {
    shared_bindings_synthio_lfo_tick(self->base.sample_rate,
        (uint16_t)frames);
    audiobiquad_allpass_refresh(self);
}

void audiobiquad_allpass_reset_buffer(audiobiquad_allpass_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    self->pending = NULL;
    self->pending_frames = 0;
    audiodsp_allpass_f32_reset(&self->state);
}

audioio_get_buffer_result_t audiobiquad_allpass_get_buffer(
    audiobiquad_allpass_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    uint32_t produced = 0;
    while (produced < AUDIODSP_FILTER_F32_FRAMES) {
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
        uint32_t run = AUDIODSP_FILTER_F32_FRAMES - produced;
        if (run > self->pending_frames) {
            run = self->pending_frames;
        }
        audiobiquad_allpass_apply_blocks(self, run);
        audiodsp_allpass_f32_process_s16(&self->config, &self->state,
            &self->buffer[produced * self->base.channel_count],
            self->pending, run);
        self->pending += run * self->base.channel_count;
        self->pending_frames -= run;
        produced += run;
    }
    if (produced == 0) {
        memset(self->buffer, 0, sizeof(self->buffer));
        produced = AUDIODSP_FILTER_F32_FRAMES;
    }
    *buffer = (uint8_t *)self->buffer;
    *buffer_length = produced * 2u * self->base.channel_count;
    return GET_BUFFER_MORE_DATA;
}
