// SPDX-License-Identifier: MIT

#include "shared/audiodsp_sample.h"

audiodsp_status_t audiodsp_sample_reset(audiodsp_sample_source_t *source,
    bool single_channel_output, uint8_t audio_channel) {
    if (source == NULL || source->ops == NULL || source->ops->reset_buffer == NULL) {
        return AUDIODSP_STATUS_INVALID_ARGUMENT;
    }
    return source->ops->reset_buffer(source->context, single_channel_output, audio_channel);
}

audiodsp_status_t audiodsp_sample_get(audiodsp_sample_source_t *source,
    bool single_channel_output, uint8_t audio_channel,
    const uint8_t **buffer, uint32_t *buffer_length,
    audiodsp_buffer_result_t *result) {
    if (source == NULL || source->ops == NULL || source->ops->get_buffer == NULL ||
        buffer == NULL || buffer_length == NULL || result == NULL) {
        return AUDIODSP_STATUS_INVALID_ARGUMENT;
    }
    return source->ops->get_buffer(source->context, single_channel_output,
        audio_channel, buffer, buffer_length, result);
}

