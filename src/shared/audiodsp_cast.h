// Runtime-neutral float block to s16: scaled, rounded and saturated.
//
// New code -- not a CircuitPython port. A host that builds its blocks in
// float (a ulab or numpy array) has to turn each one into s16 before a
// RawSample can hand it to a node. Array libraries do that through a
// temporary the size of the block; this writes straight into the caller's
// buffer, so a block path can convert without allocating.
//
// Each sample is src[i] * scale, rounded half away from zero (C's round(),
// which is also what ulab's float-to-int cast uses), then clamped to
// [-32768, 32767]. NaN becomes 0. The product is taken at the source's own
// width, float for float32 and double for float64, so it is the same number
// an array library's `block * scale` would have made.
//
// dst may alias the start of src: dst[i] never reaches a float the loop has
// still to read.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

void audiodsp_cast_s16_f32(int16_t *dst, const float *src, size_t count,
    float scale);
void audiodsp_cast_s16_f64(int16_t *dst, const double *src, size_t count,
    double scale);
