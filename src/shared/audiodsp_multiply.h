// Runtime-neutral sample-wise multiply: two audio streams multiplied
// together, which is ring modulation and, with a modulator that does not
// cross zero, amplitude modulation.
//
// New code -- not a CircuitPython port, and not from vstaudio either. Nothing
// upstream multiplies two *streams*: synthio rings a note against an
// oscillator, which cannot reach a microphone, a sampler, or the output of
// another effect. It is also the only way to modulate at audio rate on this
// palette, since an LFO-driven parameter updates once per block (about
// 187 Hz at 48 kHz) and a ring modulator wants hundreds of hertz.
//
// The pulling loop is not here, for the same reason it is not in
// audiodsp_dynamics.c: each runtime reaches its audio graph differently, so
// the bindings own the loop and call in with runs of frames.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

//: Frames one output block carries. The same 256 audiodynamics uses, so a
//: chain built out of both moves in one block size.
#define AUDIODSP_MULTIPLY_FRAMES 256u

typedef struct {
    uint32_t channel_count;
    // 0..32768: how much of the product replaces the dry signal. Held as a
    // Q15 integer rather than a float because the process loop is the hot
    // path and every other term in it is already an integer.
    int32_t mix;
} audiodsp_multiply_config_t;

void audiodsp_multiply_config_init(audiodsp_multiply_config_t *config);

void audiodsp_multiply_set_channel_count(audiodsp_multiply_config_t *config,
    uint32_t channel_count);

// Clamps to 0..1. Anything outside is a caller's arithmetic slipping rather
// than a request worth honouring.
void audiodsp_multiply_set_mix(audiodsp_multiply_config_t *config, float mix);

// out[] = a[] blended toward a[]*b[] by `mix`, over interleaved stereo
// frames. `out` may alias `a`; the arithmetic is sample by sample.
void audiodsp_multiply_process_s16(const audiodsp_multiply_config_t *config,
    int16_t *out, const int16_t *a, const int16_t *b, uint32_t frames);

// What to write when there is no modulator to multiply by: the signal,
// untouched, whatever `mix` says. A modulator that is absent or has run dry
// has to leave the signal alone rather than silence it -- this is the one
// place where "no input" and "an input of zero" must mean different things.
void audiodsp_multiply_passthrough_s16(int16_t *out, const int16_t *a,
    uint32_t frames);
