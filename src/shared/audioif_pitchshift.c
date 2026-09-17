// SPDX-License-Identifier: MIT

#include "shared/audioif_fp_contract.h"

#include "shared/audioif_pitchshift.h"
#include "shared/audioif_synth_dsp.h"

#define READ_SHIFT 8

void audioif_pitchshift_process_s16(int16_t *output, const int16_t *input,
    size_t sample_count, int16_t *window, uint32_t window_samples,
    int16_t *overlap, uint32_t overlap_samples, uint8_t channel_count,
    uint32_t read_rate, double mix, audioif_pitchshift_positions_t *positions) {
    int32_t scale = 0xfffffff / (32768 * 2 - 28000);
    for (size_t i = 0; i < sample_count; i++) {
        uint8_t channel = (uint8_t)(i % channel_count);
        uint32_t window_plane = window_samples * channel;
        uint32_t overlap_plane = overlap_samples * channel;
        int32_t sample = input[i];
        if (overlap_samples) {
            window[positions->window_index + window_plane] =
                overlap[positions->overlap_index + overlap_plane];
            overlap[positions->overlap_index + overlap_plane] =
                (int16_t)sample;
        } else {
            window[positions->window_index + window_plane] = (int16_t)sample;
        }
        uint32_t read = positions->read_index >> READ_SHIFT;
        uint32_t overlap_offset = read +
            window_samples * (read < positions->window_index) -
            positions->window_index;
        int32_t word = window[read + window_plane];
        if (overlap_samples && overlap_offset > 0 &&
            overlap_offset <= overlap_samples) {
            word *= (int32_t)overlap_offset;
            word += overlap[(positions->overlap_index + overlap_offset) %
                overlap_samples + overlap_plane] *
                (int32_t)(overlap_samples - overlap_offset);
            word /= (int32_t)overlap_samples;
        }
        double dry = 2.0 - mix < 1.0 ? 2.0 - mix : 1.0;
        double wet = mix < 1.0 ? mix : 1.0;
        word = (int32_t)(sample * dry + word * wet);
        output[i] = audioif_mix_down_sample(word, scale, -28000, 28000);
        if (channel_count == 1 || channel == 1) {
            if (++positions->window_index >= window_samples) {
                positions->window_index = 0;
            }
            if (overlap_samples &&
                ++positions->overlap_index >= overlap_samples) {
                positions->overlap_index = 0;
            }
            positions->read_index += read_rate;
            if (positions->read_index >= window_samples << READ_SHIFT) {
                positions->read_index -= window_samples << READ_SHIFT;
            }
        }
    }
}
