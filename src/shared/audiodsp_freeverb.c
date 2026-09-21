// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_freeverb.h"
#include "shared/audiodsp_synth_dsp.h"

static const uint16_t default_comb_sizes[8] =
    {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static const uint16_t default_allpass_sizes[4] = {556, 441, 341, 225};

void audiodsp_freeverb_process_s16_banks(int16_t *output, const int16_t *input,
    size_t sample_count, int16_t *const comb_buffers[8],
    const uint16_t comb_sizes[8], uint16_t *comb_indices,
    int16_t *comb_filters, int16_t *const allpass_buffers[4],
    const uint16_t allpass_sizes[4], uint16_t *allpass_indices,
    double roomsize, double damp, double mix) {
    if (roomsize < 0) roomsize = 0; else if (roomsize > 1) roomsize = 1;
    if (damp < 0) damp = 0; else if (damp > 1) damp = 1;
    if (mix < 0) mix = 0; else if (mix > 1) mix = 1;
    int16_t feedback = (int16_t)(roomsize * 9175.04) + 22937;
    int16_t damp1 = (int16_t)(damp * 13107.2);
    int16_t damp2 = (int16_t)(32768 - damp1);
    mix *= 2;
    int16_t dry = (int16_t)((2 - mix < 1 ? 2 - mix : 1) * 32767);
    int16_t wet = (int16_t)((mix < 1 ? mix : 1) * 32767);
    int32_t pair_scale = 0xfffffff / (32768 * 2 - 28000);
    for (size_t i = 0; i < sample_count; i++) {
        int32_t sample = input == NULL ? 0 : input[i];
        int16_t reverb_input = audiodsp_sat16(sample * 8738, 17);
        int32_t sum = 0;
        for (size_t comb = 0; comb < 8; comb++) {
            int16_t *buffer = comb_buffers[comb];
            uint16_t index = comb_indices[comb];
            int16_t delayed = buffer[index];
            sum += delayed;
            comb_filters[comb] = audiodsp_sat16(
                delayed * damp2 + comb_filters[comb] * damp1, 15);
            buffer[index] = audiodsp_sat16(reverb_input + audiodsp_sat16(
                comb_filters[comb] * feedback, 15), 0);
            if (++index >= comb_sizes[comb]) index = 0;
            comb_indices[comb] = index;
        }
        int16_t effect = audiodsp_sat16(sum * 31457, 17);
        for (size_t allpass = 0; allpass < 4; allpass++) {
            int16_t *buffer = allpass_buffers[allpass];
            uint16_t index = allpass_indices[allpass];
            int16_t delayed = buffer[index];
            buffer[index] = (int16_t)(effect + (delayed >> 1));
            effect = audiodsp_sat16(delayed - effect, 1);
            if (++index >= allpass_sizes[allpass]) index = 0;
            allpass_indices[allpass] = index;
        }
        int32_t word = effect * 30;
        word = audiodsp_sat16(sample * dry, 15) +
            audiodsp_sat16(word * wet, 15);
        output[i] = audiodsp_mix_down_sample(
            word, pair_scale, -28000, 28000);
    }
}

void audiodsp_freeverb_process_s16(int16_t *output, const int16_t *input,
    size_t sample_count, int16_t *comb_buffers, uint32_t *comb_indices,
    int16_t *comb_filters, int16_t *allpass_buffers,
    uint32_t *allpass_indices, double roomsize, double damp, double mix) {
    int16_t *comb_banks[8];
    int16_t *allpass_banks[4];
    uint16_t comb_indices16[8];
    uint16_t allpass_indices16[4];
    size_t offset = 0;
    for (size_t i = 0; i < 8; i++) {
        comb_banks[i] = comb_buffers + offset;
        comb_indices16[i] = (uint16_t)comb_indices[i];
        offset += default_comb_sizes[i];
    }
    offset = 0;
    for (size_t i = 0; i < 4; i++) {
        allpass_banks[i] = allpass_buffers + offset;
        allpass_indices16[i] = (uint16_t)allpass_indices[i];
        offset += default_allpass_sizes[i];
    }
    audiodsp_freeverb_process_s16_banks(output, input, sample_count,
        comb_banks, default_comb_sizes, comb_indices16, comb_filters,
        allpass_banks, default_allpass_sizes, allpass_indices16,
        roomsize, damp, mix);
    for (size_t i = 0; i < 8; i++) comb_indices[i] = comb_indices16[i];
    for (size_t i = 0; i < 4; i++) allpass_indices[i] = allpass_indices16[i];
}
