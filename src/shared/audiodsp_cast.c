// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_cast.h"

// Rounding is written out rather than left to round() and roundf(), so no
// build needs libm for it, and the fraction is taken after the product has
// been rounded to the source's width: with contraction off (the header above)
// `v - i` cannot fuse with the multiply and see a product nobody stored.
//
// The comparisons come first and in this order because each one is false for
// NaN: a NaN falls through both clamps and is caught by the third test, which
// nothing else left can fail.

static int16_t cast_one_f32(float v) {
    if (v >= 32767.0f) {
        return 32767;
    }
    if (v <= -32768.0f) {
        return -32768;
    }
    if (!(v > -32768.0f)) {
        return 0;
    }
    int32_t i = (int32_t)v;
    float frac = v - (float)i;
    if (frac >= 0.5f) {
        i += 1;
    } else if (frac <= -0.5f) {
        i -= 1;
    }
    return (int16_t)i;
}

static int16_t cast_one_f64(double v) {
    if (v >= 32767.0) {
        return 32767;
    }
    if (v <= -32768.0) {
        return -32768;
    }
    if (!(v > -32768.0)) {
        return 0;
    }
    int32_t i = (int32_t)v;
    double frac = v - (double)i;
    if (frac >= 0.5) {
        i += 1;
    } else if (frac <= -0.5) {
        i -= 1;
    }
    return (int16_t)i;
}

void audiodsp_cast_s16_f32(int16_t *dst, const float *src, size_t count,
    float scale) {
    for (size_t i = 0; i < count; i++) {
        dst[i] = cast_one_f32(src[i] * scale);
    }
}

void audiodsp_cast_s16_f64(int16_t *dst, const double *src, size_t count,
    double scale) {
    for (size_t i = 0; i < count; i++) {
        dst[i] = cast_one_f64(src[i] * scale);
    }
}
