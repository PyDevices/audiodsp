// The sample-and-hold kernel. See audiodsp_samplehold.h for why it exists and
// why the accumulator is a pair of integers rather than a finer fixed point.
//
// No `shared/audiodsp_fp_contract.h` here, deliberately: there is no float in
// this file at all. The contraction rule (audiodsp#79) is about a compiler
// choosing whether to round a multiply-add once or twice, and integer
// arithmetic gives it nothing to choose. Adding a float to this file means
// adding the include with it.
//
// SPDX-License-Identifier: MIT

#include "shared/audiodsp_samplehold.h"

#include <string.h>

static uint32_t samplehold_gcd(uint32_t left, uint32_t right) {
    while (right != 0u) {
        uint32_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

bool audiodsp_samplehold_ratio_ok(uint32_t num, uint32_t den) {
    if (num < 1u || den < 1u) {
        return false;
    }
    if (den > num) {
        return false;
    }
    return num <= AUDIODSP_SAMPLEHOLD_MAX_RATIO;
}

void audiodsp_samplehold_config_init(audiodsp_samplehold_config_t *config,
    uint32_t num, uint32_t den) {
    if (!audiodsp_samplehold_ratio_ok(num, den)) {
        num = 1u;
        den = 1u;
    }
    uint32_t divisor = samplehold_gcd(num, den);
    config->num = num / divisor;
    config->den = den / divisor;
}

bool audiodsp_samplehold_config_set(audiodsp_samplehold_config_t *config,
    uint32_t num, uint32_t den) {
    const uint32_t was_num = config->num;
    const uint32_t was_den = config->den;
    audiodsp_samplehold_config_init(config, num, den);
    return config->num != was_num || config->den != was_den;
}

void audiodsp_samplehold_reset(audiodsp_samplehold_state_t *state,
    const audiodsp_samplehold_config_t *config) {
    // Armed one step short of a wrap, so the first frame of a fresh stream
    // latches rather than emitting whatever `held` happens to hold. It is also
    // what makes the refresh count 1 + floor((N-1)*den/num) over N frames --
    // exactly N*den/num over any whole number of periods.
    state->phase = config->num - config->den;
    memset(state->held, 0, sizeof(state->held));
}

void audiodsp_samplehold_state_init(audiodsp_samplehold_state_t *state,
    const audiodsp_samplehold_config_t *config) {
    audiodsp_samplehold_reset(state, config);
}

void audiodsp_samplehold_process(const audiodsp_samplehold_config_t *config,
    audiodsp_samplehold_state_t *state, uint8_t *out, const uint8_t *in,
    uint32_t frames, uint32_t frame_bytes) {
    const uint32_t num = config->num;
    const uint32_t den = config->den;
    uint32_t phase = state->phase;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        phase += den;
        if (phase >= num) {
            // One subtraction is enough for the whole life of the node:
            // `phase` is below `num` on entry and `den` is at most `num`, so
            // the sum cannot reach twice `num`.
            phase -= num;
            memcpy(state->held, in, frame_bytes);
        }
        memcpy(out, state->held, frame_bytes);
        in += frame_bytes;
        out += frame_bytes;
    }
    state->phase = phase;
}
