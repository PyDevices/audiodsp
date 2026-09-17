// SPDX-License-Identifier: MIT

#include "shared/audioif_fp_contract.h"

#include "shared/audioif_distortion.h"

#include <math.h>

int32_t audioif_distortion_sample(int32_t sample, double drive,
    double pre_gain, double post_gain, audioif_distortion_mode_t mode,
    bool soft_clip, double mix, uint32_t word_mask) {
    int32_t word = (int32_t)(sample * pre_gain);
    if (mode == AUDIOIF_DISTORTION_LOFI) word &= word_mask;

    if (mode != AUDIOIF_DISTORTION_LOFI || soft_clip) {
        double value = word / 32768.0;
        switch (mode) {
            case AUDIOIF_DISTORTION_CLIP:
                value = pow(fabs(value), drive);
                if (word < 0) value *= -1.0;
                break;
            case AUDIOIF_DISTORTION_LOFI:
                break;
            case AUDIOIF_DISTORTION_OVERDRIVE: {
                value *= 0.686306;
                double z = 1.0 + exp(sqrt(fabs(value)) * -0.75);
                double positive = exp(value);
                value *= -1.0;
                value = (positive - exp(value * z)) / (positive + exp(value));
                break;
            }
            case AUDIOIF_DISTORTION_WAVESHAPE:
                value = (1.0 + drive) * value /
                    (1.0 + drive * fabs(value));
                break;
        }
        value *= post_gain;
        if (soft_clip) {
            value = value > 0 ? 1.0 - exp(-value) : -1.0 + exp(value);
        }
        word = (int32_t)(value * 32767.0);
    } else {
        word = (int32_t)(word * post_gain);
    }
    if (!soft_clip) {
        if (word < -32767) word = -32767;
        if (word > 32768) word = 32768;
    }
    return (int32_t)(sample * (1.0 - mix) + word * mix);
}

void audioif_distortion_process_s16(int16_t *output, const int16_t *input,
    size_t count, double drive, double pre_gain_db, double post_gain_db,
    audioif_distortion_mode_t mode, bool soft_clip, double mix) {
    double pre_gain = exp(pre_gain_db * 0.1151292546497022842);
    double post_gain = exp(post_gain_db * 0.1151292546497022842);
    uint32_t word_mask = 0;
    if (mode == AUDIOIF_DISTORTION_CLIP) {
        drive = 1.0001 - drive;
    } else if (mode == AUDIOIF_DISTORTION_WAVESHAPE) {
        drive = 2.0 * drive / (1.0001 - drive);
    } else if (mode == AUDIOIF_DISTORTION_LOFI) {
        word_mask = 0xffffffffU ^ ((1U << (uint32_t)round(drive * 14.0)) - 1U);
    }
    for (size_t i = 0; i < count; i++) {
        output[i] = (int16_t)audioif_distortion_sample(input[i], drive,
            pre_gain, post_gain, mode, soft_clip, mix, word_mask);
    }
}
