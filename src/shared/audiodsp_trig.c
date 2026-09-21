// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_trig.h"

#include <math.h>
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

void audiodsp_sincos(double theta, audiodsp_sincos_t *result) {
    // Reduce into [0, 2pi). fmod rather than a subtraction loop: a twiddle
    // table for a large transform walks a long way round, and the loop's cost
    // would grow with the angle while its accuracy fell.
    double turns = 2 * AUDIODSP_PI;
    theta = fmod(theta, turns);
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
