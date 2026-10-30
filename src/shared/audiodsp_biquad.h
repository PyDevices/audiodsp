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

// --- CircuitPython's own biquad --------------------------------------------
//
// The kernel above is a deliberate improvement on this one and is used only by
// `audiobiquad`, which is ours (audiodsp#77, 2026-09-09). Anything
// CircuitPython also has -- `synthio.Biquad`, and so `audiofilters.Filter` and
// a `Note.filter` chain -- runs THIS, so it renders CircuitPython's bytes.
//
// Arithmetic-identical to shared-module/synthio/Biquad.c at CircuitPython main
// ae47a3f15d, expression order included: the reflected `fast_sincos`, the
// Quake reciprocal square root for the shelves, coefficients with a per-filter
// number of fractional bits rounded with `round(ldexp(v, shift))`, and an
// int64_t sum into a feedback state with 12 bits below the sample. Since that
// change it differs from audiobiquad's kernel in its sine and cosine and in how
// W0 is derived (audiodsp_biquad_cp_w0() below), both upstream's and kept so. See docs/how-it-works/upstream-diff.md, "The biquads were
// Q15, so they could not go low".
#define AUDIODSP_BIQUAD_CP_DEFAULT_SHIFT 15

typedef struct {
    int32_t a1, a2, b0, b1, b2;
    // Fractional bits in the five coefficients above, chosen per filter.
    int32_t shift;
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
