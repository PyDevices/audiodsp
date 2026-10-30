// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_biquad.h"

#include "shared/audiodsp_synth_dsp.h"
#include "shared/audiodsp_trig.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Upstream CircuitPython keeps every coefficient in Q15 and accumulates the
// five products in an int32_t. That is the right trade on a microcontroller
// and it costs low frequencies, badly and silently: `b0` for a low-pass is
// (1 - cos W0)/2, which at 100 Hz / 48 kHz is 4.3e-5, or 1.4 in Q15 - so it
// rounds to 1 and the filter is no longer the one that was asked for. A
// LOW_PASS at 100 Hz used to return silence. See docs/how-it-works/upstream-diff.md.
//
// Two things are widened here, for two different reasons. The coefficients get
// as many fractional bits as they individually have room for (below), because
// they are what quantizes. The accumulator becomes an int64_t because it is
// what overflows: `a1` approaches -2, and in any format worth using its
// product with a full-scale sample is already most of an int32_t on its own.
#define AUDIODSP_BIQUAD_MAX_SHIFT 30

static double fast_sqrt(double input) {
    float number = (float)input;
    union { float f; uint32_t i; } value = {.f = number};
    value.i = 0x5f3759df - (value.i >> 1);
    value.f *= 1.5F - (number * 0.5F * value.f * value.f);
    return input * (double)value.f;
}

// The deterministic sine and cosine this needs -- and why they are not
// libm's, and why the reflection stops at pi rather than going all the way
// round -- now live in shared/audiodsp_trig.c, where the FFT can reach them
// too. audiodsp_sincos_reflect() is the arithmetic that used to be here,
// unchanged: the coefficient goldens are pinned to it.

static int32_t scale(double value, int shift) {
    double scaled = round(ldexp(value, shift));
    // choose_shift() leaves room for this by construction; the clamp is here
    // so a rounding tick at the very top of the range cannot wrap the sign.
    if (scaled > 2147483647.0) return INT32_MAX;
    if (scaled < -2147483648.0) return INT32_MIN;
    return (int32_t)scaled;
}

// The most fractional bits all five coefficients can share and still fit an
// int32_t. A plain low-pass tops out near 2 (that is `a1`) and so gets 29 of
// them; a 20 dB shelf reaches ~200 and gets 23. Fixing one format for every
// filter would mean giving them all the shelf's, which is where the precision
// that matters at low frequencies goes.
static int choose_shift(const double *values, size_t count) {
    double largest = 1.0;
    for (size_t i = 0; i < count; i++) {
        double magnitude = fabs(values[i]);
        if (magnitude > largest) largest = magnitude;
    }
    int exponent;
    frexp(largest, &exponent);  // largest < 2^exponent, and largest >= 1
    int shift = 31 - exponent;
    if (shift > AUDIODSP_BIQUAD_MAX_SHIFT) shift = AUDIODSP_BIQUAD_MAX_SHIFT;
    // Absurd A values could in principle ask for more than 31 integer bits;
    // the coefficients saturate rather than wrap, and the rounding term below
    // needs a shift of at least one.
    return shift < 1 ? 1 : shift;
}

