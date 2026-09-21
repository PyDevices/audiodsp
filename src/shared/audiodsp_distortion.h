// Runtime-neutral distortion DSP.
// SPDX-License-Identifier: MIT

#pragma once


#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AUDIODSP_DISTORTION_CLIP = 0,
    AUDIODSP_DISTORTION_LOFI = 1,
    AUDIODSP_DISTORTION_OVERDRIVE = 2,
    AUDIODSP_DISTORTION_WAVESHAPE = 3,
} audiodsp_distortion_mode_t;

int32_t audiodsp_distortion_sample(int32_t sample, double drive,
    double pre_gain, double post_gain, audiodsp_distortion_mode_t mode,
    bool soft_clip, double mix, uint32_t word_mask);
void audiodsp_distortion_process_s16(int16_t *output, const int16_t *input,
    size_t count, double drive, double pre_gain_db, double post_gain_db,
    audiodsp_distortion_mode_t mode, bool soft_clip, double mix);

