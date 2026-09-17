// SPDX-License-Identifier: MIT

#include "shared/audioif_fp_contract.h"

#include "shared/audioif_fft.h"

#include "shared/audioif_trig.h"

uint32_t audioif_rfft_table_floats(uint32_t n) {
    return 3 * (n / 2);
}

void audioif_rfft_init(audioif_rfft_t *rfft, uint32_t n, float *tables) {
    uint32_t half = n / 2;
    rfft->n = n;
    rfft->half = half;
    rfft->log2_half = 0;
    while ((1u << rfft->log2_half) < half) rfft->log2_half++;

    // Laid out back to back in the one block the caller handed over: the
    // butterfly table first (half/2 complex), then the recombination table
    // (half complex).
    float *twiddle = tables;
    float *untangle = tables + half;
    rfft->twiddle = twiddle;
    rfft->untangle = untangle;

    audioif_sincos_t sc;
    for (uint32_t k = 0; k < half / 2; k++) {
        audioif_sincos(-2.0 * AUDIOIF_PI * (double)k / (double)half, &sc);
        twiddle[2 * k] = (float)sc.c;
        twiddle[2 * k + 1] = (float)sc.s;
    }
    for (uint32_t k = 0; k < half; k++) {
        audioif_sincos(-2.0 * AUDIOIF_PI * (double)k / (double)n, &sc);
        untangle[2 * k] = (float)sc.c;
        untangle[2 * k + 1] = (float)sc.s;
    }
}

// In-place complex transform of `length` interleaved pairs. `conjugated`
// runs it backwards by the usual identity -- conjugate in, forward, conjugate
// out -- so there is one butterfly loop to get right rather than two. The
// 1/length is applied by the caller that needs it.
static void fft_complex(float *data, uint32_t length, uint32_t log2_length,
    const float *twiddle, int conjugated) {
    if (conjugated) {
        for (uint32_t k = 0; k < length; k++) data[2 * k + 1] = -data[2 * k + 1];
    }

    // Bit reversal. Counting the reversed index forward is cheaper than
    // reversing each index from scratch: add one at the top end and carry
    // downward.
    for (uint32_t i = 1, j = 0; i < length; i++) {
        uint32_t bit = length >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float re = data[2 * i], im = data[2 * i + 1];
            data[2 * i] = data[2 * j];
            data[2 * i + 1] = data[2 * j + 1];
            data[2 * j] = re;
            data[2 * j + 1] = im;
        }
    }
    (void)log2_length;

    for (uint32_t len = 2; len <= length; len <<= 1) {
        uint32_t half = len >> 1;
        uint32_t stride = length / len;
        for (uint32_t base = 0; base < length; base += len) {
            for (uint32_t j = 0; j < half; j++) {
                uint32_t t = j * stride;
                float wr = twiddle[2 * t], wi = twiddle[2 * t + 1];
                uint32_t a = base + j, b = a + half;
                float br = data[2 * b], bi = data[2 * b + 1];
                float vr = br * wr - bi * wi;
                float vi = br * wi + bi * wr;
                float ar = data[2 * a], ai = data[2 * a + 1];
                data[2 * a] = ar + vr;
                data[2 * a + 1] = ai + vi;
                data[2 * b] = ar - vr;
                data[2 * b + 1] = ai - vi;
            }
        }
    }

    if (conjugated) {
        for (uint32_t k = 0; k < length; k++) data[2 * k + 1] = -data[2 * k + 1];
    }
}

void audioif_rfft_forward(const audioif_rfft_t *rfft, const float *input,
    float *spectrum, float *scratch) {
    uint32_t half = rfft->half;

    // Pack the n reals as half complex -- even samples real, odd imaginary --
    // and transform that. This is the whole reason a real transform costs
    // half of a complex one: the packing is free and the untangling below is
    // linear.
    for (uint32_t k = 0; k < half; k++) {
        scratch[2 * k] = input[2 * k];
        scratch[2 * k + 1] = input[2 * k + 1];
    }
    fft_complex(scratch, half, rfft->log2_half, rfft->twiddle, 0);

    // Split into the even- and odd-indexed subsequences' transforms and
    // recombine. Z[half] is Z[0] by periodicity, which the mask supplies.
    for (uint32_t k = 0; k < half; k++) {
        uint32_t mirror = (half - k) & (half - 1);
        float zr = scratch[2 * k], zi = scratch[2 * k + 1];
        float mr = scratch[2 * mirror], mi = scratch[2 * mirror + 1];
        float er = 0.5f * (zr + mr), ei = 0.5f * (zi - mi);
        // The odd part carries a 1/(2i), i.e. a swap and a sign.
        float or_ = 0.5f * (zi + mi), oi = -0.5f * (zr - mr);
        float wr = rfft->untangle[2 * k], wi = rfft->untangle[2 * k + 1];
        spectrum[2 * k] = er + (or_ * wr - oi * wi);
        spectrum[2 * k + 1] = ei + (or_ * wi + oi * wr);
    }
    // Bin 0 and bin half are both real; the second is the one the loop above
    // cannot reach, since its twiddle is exp(-i*pi) and its mirror is itself.
    spectrum[1] = 0.0f;
    spectrum[2 * half] = scratch[0] - scratch[1];
    spectrum[2 * half + 1] = 0.0f;
    spectrum[0] = scratch[0] + scratch[1];
}

void audioif_rfft_inverse(const audioif_rfft_t *rfft, const float *spectrum,
    float *output, float *scratch) {
    uint32_t half = rfft->half;
    float norm = 1.0f / (float)half;

    for (uint32_t k = 0; k < half; k++) {
        uint32_t mirror = half - k;   // spectrum has half+1 bins, so this is real
        float xr = spectrum[2 * k], xi = spectrum[2 * k + 1];
        float mr = spectrum[2 * mirror], mi = spectrum[2 * mirror + 1];
        float er = 0.5f * (xr + mr), ei = 0.5f * (xi - mi);
        float or_ = 0.5f * (xr - mr), oi = 0.5f * (xi + mi);
        // Undo the forward's twiddle (conjugate) and its 1/(2i) (multiply
        // by i), in that order.
        float wr = rfft->untangle[2 * k], wi = -rfft->untangle[2 * k + 1];
        float tr = or_ * wr - oi * wi;
        float ti = or_ * wi + oi * wr;
        scratch[2 * k] = er - ti;
        scratch[2 * k + 1] = ei + tr;
    }
    fft_complex(scratch, half, rfft->log2_half, rfft->twiddle, 1);

    for (uint32_t k = 0; k < half; k++) {
        output[2 * k] = scratch[2 * k] * norm;
        output[2 * k + 1] = scratch[2 * k + 1] * norm;
    }
}
