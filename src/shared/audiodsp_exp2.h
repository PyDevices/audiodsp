// 2^x with the same bits on every target, for the nodes whose pitch or cutoff
// is read from an audio stream.
//
// libm's exp2f differs in the last place between glibc, newlib and
// MicroPython's own, and our nodes are held to one render everywhere. The
// integer part goes into the exponent; the fraction, centred on a half so the
// series converges fast, is a seven-term Taylor series, about 1e-7 relative at
// the ends.
//
// Static inline rather than a function of its own, so each translation unit
// that includes it compiles it under its own audiodsp_fp_contract.h.
//
// SPDX-License-Identifier: MIT

#pragma once

static inline float audiodsp_exp2_poly(float x) {
    if (x > 60.0f) {
        x = 60.0f;
    } else if (x < -60.0f) {
        x = -60.0f;
    }
    int whole = (int)x;
    if ((float)whole > x) {
        --whole;
    }
    const float g = (x - (float)whole) - 0.5f;
    const float p = 1.0f + g * (0.69314718f + g * (0.24022651f
        + g * (0.05550411f + g * (0.00961813f + g * (0.00133336f
        + g * 0.00015404f)))));
    float scale = 1.0f;
    float step = whole >= 0 ? 2.0f : 0.5f;
    unsigned int n = (unsigned int)(whole >= 0 ? whole : -whole);
    while (n) {
        if (n & 1u) {
            scale *= step;
        }
        step *= step;
        n >>= 1;
    }
    return 1.41421356f * p * scale;
}