void audiodsp_biquad_configure_w0(audiodsp_biquad_coefficients_t *coefficients,
    int mode, double W0, double Q, double A) {
    audiodsp_sincos_t sc;
    audiodsp_sincos_reflect(W0, &sc);
    double alpha = sc.s / (2 * Q);
    double a0, a1, a2, b0, b1, b2;
    if (mode < 4) {
        a0 = 1 + alpha; a1 = -2 * sc.c; a2 = 1 - alpha;
        if (mode == 0) {
            b2 = b0 = (1 - sc.c) * .5; b1 = 1 - sc.c;
        } else if (mode == 1) {
            b2 = b0 = (1 + sc.c) * .5; b1 = -(1 + sc.c);
        } else if (mode == 2) {
            b0 = alpha; b1 = 0; b2 = -b0;
        } else {
            b0 = 1; b1 = -2 * sc.c; b2 = 1;
        }
    } else if (mode == 4) {
        // b2 is `1 - alpha * A`, not `+`. Upstream CircuitPython has the plus
        // (shared-module/synthio/Biquad.c); see docs/how-it-works/upstream-diff.md. The sign
        // is what makes numerator and denominator sum alike at DC and Nyquist,
        // which is the whole point of a peaking filter: unity everywhere except
        // the band. With it flipped the numerator gains 2*alpha*A at DC while
        // the denominator does not, and since 1 - cos(W0) is tiny down there,
        // a +6 dB bell at 1 kHz / Q 1 comes out roughly +21 dB at DC.
        b0 = 1 + alpha * A; b1 = -2 * sc.c; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * sc.c; a2 = 1 - alpha / A;
    } else {
        double root = fast_sqrt(A);
        if (mode == 5) {
            b0 = A * ((A + 1) - (A - 1) * sc.c + 2 * root * alpha);
            b1 = 2 * A * ((A - 1) - (A + 1) * sc.c);
            b2 = A * ((A + 1) - (A - 1) * sc.c - 2 * root * alpha);
            a0 = (A + 1) + (A - 1) * sc.c + 2 * root * alpha;
            a1 = -2 * ((A - 1) + (A + 1) * sc.c);
            a2 = (A + 1) + (A - 1) * sc.c - 2 * root * alpha;
        } else {
            b0 = A * ((A + 1) + (A - 1) * sc.c + 2 * root * alpha);
            b1 = -2 * A * ((A - 1) + (A + 1) * sc.c);
            b2 = A * ((A + 1) + (A - 1) * sc.c - 2 * root * alpha);
            a0 = (A + 1) - (A - 1) * sc.c + 2 * root * alpha;
            a1 = 2 * ((A - 1) - (A + 1) * sc.c);
            a2 = (A + 1) - (A - 1) * sc.c - 2 * root * alpha;
        }
    }
    double reciprocal = 1 / a0;
    double values[5] = {a1 * reciprocal, a2 * reciprocal, b0 * reciprocal,
                        b1 * reciprocal, b2 * reciprocal};
    int shift = choose_shift(values, 5);
    coefficients->shift = shift;
    coefficients->a1 = scale(values[0], shift);
    coefficients->a2 = scale(values[1], shift);
    coefficients->b0 = scale(values[2], shift);
    coefficients->b1 = scale(values[3], shift);
    coefficients->b2 = scale(values[4], shift);
}

void audiodsp_biquad_configure(audiodsp_biquad_coefficients_t *coefficients,
    int mode, double frequency, double Q, double A, uint32_t sample_rate) {
    double W0 = frequency * (2 * AUDIODSP_PI) / sample_rate;
    audiodsp_biquad_configure_w0(coefficients, mode, W0, Q, A);
}

void audiodsp_biquad_reset(audiodsp_biquad_state_t *state) {
    // Upstream writes `memset(&st->x, 0, 4 * sizeof(int16_t))` over a struct
    // of four int32_t -- eight bytes of sixteen. It clears `x` and leaves `y`,
    // the feedback memory, so a filter that has been reset carries on
    // recursing on the previous audio: fed silence, it plays a decaying burst
    // of whatever was there before, at -1.3 dBFS for a low-pass. Both callers
    // (a note starting, and a chain's reset_buffer) mean a full reset.
    //
    // The fifth approved deviation from the oracle. Reported upstream --
    // docs/upstream-reports/biquad-reset.md.
    memset(state, 0, sizeof(*state));
}

