// audiooscillator.Oscillator for CircuitPython: the buffer plumbing around
// shared/audiodsp_oscillator.c. See Oscillator.h.
//
// SPDX-License-Identifier: MIT

#include "shared-module/audiooscillator/Oscillator.h"

#include <string.h>

void audiooscillator_oscillator_reset_buffer(
    audiooscillator_oscillator_obj_t *self, bool single_channel_output,
    uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    // A host reset starts the waveform again from phase zero and keeps the
    // input frames already taken: the inputs were not reset.
    audiodsp_oscillator_reset(&self->state);
}

audioio_get_buffer_result_t audiooscillator_oscillator_get_buffer(
    audiooscillator_oscillator_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    const uint32_t out_channels = self->base.channel_count;
    uint32_t produced = 0;
    while (produced < self->frames) {
        uint32_t run = self->frames - produced;
        const int16_t *live[AUDIODSP_OSCILLATOR_INPUTS];
        uint32_t strides[AUDIODSP_OSCILLATOR_INPUTS];
        for (int index = 0; index < (int)AUDIODSP_OSCILLATOR_INPUTS;
             ++index) {
            live[index] = NULL;
            strides[index] = self->input_channels[index];
            if (self->input[index] == MP_OBJ_NULL) {
                continue;
            }
            if (self->pending_frames[index] == 0) {
                uint8_t *raw = NULL;
                uint32_t raw_bytes = 0;
                audioio_get_buffer_result_t result = audiosample_get_buffer(
                    self->input[index], false, 0, &raw, &raw_bytes);
                const uint32_t width = 2u * self->input_channels[index];
                // An input that has run dry counts as zero for this run.
                if (result == GET_BUFFER_ERROR || raw == NULL ||
                    raw_bytes < width) {
                    continue;
                }
                self->pending[index] = (const int16_t *)(const void *)raw;
                self->pending_frames[index] = raw_bytes / width;
            }
            if (run > self->pending_frames[index]) {
                run = self->pending_frames[index];
            }
            live[index] = self->pending[index];
        }
        audiodsp_oscillator_process_s16(&self->config, &self->state,
            &self->buffer[produced * out_channels], run, live, strides);
        for (int index = 0; index < (int)AUDIODSP_OSCILLATOR_INPUTS;
             ++index) {
            if (live[index] != NULL) {
                self->pending[index] += run * strides[index];
                self->pending_frames[index] -= run;
            }
        }
        produced += run;
    }
    *buffer = (uint8_t *)self->buffer;
    *buffer_length = produced * 2u * out_channels;
    return GET_BUFFER_MORE_DATA;
}
