// TensorFlow Lite Micro's audio microfrontend, compiled for every audiodsp
// target in this one translation unit.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// The library lives unchanged in third_party/microfrontend, and the FFT it
// calls, kissfft, in third_party/kissfft (each with an ORIGIN.txt). They are
// included here rather than listed in each build, as Tremor is in
// audiodsp_vorbis.c, for the same reasons: one file for every build system to
// add, every external symbol renamed (audiodsp_microfrontend_names.h) so a
// firmware that links another copy still links, and the library's warnings
// silenced for its code only.
//
// Upstream builds its FFT from three C++ files (fft.cc, fft_util.cc and
// kiss_fft_int16.cc), which put kissfft in a namespace and compile it as
// 16-bit fixed point. They are carried in third_party but not compiled: this
// file is their C equivalent, statement for statement, with kissfft compiled
// as FIXED_POINT 16 the way kiss_fft_int16.cc does it, so the arithmetic is
// the same and no target needs a C++ compiler. The only difference is that
// the failure messages fft_util.cc prints are left out.
//
// The build adds third_party/microfrontend and third_party/kissfft to the
// include path, because the library names its headers by their full
// "tensorflow/lite/..." path.

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shared/audiodsp_microfrontend_names.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wold-style-definition"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wmissing-declarations"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wfloat-conversion"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wfloat-equal"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wbad-function-cast"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif

// --- kissfft, 16-bit fixed point (kiss_fft_int16.cc) -----------------------

#define FIXED_POINT 16
#include "kiss_fft.c"
#include "tools/kiss_fftr.c"

// --- the FFT stage (fft.cc and fft_util.cc) ---------------------------------

#include "tensorflow/lite/experimental/microfrontend/lib/fft.h"
#include "tensorflow/lite/experimental/microfrontend/lib/fft_util.h"

void FftCompute(struct FftState *state, const int16_t *input, int input_scale_shift) {
    const size_t input_size = state->input_size;
    const size_t fft_size = state->fft_size;
    int16_t *fft_input = state->input;
    // First, scale the input by the given shift.
    size_t i;
    for (i = 0; i < input_size; ++i) {
        fft_input[i] = (int16_t)((uint16_t)input[i] << input_scale_shift);
    }
    // Zero out whatever else remains in the top part of the input.
    for (; i < fft_size; ++i) {
        fft_input[i] = 0;
    }
    // Apply the FFT.
    kiss_fftr((kiss_fftr_cfg)state->scratch, state->input, (kiss_fft_cpx *)state->output);
}

void FftInit(struct FftState *state) {
    // All the initialization is done in FftPopulateState().
    (void)state;
}

void FftReset(struct FftState *state) {
    memset(state->input, 0, state->fft_size * sizeof(*state->input));
    memset(state->output, 0, (state->fft_size / 2 + 1) * sizeof(*state->output));
}

int FftPopulateState(struct FftState *state, size_t input_size) {
    state->input_size = input_size;
    state->fft_size = 1;
    while (state->fft_size < state->input_size) {
        state->fft_size <<= 1;
    }
    state->input = (int16_t *)malloc(state->fft_size * sizeof(*state->input));
    if (state->input == NULL) {
        return 0;
    }
    state->output = (struct complex_int16_t *)malloc((state->fft_size / 2 + 1) * sizeof(*state->output) * 2);
    if (state->output == NULL) {
        return 0;
    }
    // Ask kissfft how much memory it wants.
    size_t scratch_size = 0;
    kiss_fftr_cfg kfft_cfg = kiss_fftr_alloc((int)state->fft_size, 0, NULL, &scratch_size);
    if (kfft_cfg != NULL) {
        return 0;
    }
    state->scratch = malloc(scratch_size);
    if (state->scratch == NULL) {
        return 0;
    }
    state->scratch_size = scratch_size;
    // Let kissfft configure the scratch space just allocated.
    kfft_cfg = kiss_fftr_alloc((int)state->fft_size, 0, state->scratch, &scratch_size);
    if (kfft_cfg != state->scratch) {
        return 0;
    }
    return 1;
}

void FftFreeStateContents(struct FftState *state) {
    free(state->input);
    free(state->output);
    free(state->scratch);
}

// --- the rest of the library, as it is ---------------------------------------

#include "tensorflow/lite/experimental/microfrontend/lib/filterbank.c"
#include "tensorflow/lite/experimental/microfrontend/lib/filterbank_util.c"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.c"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.c"
#include "tensorflow/lite/experimental/microfrontend/lib/log_lut.c"
#include "tensorflow/lite/experimental/microfrontend/lib/log_scale.c"
#include "tensorflow/lite/experimental/microfrontend/lib/log_scale_util.c"
#include "tensorflow/lite/experimental/microfrontend/lib/noise_reduction.c"
#include "tensorflow/lite/experimental/microfrontend/lib/noise_reduction_util.c"
#include "tensorflow/lite/experimental/microfrontend/lib/pcan_gain_control.c"
#include "tensorflow/lite/experimental/microfrontend/lib/pcan_gain_control_util.c"
#include "tensorflow/lite/experimental/microfrontend/lib/window.c"
#include "tensorflow/lite/experimental/microfrontend/lib/window_util.c"

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
