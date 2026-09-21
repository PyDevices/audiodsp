// Runtime-neutral mid/side matrix: a stereo pair decomposed into its mono sum
// and its difference, the difference scaled, and the pair rebuilt.
//
// New code -- not a CircuitPython port, and not from vstaudio either. Nothing
// upstream widens or narrows a stereo image. `audiomixer.Mixer` has a per-
// voice `pan`, which places a source between the speakers; it cannot reach
// what is already *between* them, and no amount of panning collapses a stereo
// pair to mono or pushes its sides out.
//
// The node exists for the drive classes rather than for stereo width alone.
// A saturator, a fuzz or an overdrive is a nonlinearity, and a nonlinearity
// applied to a stereo pair intermodulates the two channels: the sides come
// back as sum-and-difference products of both. Splitting to mid and side,
// driving the mid, and rebuilding is how a stereo drive keeps its image --
// and it is the only shape in which `audioroute.Splitter`'s parallel branches
// can carry a mid and a side rather than two copies of the same stereo pair.
//
// The pulling loop is not here, for the same reason it is not in
// audiodsp_multiply.c: each runtime reaches its audio graph differently, so
// the bindings own the loop and call in with runs of frames.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

//: Frames one output block carries. The same 256 audiomath and audiodynamics
//: use, so a chain built out of any of them moves in one block size.
#define AUDIODSP_MIDSIDE_FRAMES 256u

typedef struct {
    uint32_t channel_count;
    // 0..32768: the side gain, held as **Q14** rather than the Q15 every
    // other integer coefficient in this tree uses, because this one has to
    // reach 2.0 rather than 1.0. Q15 would put `width = 2` at 65536, and
    // 65536 * 65535 (the widest difference two int16 samples can have)
    // is 4294901760 -- past INT32_MAX, on the hot path, on parts where a
    // 64-bit multiply is not free. Q14 caps the same product at
    // 32768 * 65535 = 2147450880, which fits int32 with 32767 to spare.
    int32_t width;
} audiodsp_midside_config_t;

void audiodsp_midside_config_init(audiodsp_midside_config_t *config);

void audiodsp_midside_set_channel_count(audiodsp_midside_config_t *config,
    uint32_t channel_count);

// Clamps to 0..2. Anything outside is a caller's arithmetic slipping rather
// than a request worth honouring -- the same treatment
// audiodsp_multiply_set_mix gives a mix outside 0..1.
void audiodsp_midside_set_width(audiodsp_midside_config_t *config, float width);

// out[] = in[] with the difference between the channels scaled by `width`,
// over interleaved stereo frames. `out` may alias `in`; there is no state,
// so a block boundary is not observable and no reset is needed.
//
// At `width = 1` the output is the input, byte for byte, for every int16
// pair -- not approximately, exactly. That is what makes this node free to
// leave in a chain that does not use it, and it is asserted rather than
// assumed in tests/parity/midside_probe.py.
//
// A mono config copies its frames: there is no difference to scale.
void audiodsp_midside_process_s16(const audiodsp_midside_config_t *config,
    int16_t *out, const int16_t *in, uint32_t frames);
