// Deterministic sine and cosine, shared by everything that needs a trig
// value it can be held to across interpreters.
//
// Not libm, deliberately. glibc, newlib and MicroPython's own sin/cos agree
// to within an ulp and differ in the last place, and this port's parity rule
// is that one hash of one probe matches on CPython, MicroPython and
// CircuitPython. A polynomial evaluated in the same order is the same
// function everywhere; `sin()` is three functions that nearly agree.
//
// Extracted from audiodsp_biquad.c during the convolution work, because the
// FFT's twiddle factors need the same guarantee for the same reason. The
// biquad's entry point keeps its exact previous arithmetic -- see
// audiodsp_sincos_reflect() -- so no filter coefficient moves by extracting it.
//
// SPDX-License-Identifier: MIT

#pragma once

#define AUDIODSP_PI 3.14159265358979323846

typedef struct { double s, c; } audiodsp_sincos_t;

// The two Taylor series, seven terms apiece, over [0, pi/2]. Worst error
// across that quarter is about 7e-9, against the ~5e-6 of the single
// polynomial CircuitPython fits to both functions at once. Outside the
// quarter it degrades quickly; the callers below are what keep it inside.
void audiodsp_sincos_quarter(double x, audiodsp_sincos_t *result);

// Reflects about pi/2 only: sin(pi - t) = sin t, cos(pi - t) = -cos t. Good
// over [0, pi], which is every angle a biquad can ask for at or below
// Nyquist, and above Nyquist it stays merely wrong rather than absurd.
//
// This is the biquad's historical behaviour, kept exactly. Do not "fix" it to
// full-circle reduction: a frequency above Nyquist would then get a
// *different* wrong answer, and several goldens are pinned to this one.
void audiodsp_sincos_reflect(double theta, audiodsp_sincos_t *result);

// Full circle. Reduces into [0, pi/2] by quadrant, with the signs put back,
// so any angle at all is accurate. This is what the FFT wants: a twiddle
// table walks the whole rotation, not half of it.
void audiodsp_sincos(double theta, audiodsp_sincos_t *result);
