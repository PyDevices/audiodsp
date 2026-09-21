// Runtime-neutral signal splitter: fans one audio stream out to several taps
// with independent read cursors over a shared ring, so one source can feed
// parallel branches (an exciter, a Haas widener, multiband splits) that a
// Mixer then sums back together.
//
// Ported from micropython-vst3's usermods/vstaudio/vstaudio_dsp.c.
//
// Whichever tap is pulled first refills the ring; the others read what it
// wrote. A tap nobody reads must not wedge the ring, so writing past a
// laggard's cursor drags that cursor forward and silently drops what it never
// collected -- the branch skips ahead rather than stalling the graph.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AUDIODSP_SPLITTER_RING_FRAMES 8192u
#define AUDIODSP_SPLITTER_MAX_TAPS 4u
#define AUDIODSP_SPLITTER_CHUNK_FRAMES 256u

typedef struct {
    uint32_t tap_count;
    uint32_t channel_count;
    uint32_t write_pos;
    uint32_t read_pos[AUDIODSP_SPLITTER_MAX_TAPS];
    int16_t ring[AUDIODSP_SPLITTER_RING_FRAMES * 2];
} audiodsp_splitter_state_t;

void audiodsp_splitter_init(audiodsp_splitter_state_t *state, uint32_t tap_count);

void audiodsp_splitter_set_channel_count(audiodsp_splitter_state_t *state,
    uint32_t channel_count);

// Append interleaved stereo frames, dragging any cursor the write laps.
//
// Returns HOW MANY FRAMES IT TOOK, which is `count` capped at one ring. The
// caller keeps the rest and offers it on the next call rather than letting it
// be written over unread: a source is entitled to hand back more than the ring
// holds in one go -- a `RawSample` over a table returns the whole table -- and
// writing 9600 frames into an 8192-frame ring laps every cursor, including the
// cursor of the tap that is about to read. The head of the block is destroyed
// before anyone sees it and the stream has a seam at 8192. audiodsp#87.
//
// The cap is not the same thing as the lapping above it. Dragging a LAGGARD
// forward is deliberate: a tap nobody reads must not wedge the ring, and what
// it loses it was never going to collect. Lapping the tap that is pulling is
// not deliberate; that is data loss on the live branch.
uint32_t audiodsp_splitter_write(audiodsp_splitter_state_t *state,
    const int16_t *frames, uint32_t count);

// True when `tap` has read everything written -- the caller's cue to pull the
// source for more.
static inline bool audiodsp_splitter_starved(
    const audiodsp_splitter_state_t *state, uint32_t tap) {
    return state->write_pos == state->read_pos[tap];
}

// Claim the next run of frames for `tap`, at most one chunk and never across
// the ring's wrap. Returns the frame count and writes the ring frame index it
// starts at; zero means the tap has caught up and the caller should hand out
// silence.
uint32_t audiodsp_splitter_take(audiodsp_splitter_state_t *state, uint32_t tap,
    uint32_t *start_frame);
