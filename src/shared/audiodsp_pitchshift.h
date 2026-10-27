// Runtime-neutral pitch-shift processing.
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t window_index;
    uint32_t overlap_index;
    uint32_t read_index;
    // The splice (docs/how-it-works/upstream-diff.md): while a crossfade runs, how many
    // frames the incoming side is moved to land in phase with the outgoing
    // one, and whether a crossfade is running. Both 0 where a new node
    // starts, and after a reset.
    uint32_t align;
    bool fading;
    // Which way the read was moving against the write when the crossfade
    // began, which says which side the move was applied to.
    bool fading_rising;
} audiodsp_pitchshift_positions_t;

// `align_span` is the furthest, in frames, a splice may move the incoming
// side: a 50th of the sample rate covers a whole period down to G1. 0 leaves
// every splice where upstream CircuitPython puts it.
void audiodsp_pitchshift_process_s16(int16_t *output, const int16_t *input,
    size_t sample_count, int16_t *window, uint32_t window_samples,
    int16_t *overlap, uint32_t overlap_samples, uint8_t channel_count,
    uint32_t read_rate, double mix, bool freeze, uint32_t align_span,
    audiodsp_pitchshift_positions_t *positions);
