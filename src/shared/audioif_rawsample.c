// SPDX-License-Identifier: MIT

#include "shared/audioif_fp_contract.h"

#include "shared/audioif_rawsample.h"

static audioif_status_t rawsample_reset(void *context,
    bool single_channel_output, uint8_t audio_channel) {
    (void)single_channel_output;
    (void)audio_channel;
    audioif_rawsample_state_t *state = context;
    if (state->deinited || state->buffer == NULL) {
        return AUDIOIF_STATUS_DEINITIALIZED;
    }
    // RawSample's double buffers model alternating producer-owned halves;
    // reset does not rewind that producer state in CircuitPython.
    return AUDIOIF_STATUS_OK;
}

static audioif_status_t rawsample_get(void *context,
    bool single_channel_output, uint8_t audio_channel,
    const uint8_t **buffer, uint32_t *buffer_length,
    audioif_buffer_result_t *result) {
    audioif_rawsample_state_t *state = context;
    if (state->deinited || state->buffer == NULL) {
        return AUDIOIF_STATUS_DEINITIALIZED;
    }

    const uint32_t sample_width = state->info->bits_per_sample / 8;
    uint32_t offset = 0;
    if (single_channel_output) {
        offset = (audio_channel % state->info->channel_count) * sample_width;
    }
    if (state->info->single_buffer) {
        *buffer_length = state->info->max_buffer_length;
    } else {
        *buffer_length = state->info->max_buffer_length / 2;
        offset += *buffer_length * state->buffer_index;
        state->buffer_index = 1 - state->buffer_index;
    }
    *buffer = state->buffer + offset;
    *result = AUDIOIF_BUFFER_DONE;
    return AUDIOIF_STATUS_OK;
}

static const audioif_sample_ops_t rawsample_ops = {
    .reset_buffer = rawsample_reset,
    .get_buffer = rawsample_get,
};

void audioif_rawsample_construct(audioif_rawsample_state_t *state,
    audioif_sample_info_t *info, uint8_t *buffer, uint32_t len,
    uint8_t bytes_per_sample, bool samples_signed, uint8_t channel_count,
    uint32_t sample_rate, bool single_buffer) {
    info->bits_per_sample = bytes_per_sample * 8;
    info->samples_signed = samples_signed;
    info->max_buffer_length = len;
    info->channel_count = channel_count;
    info->sample_rate = sample_rate;
    info->single_buffer = single_buffer;
    state->info = info;
    state->buffer = buffer;
    state->buffer_length = len;
    state->buffer_index = 0;
    state->deinited = false;
}

void audioif_rawsample_deinit(audioif_rawsample_state_t *state) {
    state->buffer = NULL;
    state->deinited = true;
}

audioif_sample_source_t audioif_rawsample_source(audioif_rawsample_state_t *state) {
    audioif_sample_source_t source = {
        .ops = &rawsample_ops,
        .context = state,
        .info = state->info,
    };
    return source;
}
