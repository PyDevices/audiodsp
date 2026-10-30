// Runtime-neutral kernel of audiopump.DriftResampler: reads a stream that
// arrives on one clock and plays it on another, steering the conversion
// ratio so the buffer between the two clocks stays at a target fill.
//
// New code -- not a CircuitPython port. Two devices that both say 48 kHz
// differ by tens of parts per million, so a Ring between an input and an
// output on separate clocks slowly fills or empties. `audiospeed.Resampler`
// converts at a fixed ratio and `SpeedChanger`'s 16.16 rate moves in steps of
// about 15 ppm, which is the size of the error being corrected. Here the
// read position is 32.32 fixed point, a step of 0.0002 ppm.
//
// Two parts, both described in docs/how-it-works/drift-resampler.md:
//
//   * Interpolation: a 32-tap Kaiser-windowed sinc, tabulated at 128
//     fractional positions with linear interpolation between neighbouring
//     rows (audiodsp_drift_table.h, written by tools/design_drift_table.py).
//     Per output frame that is 32 coefficient blends and 32 multiply-adds a
//     channel, all single-precision float.
//
//   * Control: once per block the fill (frames buffered ahead of the read
//     position) goes through two one-pole smoothers into a PI loop whose
//     output is the ratio's offset from nominal, clamped to +/-max_ppm. The
//     loop starts at 8x its bandwidth and narrows to it over about a minute,
//     so it locks quickly and then holds still.
//
// The pulling loop is not here: each runtime reaches its Ring differently.
// A block is plan(level) -> read that many frames -> push() -> render().
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AUDIODSP_DRIFT_TAPS 32u
#define AUDIODSP_DRIFT_PHASE_BITS 7u
#define AUDIODSP_DRIFT_PHASES (1u << AUDIODSP_DRIFT_PHASE_BITS)
//: The largest block, in output frames.
#define AUDIODSP_DRIFT_MAX_FRAMES 1024u
//: Bounds on max_ppm and on the nominal ratio (input rate over output rate).
//: The filter's cutoff is set for the input rate, so the node converts up or
//: across, never down: going down would fold everything between the two
//: Nyquist frequencies back into the band.
#define AUDIODSP_DRIFT_MAX_PPM 10000u
#define AUDIODSP_DRIFT_MIN_RATIO_NUM 1u
#define AUDIODSP_DRIFT_MIN_RATIO_DEN 2u
//: Bounds on the loop's settled bandwidth, in hertz. Above about 0.1 Hz the
//: loop starts following the producer's packet-sized steps in the fill, and
//: the ratio swings by hundreds of ppm from one block to the next.
#define AUDIODSP_DRIFT_MIN_BANDWIDTH 0.001f
#define AUDIODSP_DRIFT_MAX_BANDWIDTH 0.1f

typedef struct {
    // Configuration, fixed at init.
    uint32_t channels;      // 1 or 2
    uint32_t frames;        // output frames per block
    uint32_t in_rate;       // Hz
    uint32_t out_rate;      // Hz
    uint32_t max_ppm;
    uint64_t nominal;       // input frames per output frame, 32.32
    float max_dev;          // ratio offset limit, as a fraction
    float bandwidth;        // the loop's final natural frequency, Hz
    float dt;               // seconds per block
    float gear_decay;       // per-block decay of the start-up gear

    // Settable while running.
    uint32_t target;        // fill to hold, in input frames

    // Control state.
    float gear;             // extra bandwidth multiple, 7 -> 0
    float s1;               // fill, smoothed once
    float s2;               // fill, smoothed twice
    float integ;            // the loop's integrator: ratio offset
    float dev;              // the ratio offset in use this block
    uint64_t step;          // the ratio in use, 32.32
    bool running;           // false: waiting for the ring to reach target

    // Resampling state. `work` is interleaved float frames; `idx` is the
    // frame the read position is in, `frac` how far past it.
    float *work;
    uint32_t capacity;      // frames work holds
    uint32_t have;          // frames work holds now
    uint32_t idx;
    uint32_t frac;
    uint32_t pending;       // frames plan() asked for and push() has not had
    uint32_t skip;          // of those, how many to drop first

    uint32_t underruns;
} audiodsp_drift_t;

//: True if the pair of rates is one this node converts between.
bool audiodsp_drift_rates_ok(uint32_t in_rate, uint32_t out_rate);

//: Frames of `work` a node of this shape needs; allocate that times
//: channels floats. Arguments already checked.
uint32_t audiodsp_drift_capacity(uint32_t frames, uint32_t in_rate,
    uint32_t out_rate, uint32_t max_ppm);

//: The input frames one block can consume at most, which a target must
//: cover for the node ever to start.
uint32_t audiodsp_drift_min_target(uint32_t frames, uint32_t in_rate,
    uint32_t out_rate, uint32_t max_ppm);

//: The largest target a ring of `ring_frames` frames allows: the fill is
//: counted beyond the filter's look-ahead, which the ring must also hold.
uint32_t audiodsp_drift_max_target(uint32_t ring_frames);

//: The target a node gets when none is given: half the ring, or `least`
//: (audiodsp_drift_min_target) if half the ring is less.
uint32_t audiodsp_drift_default_target(uint32_t least, uint32_t ring_frames);

//: `work` is `audiodsp_drift_capacity(...) * channels` floats. `bandwidth`
//: is in hertz and positive; the bindings check every argument.
void audiodsp_drift_init(audiodsp_drift_t *d, float *work, uint32_t channels,
    uint32_t frames, uint32_t in_rate, uint32_t out_rate, uint32_t target,
    uint32_t max_ppm, float bandwidth);

//: Back to the state init left: waiting, history silent, loop at its
//: start-up gear. The rate estimate is forgotten too.
void audiodsp_drift_reset(audiodsp_drift_t *d);

//: Called once a block with the frames the ring holds. Returns how many to
//: read and push() before render(), or 0 for a block of silence (waiting for
//: the ring to fill, or an underrun, which is counted and sends the node back
//: to waiting). On the block that starts playback, the count includes what
//: the ring holds beyond the target, which push() drops, so playback starts
//: at the target rather than the loop having to drain the excess.
uint32_t audiodsp_drift_plan(audiodsp_drift_t *d, uint32_t level);

//: Interleaved 16-bit frames, in order; may be called more than once per
//: block (a ring's two segments) as long as the total is what plan() said.
void audiodsp_drift_push(audiodsp_drift_t *d, const int16_t *in,
    uint32_t frames);

//: Writes one block, `frames * channels` samples. Only after a plan() that
//: returned non-zero and the pushes it asked for.
void audiodsp_drift_render(audiodsp_drift_t *d, int16_t *out);

//: The ratio's offset from nominal, in ppm, and the smoothed fill, in
//: frames, for display.
float audiodsp_drift_ppm(const audiodsp_drift_t *d);
float audiodsp_drift_fill(const audiodsp_drift_t *d);
//: The ratio in use, input frames per output frame.
float audiodsp_drift_rate(const audiodsp_drift_t *d);
