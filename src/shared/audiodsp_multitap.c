// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_multitap.h"
#include "shared/audiodsp_synth_dsp.h"

uint32_t audiodsp_multitap_process_s16(int16_t *output,
    const int16_t *input, size_t sample_count, int16_t *delay_buffer,
    uint32_t position, uint32_t delay_samples, uint8_t channel_count,
    const uint32_t *tap_offsets, const double *tap_levels, size_t tap_count,
    double decay, double mix) {
    int32_t tap_scale = tap_count > 1 ?
        0xfffffff / (32768 * (int32_t)tap_count - 28000) : 0;
    int32_t pair_scale = 0xfffffff / (32768 * 2 - 28000);
    for (size_t i = 0; i < sample_count; i++) {
        uint8_t channel = (uint8_t)(i % channel_count);
        uint32_t plane = delay_samples * channel;
        int32_t sample = input[i];
        int32_t word = 0;
        for (size_t tap = 0; tap < tap_count; tap++) {
            uint32_t tap_position =
                (position + delay_samples - tap_offsets[tap]) % delay_samples;
            word += (int32_t)(delay_buffer[tap_position + plane] *
                tap_levels[tap]);
        }
        if (tap_count > 1) {
            word = audiodsp_mix_down_sample(word, tap_scale, -28000, 28000);
        }
        int32_t delayed = delay_buffer[position + plane];
        if (tap_count == 0) word = delayed;
        delayed = (int32_t)(delayed * decay) + sample;
        delay_buffer[position + plane] = audiodsp_mix_down_sample(
            delayed, pair_scale, -28000, 28000);
        double dry = 2.0 - mix < 1.0 ? 2.0 - mix : 1.0;
        double wet = mix < 1.0 ? mix : 1.0;
        word = (int32_t)(sample * dry + word * wet);
        output[i] = audiodsp_mix_down_sample(
            word, pair_scale, -28000, 28000);
        if ((channel_count == 1 || channel == 1) && ++position >= delay_samples) {
            position = 0;
        }
    }
    return position;
}
