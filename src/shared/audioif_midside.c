// Runtime-neutral mid/side matrix. See audioif_midside.h for provenance.
// SPDX-License-Identifier: MIT

#include "shared/audioif_fp_contract.h"

#include "shared/audioif_midside.h"

void audioif_midside_config_init(audioif_midside_config_t *config) {
    config->channel_count = 2;
    config->width = 16384;
}

void audioif_midside_set_channel_count(audioif_midside_config_t *config,
    uint32_t channel_count) {
    config->channel_count = channel_count == 1u ? 1u : 2u;
}

void audioif_midside_set_width(audioif_midside_config_t *config, float width) {
    if (width <= 0.0f) {
        config->width = 0;
    } else if (width >= 2.0f) {
        config->width = 32768;
    } else {
        config->width = (int32_t)(width * 16384.0f + 0.5f);
    }
}

void audioif_midside_process_s16(const audioif_midside_config_t *config,
    int16_t *out, const int16_t *in, uint32_t frames) {
    if (config->channel_count == 1u) {
        for (uint32_t i = 0; i < frames; ++i) {
            out[i] = in[i];
        }
        return;
    }
    const int32_t width = config->width;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        const int32_t left = in[frame * 2u];
        const int32_t right = in[frame * 2u + 1u];
        const int32_t sum = left + right;
        const int32_t difference = left - right;
        // |difference| <= 65535 and width <= 32768, so this product is at
        // most 2147450880 -- the reason width is Q14. See the header.
        const int32_t side = (width * difference) >> 14;
        // Halve LAST. At width = 1 the shift undoes the doubling exactly:
        // side == difference (a Q14 multiply by 16384 is a shift left by 14,
        // and the shift right by 14 takes it straight back, negatives
        // included), so sum + side is 2*left and (2*left + 1) >> 1 is left
        // for every int16 there is. Halving the sum first would lose a bit
        // before the side was ever added, and a MidSide left at its default
        // would quietly cost the chain an LSB.
        //
        // The +1 rounds half up rather than toward minus infinity. Without
        // it every odd result loses half an LSB downward and the halving
        // becomes a small DC offset on anything asymmetric -- which is
        // exactly what a drive downstream turns into an audible bias. It
        // goes on both halves, so the mono sum stays on (left + right) or
        // one more, never split between the channels.
        int32_t left_out = (sum + side + 1) >> 1;
        int32_t right_out = (sum - side + 1) >> 1;
        if (left_out > 32767) {
            left_out = 32767;
        } else if (left_out < -32768) {
            left_out = -32768;
        }
        if (right_out > 32767) {
            right_out = 32767;
        } else if (right_out < -32768) {
            right_out = -32768;
        }
        out[frame * 2u] = (int16_t)left_out;
        out[frame * 2u + 1u] = (int16_t)right_out;
    }
}
