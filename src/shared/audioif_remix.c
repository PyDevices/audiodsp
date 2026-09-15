// SPDX-License-Identifier: MIT

#include "shared/audioif_remix.h"

#include <string.h>

void audioif_remix_s16(int16_t *dst, const int16_t *src, uint32_t frames,
    uint32_t src_ch, uint32_t dst_ch) {
    if (src_ch == dst_ch) {
        if (dst != src && frames != 0) {
            memcpy(dst, src, (size_t)frames * src_ch * sizeof(int16_t));
        }
        return;
    }
    if (src_ch == 2 && dst_ch == 1) {
        for (uint32_t i = 0; i < frames; i++) {
            int32_t left = src[2u * i];
            int32_t right = src[2u * i + 1u];
            dst[i] = (int16_t)((left + right) / 2);
        }
        return;
    }
    for (uint32_t i = 0; i < frames; i++) {
        int16_t sample = src[i];
        dst[2u * i] = sample;
        dst[2u * i + 1u] = sample;
    }
}
