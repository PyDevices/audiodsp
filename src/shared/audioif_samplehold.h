// Runtime-neutral sample-and-hold rate reducer: one frame in, one frame out,
// and the value only changes when an exact rational accumulator wraps.
//
// New code -- not a CircuitPython port. What the palette has instead is a pair
// of `audiospeed.SpeedChanger` nodes, one down by the hold ratio and one up by
// its reciprocal, and **the pair cannot be made reciprocal**. `SpeedChanger`'s
// rate is 16.16 fixed point, so a hold ratio only inverts exactly when the
// down leg's Q16 value divides 2^32 -- a power of two. At the effects
// programme's shipped hold (26 040 Hz at 48 kHz, ratio 120804/35553) the
// second-pass refuter measured the product of the two rates at
// 0.9999947184696794: one sample late per 189 339 frames, and at 44.1 kHz
// 1.0000107865780592, one sample early per 92 708. A four-sample click read
// delay 1 at frame 256 and 2 at frame 196 608; at Mix 0.5 a steady 12 kHz tone
// swung 10.74 dB over a twelve-second render. A slow flange on a static
// setting (audioif#97).
//
// The fix is not a finer grid -- it is a grid with no remainder. The hold
// ratio arrives as a pair of integers, `num` frames carrying `den` new values,
// and the accumulator is the exact remainder of `n * den` modulo `num`:
//
//     phase += den;                   // phase < num always
//     if (phase >= num) { phase -= num; latch the frame we are looking at; }
//     emit the latched frame
//
// Every quantity is an integer, nothing is rounded, and the accumulator
// returns to where it started after exactly `num` frames however long the
// render is. 26040/48000 reduces to 217/400 and is exact; so is 44100/26040,
// and so is every other pair a class can name. There is no rate at which this
// drifts, because there is nothing to drift: it is counting.
//
// **Latency is 0**, and the constant is worth stating because a hold looks
// like a delay and is not one. A refresh latches the frame it is looking at
// and emits it in the same frame, so nothing is buffered between input and
// output and a ratio of 1/1 is a wire byte for byte. What a hold *does*
// displace is an event that lands on a frame it drops -- that event arrives on
// the next kept frame, up to ceil(num/den) - 1 frames later -- and that
// displacement is the effect rather than a latency of this node. A class
// turning this into a rate knob reports the bound; see audioif#97.
//
// **It is not a resampler and must not interpolate.** A zero-order hold's
// images standing at their own level is the sound being asked for. Anything
// that smoothed them would be a different node.
//
// The node holds *frames*, as bytes, so it carries whatever its source is --
// 8- or 16-bit, mono or stereo, signed or not -- without looking inside a
// sample. Both channels of a stereo frame therefore refresh on the same frame,
// always, and silence in is that silence out to the byte whatever its DC
// happens to be.
//
// The pulling loop is not here, for the same reason it is not in
// audioif_shaper.c: each runtime reaches its audio graph differently.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

//: Frames one output block carries. The same 256 the rest of audioif's own
//: nodes use, so a chain built out of several of them moves in one block size.
#define AUDIOIF_SAMPLEHOLD_FRAMES 256u

//: The widest frame this node carries: two channels of 16-bit audio. A held
//: frame is a copy of one of these, so the bound is the size of the state.
#define AUDIOIF_SAMPLEHOLD_MAX_FRAME_BYTES 4u

//: The largest `num` a ratio may name. Well under 2^31, so `phase + den` can
//: never overflow a uint32_t, and far above any audio ratio: a sample rate
//: over a hold rate in whole hertz is at most a few hundred thousand.
#define AUDIOIF_SAMPLEHOLD_MAX_RATIO 1073741824u

//: The hold ratio, reduced. `num` source frames carry `den` new values, so
//: `den <= num` and the node refreshes `den` times in every `num` frames --
//: exactly, forever. 1/1 refreshes every frame, which is a wire.
typedef struct {
    uint32_t num;
    uint32_t den;
} audioif_samplehold_config_t;

typedef struct {
    //: The remainder of `n * den` modulo `num`, carried frame to frame. It is
    //: armed at `num - den`, so the very first frame wraps and latches: a node
    //: never emits a held value it has not been given.
    uint32_t phase;
    uint8_t held[AUDIOIF_SAMPLEHOLD_MAX_FRAME_BYTES];
} audioif_samplehold_state_t;

//: True if `num`/`den` is a ratio this node accepts: both at least 1, den no
//: greater than num (a hold cannot invent frames), num within the bound above.
//: The bindings call this and raise; the kernel never rejects silently.
bool audioif_samplehold_ratio_ok(uint32_t num, uint32_t den);

//: Reduces by the greatest common divisor, so 26040/48000 is stored as
//: 217/400 and every caller of `num`/`den` reads the same pair on every
//: target. Out-of-range input is clamped to 1/1 rather than left undefined;
//: the bindings are what refuse it.
void audioif_samplehold_config_init(audioif_samplehold_config_t *config,
    uint32_t num, uint32_t den);

//: Sets a new ratio mid-stream. Returns true if the reduced pair actually
//: moved, which is the caller's cue to re-arm the accumulator -- a ratio set
//: to what it already was leaves the hold running rather than re-latching, so
//: a class that writes its settings on every block does not restart the
//: staircase 187 times a second.
bool audioif_samplehold_config_set(audioif_samplehold_config_t *config,
    uint32_t num, uint32_t den);

void audioif_samplehold_state_init(audioif_samplehold_state_t *state,
    const audioif_samplehold_config_t *config);

//: Arms the accumulator and forgets the held frame. That is the whole of this
//: node's state: it has no delay line and no filter.
void audioif_samplehold_reset(audioif_samplehold_state_t *state,
    const audioif_samplehold_config_t *config);

//: `frames` frames of `frame_bytes` bytes each, in and out. `out` may alias
//: `in`: each frame is read before its output is written. `frame_bytes` must
//: be 1..AUDIOIF_SAMPLEHOLD_MAX_FRAME_BYTES; the bindings check it once, at
//: construction, rather than every block.
void audioif_samplehold_process(const audioif_samplehold_config_t *config,
    audioif_samplehold_state_t *state, uint8_t *out, const uint8_t *in,
    uint32_t frames, uint32_t frame_bytes);
