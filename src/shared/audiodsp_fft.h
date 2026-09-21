// Real-input FFT, in float, with the twiddle tables handed in rather than
// allocated. New code -- nothing upstream in CircuitPython transforms
// anything, and vstaudio never did either.
//
// Everything here exists to serve audiodsp_convolve.c, so the shape is the
// one partitioned convolution wants: real blocks of N samples in, N/2 + 1
// complex bins out, and back. It is a plain radix-2 Cooley-Tukey with a
// bit-reversal pass; there is no attempt at a split-radix or a hand-unrolled
// first stage, because the honest bottleneck in a convolver is the pointwise
// multiply across the partitions, not the two transforms either side of it.
//
// **Float, not double.** A double transform would double the memory of every
// stored partition, which is the thing that decides whether an impulse
// response fits on a microcontroller at all. float32 gives about 7 digits and
// the transform's error grows as sqrt(log2 N), so a 512-point transform of
// int16 audio lands ~1e-3 out of a full-scale 32768 -- five orders of
// magnitude below the samples it is made of.
//
// **The twiddles come from audiodsp_sincos(), not libm**, for the reason given
// in audiodsp_trig.h: a golden hash of a probe has to match on all three
// interpreters, and three libms do not agree in the last place. They are
// computed once, at init.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

// A real transform of length `n`, built on a complex transform of length
// n/2. `n` must be a power of two, at least 4.
typedef struct {
    uint32_t n;          // real samples per block
    uint32_t half;       // n / 2, the complex transform's length
    uint32_t log2_half;
    // half/2 complex, interleaved: exp(-2*pi*i*k/half). The complex
    // butterflies index this with a per-stage stride.
    const float *twiddle;
    // half complex, interleaved: exp(-2*pi*i*k/n). Recombines the complex
    // transform's output into the real one's. Half-angle to the table above,
    // which is why it cannot be shared with it.
    const float *untangle;
} audiodsp_rfft_t;

// Floats a caller must provide for `audiodsp_rfft_init` to fill: 3 * (n / 2).
uint32_t audiodsp_rfft_table_floats(uint32_t n);

// Fills `tables` (see above) and points `rfft` at it. `tables` must outlive
// `rfft`; nothing here allocates or frees.
void audiodsp_rfft_init(audiodsp_rfft_t *rfft, uint32_t n, float *tables);

// `input`: n real samples. `spectrum`: (n/2 + 1) complex, interleaved re,im.
// Bin 0 and bin n/2 are real; their imaginary parts are written as zero
// rather than packed together, because a convolver multiplies bins in a loop
// and a packed pair would need that loop to special-case its own ends.
//
// `scratch`: n floats, clobbered.
void audiodsp_rfft_forward(const audiodsp_rfft_t *rfft, const float *input,
    float *spectrum, float *scratch);

// The inverse of the above, scaled so that inverse(forward(x)) == x.
// `spectrum` is not modified. `scratch`: n floats, clobbered.
void audiodsp_rfft_inverse(const audiodsp_rfft_t *rfft, const float *spectrum,
    float *output, float *scratch);