// The output memory keeps AUDIODSP_BIQUAD_STATE_SHIFT bits below the sample
// grid, which matters more than it looks. A biquad low down has both poles
// close to the unit circle, and 1/A(z) - the gain the loop applies to whatever
// error is fed back into it - runs to four or five figures at DC: 4000 for a
// 100 Hz low-pass, 43000 for a 30 Hz high-pass. Rounding the feedback to whole
// samples, as upstream does, hands that gain a half-LSB of error to amplify.
//
// On the accumulator's headroom: a coefficient is under 2^31 by construction,
// `buffer` carries sample-range values (both callers feed it int16 material),
// and `y` is clamped to the sample range plus its extra bits - so the five
// terms come to about 2^60.4 worst case, six bits inside an int64_t. Stressed
// with rail-to-rail input across 315 mode/frequency/Q/A combinations, nothing
// wrapped.
void audiodsp_biquad_process(const audiodsp_biquad_coefficients_t *c,
    audiodsp_biquad_state_t *state, int32_t *buffer, size_t sample_count) {
    const int shift = c->shift;
    const int64_t rounding = (int64_t)1 << (shift - 1);
    const int64_t ceiling = (int64_t)32767 << AUDIODSP_BIQUAD_STATE_SHIFT;
    const int64_t floor_level = -((int64_t)32768 << AUDIODSP_BIQUAD_STATE_SHIFT);
    int32_t x0 = state->x[0], x1 = state->x[1];
    int32_t y0 = state->y[0], y1 = state->y[1];
    for (size_t i = 0; i < sample_count; i++) {
        int32_t input = buffer[i];
        int64_t accumulator =
            (((int64_t)c->b0 * input + (int64_t)c->b1 * x0 +
              (int64_t)c->b2 * x1) << AUDIODSP_BIQUAD_STATE_SHIFT) -
            (int64_t)c->a1 * y0 - (int64_t)c->a2 * y1;
        int64_t output = (accumulator + rounding) >> shift;
        if (output > ceiling) output = ceiling;
        else if (output < floor_level) output = floor_level;
        x1 = x0; x0 = input; y1 = y0; y0 = (int32_t)output;
        buffer[i] = (int32_t)((output + (1 << (AUDIODSP_BIQUAD_STATE_SHIFT - 1)))
            >> AUDIODSP_BIQUAD_STATE_SHIFT);
    }
    state->x[0] = x0; state->x[1] = x1;
    state->y[0] = y0; state->y[1] = y1;
}

// --- CircuitPython's own biquad --------------------------------------------
//
// See audiodsp_biquad.h for why this exists beside the widened kernel above:
// audiodsp#77, 2026-09-09. Upstream nodes run this so they render
// CircuitPython's bytes; `audiobiquad`, which is ours, runs the widened one.
//
// It is shared-module/synthio/Biquad.c as of CircuitPython main ae47a3f15d,
// which is ahead of the pinned 11.0.0-alpha.2 in two commits from
// adafruit/circuitpython#11679 (taken in upstream's words, ahead of the pin;
// docs/how-it-works/upstream-sync.md):
//
//   * 24546b7d1d: fast_sincos() reflects the angle into [0, pi/4] and
//     evaluates a minimax polynomial there, where it used to extrapolate one
//     fit over [0, pi/2] all the way to Nyquist.
//   * 0a7df85004: the coefficients have as many fractional bits as the
//     largest of them leaves room for (up to 30), the feedback state keeps
//     BIQUAD_STATE_SHIFT bits below the sample, the sum of products is an
//     int64_t, and the result rounds to nearest. The old Q15 sum rounded every
//     negative result one count high, so a 200 Hz shelf at A = 1.0 settled
//     371 counts off zero in silence (audiodsp#310).
//
// Both are departures from the pinned oracle until the pin moves to a release
// that contains them.
#define CP_BIQUAD_MAX_SHIFT (30)
#define CP_BIQUAD_STATE_SHIFT (12)
#define CP_BIQUAD_STATE_MAX (32767 * (1 << CP_BIQUAD_STATE_SHIFT))
#define CP_BIQUAD_STATE_MIN (-32768 * (1 << CP_BIQUAD_STATE_SHIFT))

typedef struct { double s, c; } cp_sincos_t;

// Sine and cosine of theta, evaluated together. theta is the filter's angle
// 2 * pi * f / sample_rate, which is in [0, pi] up to Nyquist.
//
// Reflect into [0, pi / 2] (sin(pi - t) = sin(t), cos(pi - t) = -cos(t)), then
// into [0, pi / 4] by swapping the two (sin(pi / 2 - t) = cos(t)). On that
// interval a four-term polynomial for each, fitted to minimise the worst-case
// error, is accurate to the last bit or two of a float.
static void cp_fast_sincos(double theta, cp_sincos_t *result) {
    bool negate_cos = theta > (AUDIODSP_PI / 2);
    double t = negate_cos ? (AUDIODSP_PI - theta) : theta;
    bool swap = t > (AUDIODSP_PI / 4);
    if (swap) {
        t = (AUDIODSP_PI / 2) - t;
    }
    double t2 = t * t;
    double s = t * (1 + t2 * (-0.16666651 + t2 * (0.0083319786 + t2 * -0.00019495635)));
    double c = 1 + t2 * (-0.5 + t2 * (0.041666647 + t2 * (-0.0013887368 + t2 * 0.000024438452)));
    double sin_theta = swap ? c : s;
    double cos_theta = swap ? s : c;
    result->s = sin_theta;
    result->c = negate_cos ? -cos_theta : cos_theta;
}

