// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_echo.h"
#include "shared/audiodsp_synth_dsp.h"

void audiodsp_echo_process_s16(int16_t *output, const int16_t *input,
    size_t sample_count, int16_t *delay_buffer,
    uint32_t delay_samples_per_channel, uint32_t maximum_samples_per_channel,
    uint32_t rate, double decay, double mix, bool frequency_shift,
    uint8_t channel_count, audiodsp_echo_positions_t *positions) {
    int32_t mixdown_scale = 0x0fffffff / (32768 * 2 - 28000);
    for (size_t i = 0; i < sample_count; i++) {
        bool right = channel_count == 2 && i % 2 == 1;
        uint32_t offset = right ? maximum_samples_per_channel : 0;
        uint32_t position = right ? positions->right_position :
            positions->left_position;
        int32_t sample = input[i];
        int32_t echo;
        if (frequency_shift) {
            echo = delay_buffer[(position >> 8) + offset];
            uint32_t next = position + rate;
            for (uint32_t j = position >> 8; j < next >> 8; j++) {
                int32_t word = (int32_t)(delay_buffer[
                    (j % delay_samples_per_channel) + offset] * decay + sample);
                delay_buffer[(j % delay_samples_per_channel) + offset] =
                    audiodsp_mix_down_sample(word, mixdown_scale, -28000, 28000);
            }
            position = next % (delay_samples_per_channel << 8);
        } else {
            echo = delay_buffer[position + offset];
            int32_t word = (int32_t)(echo * decay + sample);
            delay_buffer[position++ + offset] = audiodsp_mix_down_sample(word,
                mixdown_scale, -28000, 28000);
            if (position >= delay_samples_per_channel) position = 0;
        }
        int32_t word = (int32_t)(sample * ((2.0 - mix < 1.0) ? 2.0 - mix : 1.0) +
            echo * ((mix < 1.0) ? mix : 1.0));
        output[i] = audiodsp_mix_down_sample(word, mixdown_scale, -28000, 28000);
        if (right) positions->right_position = position;
        else positions->left_position = position;
    }
}
