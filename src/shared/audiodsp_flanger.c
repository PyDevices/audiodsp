// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_flanger.h"
#include "shared/audiodsp_synth_dsp.h"

// Same pair-voice scale the CPython twin's `_mix_down` uses
// (`0xfffffff // (32768 * 2 - 28000)`).
#define AUDIODSP_FLANGER_MIX_SCALE (0xfffffff / (32768 * 2 - 28000))

uint32_t audiodsp_flanger_ms_to_frames_q16(double ms, uint32_t sample_rate,
    uint32_t frames) {
    double count = ms * (double)sample_rate / 1000.0;
    double max_frames = (double)((frames - 2) < 65535u ? (frames - 2) : 65535u);
    if (count < 1.0) {
        count = 1.0;
    }
    if (count > max_frames) {
        count = max_frames;
    }
    return (uint32_t)(count * 65536.0);
}

uint32_t audiodsp_flanger_triangle(uint32_t phase) {
    uint32_t tri = phase >> 15;
    if (tri > 65535) {
        tri = 131071 - tri;
    }
    return tri;
}

void audiodsp_flanger_process_s16(int16_t *output, const int16_t *input,
    size_t sample_count, int16_t *delay_line, uint32_t frames,
    uint8_t channel_count, audiodsp_flanger_state_t *state,
    uint32_t delay_min, uint32_t delay_span, uint32_t phase_inc,
    int32_t feedback, int32_t mix, bool invert) {
    for (size_t index = 0; index < sample_count; index++) {
        int32_t sample = input[index];
        uint8_t channel = (channel_count == 2 && (index % 2) == 1) ? 1 : 0;
        state->lfo_phase[channel] += phase_inc;
        uint32_t tri = audiodsp_flanger_triangle(state->lfo_phase[channel]);
        uint32_t delay_q16 = delay_min +
            (uint32_t)(((uint64_t)delay_span * (uint64_t)tri) >> 16);
        uint32_t delay_int = delay_q16 >> 16;
        uint32_t delay_frac = delay_q16 & 0xffff;
        uint32_t plane = (uint32_t)channel * frames;
        uint32_t write = state->write_pos[channel];
        uint32_t r0 = write + frames - delay_int;
        if (r0 >= frames) {
            r0 -= frames;
        }
        uint32_t r1 = (r0 == 0) ? frames - 1 : r0 - 1;
        int32_t s0 = delay_line[plane + r0];
        int32_t s1 = delay_line[plane + r1];
        // 64-bit for the same reason the span/tri product above is: the
        // twin computes this in Python, which does not overflow. `s1 - s0`
        // reaches +/-65535 when adjacent line samples sit at opposite rails,
        // and 65535 * 65535 is 4.29e9 - past INT32_MAX. In int32 that is
        // signed overflow, and it is reachable on ordinary material: a
        // full-scale alternating source diverged from the CPython twin on
        // every setting tried until this cast.
        int32_t wet = s0 +
            (int32_t)((((int64_t)s1 - (int64_t)s0) * (int64_t)delay_frac) >> 16);
        delay_line[plane + write] = audiodsp_sat16(
            sample + audiodsp_sat16(wet * feedback, 15), 0);
        state->write_pos[channel] = (write + 1 == frames) ? 0 : write + 1;
        int32_t word;
        if (mix <= 328) {
            word = sample;
        } else {
            int32_t wet_word = audiodsp_sat16(wet * mix, 15);
            word = invert ? sample - wet_word : sample + wet_word;
            word = audiodsp_mix_down_sample(word, AUDIODSP_FLANGER_MIX_SCALE,
                -28000, 28000);
        }
        output[index] = (int16_t)word;
    }
}