static int32_t cp_scale(double arg, int shift) {
    double scaled = round(ldexp(arg, shift));
    // cp_choose_shift() leaves room for this; the clamp is so that a
    // rounding tick at the very top of the range cannot wrap the sign.
    if (scaled >= 2147483648.0) {
        return INT32_MAX;
    }
    if (scaled <= -2147483648.0) {
        return INT32_MIN;
    }
    return (int32_t)scaled;
}

// The most fractional bits that all five coefficients can share and still fit
// in an int32_t, up to CP_BIQUAD_MAX_SHIFT. A plain low-pass tops out just
// under 2 (that is a1) and gets 30; a shelf with 24 dB of gain reaches almost
// 32 and gets 26.
static int cp_choose_shift(const double *coefficients, size_t count) {
    double largest = 1;
    for (size_t i = 0; i < count; i++) {
        double magnitude = fabs(coefficients[i]);
        if (magnitude > largest) {
            largest = magnitude;
        }
    }
    int exponent;
    frexp(largest, &exponent); // largest < 2 ** exponent
    int shift = 31 - exponent;
    if (shift > CP_BIQUAD_MAX_SHIFT) {
        shift = CP_BIQUAD_MAX_SHIFT;
    }
    // The rounding term in the filter needs at least one bit.
    return shift < 1 ? 1 : shift;
}

void audiodsp_biquad_cp_init(audiodsp_biquad_cp_coefficients_t *coefficients) {
    coefficients->a1 = coefficients->a2 = 0;
    coefficients->b1 = coefficients->b2 = 0;
    coefficients->shift = AUDIODSP_BIQUAD_CP_DEFAULT_SHIFT;
    coefficients->b0 = 1 << AUDIODSP_BIQUAD_CP_DEFAULT_SHIFT;
}

double audiodsp_biquad_cp_w0(double frequency, uint32_t sample_rate) {
    // A multiply by a reciprocal, matching synthio_global_W_scale exactly.
    // `frequency * (2 * PI) / sample_rate` differs in the last bits, and
    // rounding the coefficients turns that into different ones.
    double reciprocal = 1.0 / sample_rate;
    return frequency * ((2 * AUDIODSP_PI) * reciprocal);
}

void audiodsp_biquad_cp_configure(audiodsp_biquad_cp_coefficients_t *coefficients,
    int mode, double W0, double Q, double A) {
    cp_sincos_t sc;
    cp_fast_sincos(W0, &sc);
    double alpha = sc.s / (2 * Q);
    double a0, a1, a2, b0, b1, b2;
    if (mode < 4) {
        a0 = 1 + alpha; a1 = -2 * sc.c; a2 = 1 - alpha;
        if (mode == 0) {
            b2 = b0 = (1 - sc.c) * .5; b1 = 1 - sc.c;
        } else if (mode == 1) {
            b2 = b0 = (1 + sc.c) * .5; b1 = -(1 + sc.c);
        } else if (mode == 2) {
            b0 = alpha; b1 = 0; b2 = -b0;
        } else {
            b0 = 1; b1 = -2 * sc.c; b2 = 1;
        }
    } else if (mode == 4) {
        b0 = 1 + alpha * A; b1 = -2 * sc.c; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * sc.c; a2 = 1 - alpha / A;
    } else {
        double root = fast_sqrt(A);
        if (mode == 5) {
            b0 = A * ((A + 1) - (A - 1) * sc.c + 2 * root * alpha);
            b1 = 2 * A * ((A - 1) - (A + 1) * sc.c);
            b2 = A * ((A + 1) - (A - 1) * sc.c - 2 * root * alpha);
            a0 = (A + 1) + (A - 1) * sc.c + 2 * root * alpha;
            a1 = -2 * ((A - 1) + (A + 1) * sc.c);
            a2 = (A + 1) + (A - 1) * sc.c - 2 * root * alpha;
        } else {
            b0 = A * ((A + 1) + (A - 1) * sc.c + 2 * root * alpha);
            b1 = -2 * A * ((A - 1) + (A + 1) * sc.c);
            b2 = A * ((A + 1) + (A - 1) * sc.c - 2 * root * alpha);
            a0 = (A + 1) - (A - 1) * sc.c + 2 * root * alpha;
            a1 = 2 * ((A - 1) - (A + 1) * sc.c);
            a2 = (A + 1) - (A - 1) * sc.c - 2 * root * alpha;
        }
    }
    double recip_a0 = 1 / a0;

    a1 *= recip_a0;
    a2 *= recip_a0;
    b0 *= recip_a0;
    b1 *= recip_a0;
    b2 *= recip_a0;

    double values[] = {a1, a2, b0, b1, b2};
    int shift = cp_choose_shift(values, sizeof(values) / sizeof(values[0]));
    coefficients->shift = shift;
    coefficients->a1 = cp_scale(a1, shift);
    coefficients->a2 = cp_scale(a2, shift);
    coefficients->b0 = cp_scale(b0, shift);
    coefficients->b1 = cp_scale(b1, shift);
    coefficients->b2 = cp_scale(b2, shift);
}

