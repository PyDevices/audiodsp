// Runtime-neutral phaser sample processing.
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

void audiodsp_phaser_process_s16(int16_t *samples, size_t sample_count,
    int16_t *feedback_words, int16_t *allpass_words, uint8_t channel_count,
    uint8_t stages, double frequency, double nyquist, double feedback,
    double mix);
void audiodsp_phaser_process_s16_fixed(int16_t *samples, size_t sample_count,
    int16_t *feedback_words, int16_t *allpass_words, uint8_t channel_count,
    uint8_t stages, int16_t coefficient, int16_t feedback, int16_t mix);
