// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_freeverb.h"
#include "shared/audiodsp_synth_dsp.h"

static const uint16_t default_comb_sizes[8] =
    {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static const uint16_t default_allpass_sizes[4] = {556, 441, 341, 225};

void audiodsp_freeverb_process_s16_banks(int16_t *output, const int16_t *input,
    size_t sample_count, uint32_t channel_count,
    int16_t *const comb_buffers[16], const uint16_t comb_sizes[16],
    uint16_t *comb_indices, int16_t *comb_filters,
    int16_t *const allpass_buffers[8], const uint16_t allpass_sizes[8],
    uint16_t *allpass_indices, double roomsize, double damp, double mix) {
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
    // Which channel's bank the sample takes. CircuitPython 10.3.0 declared
    // these inside the loop, so the switch to the right bank was lost on the
    // next sample and both channels ran through the left one; upstream moved
    // them out in 6dddbda87 (11.0.0-alpha.1), and so does this.
    size_t comb_offset = 0, allpass_offset = 0;
    for (size_t i = 0; i < sample_count; i++) {
        int32_t sample = input == NULL ? 0 : input[i];
        int16_t reverb_input = audiodsp_sat16(sample * 8738, 17);
        int32_t sum = 0;
        for (size_t comb = comb_offset; comb < comb_offset + 8; comb++) {
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
        for (size_t allpass = allpass_offset; allpass < allpass_offset + 4;
             allpass++) {
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
        if (channel_count == 2u && comb_offset == 0) {
            comb_offset = 8;
            allpass_offset = 4;
        } else {
            comb_offset = 0;
            allpass_offset = 0;
        }
    }
}

void audiodsp_freeverb_process_s16(int16_t *output, const int16_t *input,
    size_t sample_count, uint32_t channel_count, int16_t *comb_buffers,
    uint32_t *comb_indices, int16_t *comb_filters, int16_t *allpass_buffers,
    uint32_t *allpass_indices, double roomsize, double damp, double mix) {
    const size_t banks = channel_count == 2u ? 2u : 1u;
    int16_t *comb_banks[16];
    int16_t *allpass_banks[8];
    uint16_t comb_sizes[16];
    uint16_t allpass_sizes[8];
    uint16_t comb_indices16[16];
    uint16_t allpass_indices16[8];
    size_t offset = 0;
    for (size_t i = 0; i < 8 * banks; i++) {
        comb_banks[i] = comb_buffers + offset;
        comb_sizes[i] = default_comb_sizes[i % 8];
        comb_indices16[i] = (uint16_t)comb_indices[i];
        offset += default_comb_sizes[i % 8];
    }
    offset = 0;
    for (size_t i = 0; i < 4 * banks; i++) {
        allpass_banks[i] = allpass_buffers + offset;
        allpass_sizes[i] = default_allpass_sizes[i % 4];
        allpass_indices16[i] = (uint16_t)allpass_indices[i];
        offset += default_allpass_sizes[i % 4];
    }
    audiodsp_freeverb_process_s16_banks(output, input, sample_count,
        (uint32_t)banks, comb_banks, comb_sizes, comb_indices16, comb_filters,
        allpass_banks, allpass_sizes, allpass_indices16, roomsize, damp, mix);
    for (size_t i = 0; i < 8 * banks; i++) comb_indices[i] = comb_indices16[i];
    for (size_t i = 0; i < 4 * banks; i++) {
        allpass_indices[i] = allpass_indices16[i];
    }
}