// Returns the new feedback state: the output with CP_BIQUAD_STATE_SHIFT bits
// below the sample. Near 0 Hz a biquad turns each unit of error in its
// feedback into thousands of units of output, so the feedback must not be
// rounded to whole samples. The coefficients can have up to 30 fractional
// bits, so the sum of products needs 64 bits.
static inline int32_t cp_filter_sample(int32_t input, int32_t a1, int32_t a2,
    int32_t b0, int32_t b1, int32_t b2, int shift,
    int32_t x0, int32_t x1, int32_t y0, int32_t y1) {
    int64_t acc = ((int64_t)b0 * input + (int64_t)b1 * x0 + (int64_t)b2 * x1) * (1 << CP_BIQUAD_STATE_SHIFT)
        - (int64_t)a1 * y0 - (int64_t)a2 * y1;
    // Add half a step, then shift, to round to nearest.
    acc = (acc + ((int64_t)1 << (shift - 1))) >> shift;
    if (acc > CP_BIQUAD_STATE_MAX) {
        return CP_BIQUAD_STATE_MAX;
    }
    if (acc < CP_BIQUAD_STATE_MIN) {
        return CP_BIQUAD_STATE_MIN;
    }
    return (int32_t)acc;
}

// The state is already within the range of a sample, so rounding is all it needs.
static inline int32_t cp_state_to_sample(int32_t y) {
    return (y + (1 << (CP_BIQUAD_STATE_SHIFT - 1))) >> CP_BIQUAD_STATE_SHIFT;
}

int32_t audiodsp_biquad_cp_sample(
    const audiodsp_biquad_cp_coefficients_t *coefficients,
    audiodsp_biquad_state_t *state, int32_t input) {
    int32_t y = cp_filter_sample(input,
        coefficients->a1, coefficients->a2, coefficients->b0,
        coefficients->b1, coefficients->b2, coefficients->shift,
        state->x[0], state->x[1], state->y[0], state->y[1]);
    state->x[1] = state->x[0];
    state->x[0] = input;
    state->y[1] = state->y[0];
    state->y[0] = y;
    return cp_state_to_sample(y);
}

void audiodsp_biquad_cp_process(
    const audiodsp_biquad_cp_coefficients_t *coefficients,
    audiodsp_biquad_state_t *state, int32_t *buffer, size_t sample_count) {
    int32_t a1 = coefficients->a1, a2 = coefficients->a2;
    int32_t b0 = coefficients->b0, b1 = coefficients->b1, b2 = coefficients->b2;
    int shift = coefficients->shift;
    int32_t x0 = state->x[0], x1 = state->x[1];
    int32_t y0 = state->y[0], y1 = state->y[1];
    for (size_t n = sample_count; n; --n, ++buffer) {
        int32_t input = *buffer;
        int32_t y = cp_filter_sample(input, a1, a2, b0, b1, b2, shift,
            x0, x1, y0, y1);
        x1 = x0; x0 = input;
        y1 = y0; y0 = y;
        *buffer = cp_state_to_sample(y);
    }
    state->x[0] = x0; state->x[1] = x1;
    state->y[0] = y0; state->y[1] = y1;
}
