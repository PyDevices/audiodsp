// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_trig.h"

#include <float.h>
#include <stdbool.h>

void audiodsp_sincos_quarter(double x, audiodsp_sincos_t *result) {
    static const double sine_terms[7] = {
        1.0, -1.0 / 6, 1.0 / 120, -1.0 / 5040,
        1.0 / 362880, -1.0 / 39916800, 1.0 / 6227020800.0,
    };
    static const double cosine_terms[7] = {
        1.0, -1.0 / 2, 1.0 / 24, -1.0 / 720,
        1.0 / 40320, -1.0 / 3628800, 1.0 / 479001600.0,
    };
    double x2 = x * x, s = 0.0, c = 0.0;
    for (int term = 6; term >= 0; term--) {
        s = s * x2 + sine_terms[term];
        c = c * x2 + cosine_terms[term];
    }
    result->s = s * x;
    result->c = c;
}

void audiodsp_sincos_reflect(double theta, audiodsp_sincos_t *result) {
    bool reflected = theta > AUDIODSP_PI / 2;
    audiodsp_sincos_quarter(reflected ? AUDIODSP_PI - theta : theta, result);
    if (reflected) result->c = -result->c;
}

// fmod(x, y) for y > 0, to the bit, without calling libm.
//
// Some builds have no double-precision fmod to call. CircuitPython's RP2040
// port links with the pico-sdk's --wrap=fmod but compiles none of the SDK's
// double math, so a call to fmod has nothing to resolve to and the firmware
// does not link.
//
// The result is the one fmod gives, because fmod's result is exact: it is
// x - n*y with nothing rounded. This takes it the same way, by long division
// in binary. Each step subtracts the largest y*2^k that fits. Doubling and
// halving a double are exact, and a - m is exact whenever m <= a < 2m
// (Sterbenz), so nothing is ever rounded here either. The loop runs about
// twice per binary order of magnitude between x and y, so a large angle costs
// a few dozen steps and an angle already in range costs none.
static double audiodsp_fmod_positive(double x, double y) {
    double a = x < 0 ? -x : x;
    if (!(a <= DBL_MAX)) {
        return x - x;  // inf or NaN: NaN, as fmod returns
    }
    if (a >= y) {
        double m = y;
        while (m * 2.0 <= a) {
            m *= 2.0;
        }
        while (a >= y) {
            while (m > a) {
                m *= 0.5;
            }
            a -= m;
        }
    }
    return x < 0 ? -a : a;
}

void audiodsp_sincos(double theta, audiodsp_sincos_t *result) {
    // Reduce into [0, 2pi). A remainder rather than a subtraction loop: a
    // twiddle table for a large transform walks a long way round, and the
    // loop's cost would grow with the angle while its accuracy fell.
    double turns = 2 * AUDIODSP_PI;
    theta = audiodsp_fmod_positive(theta, turns);
    if (theta < 0) theta += turns;

    // Quadrant, then the signs. The comparisons are against exact multiples
    // of the reduced angle rather than against a running counter so that a
    // value sitting exactly on an axis lands in the branch whose subtraction
    // gives it zero -- sin(pi) comes out as sin(0), not as the series
    // evaluated at 1e-16.
    if (theta <= AUDIODSP_PI / 2) {
        audiodsp_sincos_quarter(theta, result);
    } else if (theta <= AUDIODSP_PI) {
        audiodsp_sincos_quarter(AUDIODSP_PI - theta, result);
        result->c = -result->c;
    } else if (theta <= 3 * AUDIODSP_PI / 2) {
        audiodsp_sincos_quarter(theta - AUDIODSP_PI, result);
        result->s = -result->s;
        result->c = -result->c;
    } else {
        audiodsp_sincos_quarter(turns - theta, result);
        result->s = -result->s;
    }
}
