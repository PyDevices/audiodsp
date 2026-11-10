// Runtime-neutral sample-wise multiply. See audiodsp_multiply.h for provenance.
// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"

#include <stddef.h>

#include "shared/audiodsp_multiply.h"

void audiodsp_multiply_config_init(audiodsp_multiply_config_t *config) {
    config->channel_count = 2;
    config->mix = 32768;
    config->ramp_frames = 0;
}

void audiodsp_multiply_set_sample_rate(audiodsp_multiply_config_t *config,
    uint32_t sample_rate) {
    config->ramp_frames = audiodsp_level_ramp_frames(sample_rate,
        AUDIODSP_LEVEL_RAMP_MS);
}

void audiodsp_multiply_state_clear(audiodsp_multiply_state_t *state) {
    audiodsp_level_ramp_clear(&state->mix_ramp);
}

void audiodsp_multiply_set_channel_count(audiodsp_multiply_config_t *config,
    uint32_t channel_count) {
    config->channel_count = channel_count == 1u ? 1u : 2u;
}

void audiodsp_multiply_set_mix(audiodsp_multiply_config_t *config, float mix) {
    if (mix <= 0.0f) {
        config->mix = 0;
    } else if (mix >= 1.0f) {
        config->mix = 32768;
    } else {
        config->mix = (int32_t)(mix * 32768.0f + 0.5f);
    }
}

void audiodsp_multiply_process_s16(const audiodsp_multiply_config_t *config,
    audiodsp_multiply_state_t *state, int16_t *out, const int16_t *a,
    const int16_t *b, uint32_t frames) {
    int32_t wet = config->mix;
    int32_t dry = 32768 - wet;
    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    audiodsp_level_ramp_t *ramp = NULL;
    if (state != NULL) {
        ramp = &state->mix_ramp;
        audiodsp_level_ramp_aim(ramp, (float)config->mix, config->ramp_frames);
    }
    for (uint32_t frame = 0; frame < frames; ++frame) {
        // While a walk runs the frame takes the next step, rounded to the
        // Q15 grid; once it lands `current` is `mix` exactly, so a node at
        // rest renders the bytes it did before the walk existed.
        if (ramp != NULL && audiodsp_level_ramp_moving(ramp)) {
            wet = (int32_t)(audiodsp_level_ramp_next(ramp) + 0.5f);
            dry = 32768 - wet;
        }
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const uint32_t i = frame * channels + channel;
            const int32_t signal = a[i];
            // Scaling by 32768 rather than 32767 keeps a full-scale
            // modulator a gain of one to within an LSB and keeps the shift a
            // shift. The two negative rails are the one product that lands
            // outside int16, which is what the clamp below is for.
            const int32_t product = (signal * (int32_t)b[i]) >> 15;
            int32_t value = (dry * signal + wet * product) >> 15;
            if (value > 32767) {
                value = 32767;
            } else if (value < -32768) {
                value = -32768;
            }
            out[i] = (int16_t)value;
        }
    }
}

void audiodsp_multiply_passthrough_s16(int16_t *out, const int16_t *a,
    uint32_t frames) {
    // Passthrough has no config, and is retained as the stereo helper used by
    // older callers. Mono bindings copy their source bytes directly.
    const uint32_t samples = frames * 2u;
    for (uint32_t i = 0; i < samples; ++i) {
        out[i] = a[i];
    }
}
