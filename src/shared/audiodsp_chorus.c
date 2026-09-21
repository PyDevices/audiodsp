// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_chorus.h"
#include "shared/audiodsp_synth_dsp.h"

uint32_t audiodsp_chorus_process_s16(int16_t *output,
    const int16_t *input, size_t sample_count, int16_t *delay_buffer,
    uint32_t position, uint32_t delay_samples, uint32_t maximum_samples,
    int32_t voices, double mix) {
    int32_t scale = 0xfffffff / (32768 * voices - 28000);
    for (size_t i = 0; i < sample_count; i++) {
        int32_t sample = input[i];
        delay_buffer[position++] = (int16_t)sample;
        int32_t word = 0;
        if (voices == 1) {
            word = sample;
        } else {
            int32_t step = (int32_t)(delay_samples / (voices - 1)) - 1;
            int32_t read_position = (int32_t)position - 1;
            for (int32_t voice = 0; voice < voices; voice++) {
                if (read_position < 0) read_position += maximum_samples;
                word += delay_buffer[read_position];
                read_position -= step;
            }
            word = audiodsp_mix_down_sample(word, scale, -28000, 28000);
        }
        word = sample + (int32_t)(word * mix);
        output[i] = audiodsp_mix_down_sample(word, 2, -28000, 28000);
        if (position >= maximum_samples) position = 0;
    }
    return position;
}
