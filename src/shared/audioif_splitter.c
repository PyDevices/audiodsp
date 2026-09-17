// Runtime-neutral signal splitter. See audioif_splitter.h for provenance.
// SPDX-License-Identifier: MIT

#include "shared/audioif_splitter.h"

#include <string.h>

void audioif_splitter_init(audioif_splitter_state_t *state,
    uint32_t tap_count) {
    state->tap_count = tap_count;
    state->channel_count = 2u;
    state->write_pos = 0;
    for (uint32_t index = 0; index < AUDIOIF_SPLITTER_MAX_TAPS; ++index) {
        state->read_pos[index] = 0;
    }
    memset(state->ring, 0, sizeof(state->ring));
}

void audioif_splitter_set_channel_count(audioif_splitter_state_t *state,
    uint32_t channel_count) {
    state->channel_count = channel_count == 1u ? 1u : 2u;
}

uint32_t audioif_splitter_write(audioif_splitter_state_t *state,
    const int16_t *frames, uint32_t count) {
    // One ring at most. See the header for why the caller keeps the rest.
    if (count > AUDIOIF_SPLITTER_RING_FRAMES) {
        count = AUDIOIF_SPLITTER_RING_FRAMES;
    }
    const uint32_t taken = count;
    while (count-- != 0) {
        const uint32_t at =
            (state->write_pos % AUDIOIF_SPLITTER_RING_FRAMES) * 2u;
        state->ring[at] = frames[0];
        state->ring[at + 1u] = state->channel_count == 1u
            ? frames[0] : frames[1];
        frames += state->channel_count;
        ++state->write_pos;
        for (uint32_t tap = 0; tap < state->tap_count; ++tap) {
            if (state->write_pos - state->read_pos[tap] >
                AUDIOIF_SPLITTER_RING_FRAMES) {
                ++state->read_pos[tap];   // an unread tap must not wedge the ring
            }
        }
    }
    return taken;
}

uint32_t audioif_splitter_take(audioif_splitter_state_t *state, uint32_t tap,
    uint32_t *start_frame) {
    const uint32_t available = state->write_pos - state->read_pos[tap];
    if (available == 0) {
        *start_frame = 0;
        return 0;
    }
    const uint32_t start = state->read_pos[tap] % AUDIOIF_SPLITTER_RING_FRAMES;
    uint32_t run = AUDIOIF_SPLITTER_RING_FRAMES - start;
    if (run > available) {
        run = available;
    }
    if (run > AUDIOIF_SPLITTER_CHUNK_FRAMES) {
        run = AUDIOIF_SPLITTER_CHUNK_FRAMES;
    }
    *start_frame = start;
    state->read_pos[tap] += run;
    return run;
}
