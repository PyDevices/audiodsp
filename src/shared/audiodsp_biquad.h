// Runtime-neutral synth biquad coefficient and sample processing.
// SPDX-License-Identifier: MIT

#pragma once


#include <stddef.h>
#include <stdint.h>

// How many fractional bits the filter's output memory keeps beyond the 16 the
// samples themselves carry. See audiodsp_biquad.c for why the feedback path
// needs them; anything that stores a `y` value out of a state struct has to
// know it is not in sample units.
#define AUDIODSP_BIQUAD_STATE_SHIFT 12

typedef struct {
    int32_t a1, a2, b0, b1, b2;
    // Fractional bits in the five coefficients above, chosen per filter.
    int32_t shift;
} audiodsp_biquad_coefficients_t;

typedef struct {
    // `x` is in sample units; `y` carries AUDIODSP_BIQUAD_STATE_SHIFT extra
    // fractional bits.
    int32_t x[2], y[2];
} audiodsp_biquad_state_t;

void audiodsp_biquad_configure(audiodsp_biquad_coefficients_t *coefficients,
    int mode, double frequency, double Q, double A, uint32_t sample_rate);
void audiodsp_biquad_configure_w0(audiodsp_biquad_coefficients_t *coefficients,
    int mode, double W0, double Q, double A);
void audiodsp_biquad_reset(audiodsp_biquad_state_t *state);
void audiodsp_biquad_process(const audiodsp_biquad_coefficients_t *coefficients,
    audiodsp_biquad_state_t *state, int32_t *buffer, size_t sample_count);

// --- CircuitPython's own Q15 biquad ---------------------------------------
//
// The kernel above is a deliberate improvement on this one and is used only by
// `audiobiquad`, which is ours (audiodsp#77, Brad 2026-09-09). Anything
// CircuitPython also has -- `synthio.Biquad`, and so `audiofilters.Filter` and
// a `Note.filter` chain -- runs THIS, so it renders CircuitPython's bytes.
//
// Arithmetic-identical to shared-module/synthio/Biquad.c at 10.3.0, expression
// order included: `fast_sincos`'s 5th-order fit, the Quake reciprocal square
// root for the shelves, Q15 coefficients rounded with `round(ldexp(v, 15))`,
// and an int32_t accumulator saturated to int16 each step. Its known costs are
// upstream's and are not corrected here -- that is the point of it. They are
// measured in docs/upstream-diff.md, "The biquads were Q15, so they could not
// go low".
#define AUDIODSP_BIQUAD_CP_SHIFT 15

typedef struct {
    int32_t a1, a2, b0, b1, b2;
} audiodsp_biquad_cp_coefficients_t;

// A pass-through, for a filter that reaches the DSP before its first tick.
void audiodsp_biquad_cp_init(audiodsp_biquad_cp_coefficients_t *coefficients);
// `W0` is the pre-scaled angular frequency. Callers must derive it the way
// CircuitPython does -- `frequency * ((2 * PI) * (1.0 / sample_rate))`, a
// multiply by a reciprocal -- because `frequency * (2 * PI) / sample_rate`
// differs in the last bits and the coefficients are rounded.
// `audiodsp_biquad_cp_w0()` is that derivation, so no caller has to remember it.
double audiodsp_biquad_cp_w0(double frequency, uint32_t sample_rate);
void audiodsp_biquad_cp_configure(audiodsp_biquad_cp_coefficients_t *coefficients,
    int mode, double W0, double Q, double A);
int32_t audiodsp_biquad_cp_sample(
    const audiodsp_biquad_cp_coefficients_t *coefficients,
    audiodsp_biquad_state_t *state, int32_t input);
void audiodsp_biquad_cp_process(
    const audiodsp_biquad_cp_coefficients_t *coefficients,
    audiodsp_biquad_state_t *state, int32_t *buffer, size_t sample_count);
