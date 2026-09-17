// SPDX-License-Identifier: MIT

#include "shared/audioif_fp_contract.h"

#include "shared/audioif_phaser.h"
#include "shared/audioif_synth_dsp.h"

void audioif_phaser_process_s16(int16_t *samples, size_t sample_count,
    int16_t *feedback_words, int16_t *allpass_words, uint8_t channel_count,
    uint8_t stages, double frequency, double nyquist, double feedback_value,
    double mix_value) {
    int16_t feedback = (int16_t)(feedback_value * 32767);
    int16_t mix = (int16_t)(mix_value * 32767);
    if (mix <= 328) return;
    frequency /= nyquist;
    int16_t coefficient = (int16_t)((1.0 - frequency) /
        (1.0 + frequency) * 32767);
    audioif_phaser_process_s16_fixed(samples, sample_count, feedback_words,
        allpass_words, channel_count, stages, coefficient, feedback, mix);
}

void audioif_phaser_process_s16_fixed(int16_t *samples, size_t sample_count,
    int16_t *feedback_words, int16_t *allpass_words, uint8_t channel_count,
    uint8_t stages, int16_t coefficient, int16_t feedback, int16_t mix) {
    if (mix <= 328) return;
    for (size_t i = 0; i < sample_count; i++) {
        uint8_t channel = (uint8_t)(i % channel_count);
        size_t offset = (size_t)stages * channel;
        int32_t sample = samples[i];
        int32_t word = audioif_sat16(sample + audioif_sat16(
            (int32_t)feedback_words[channel] * feedback, 15), 0);
        for (uint8_t stage = 0; stage < stages; stage++) {
            size_t index = offset + stage;
            int32_t allpass = audioif_sat16(audioif_sat16(
                word * -coefficient, 15) + allpass_words[index], 0);
            allpass_words[index] = audioif_sat16(audioif_sat16(
                allpass * coefficient, 15) + word, 0);
            word = allpass;
        }
        feedback_words[channel] = (int16_t)word;
        samples[i] = audioif_mix_down_sample(
            sample + audioif_sat16(word * mix, 15), 2, -28000, 28000);
    }
}
