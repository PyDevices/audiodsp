// Runtime-neutral partitioned convolution: an impulse response, applied
// properly, at a cost that does not grow with its length the way the naive
// sum does.
//
// New code -- there is nothing like it in CircuitPython or in vstaudio. It is
// the one effect on the catalogue that cannot be approximated by the others:
// a plate emulation is a network of delays that *sounds like* a plate, while
// a convolution of a plate's recorded impulse *is* that plate, and the same
// goes for a guitar cabinet, a stairwell, or a spring tank someone measured.
//
// **Uniform-partitioned overlap-save.** The impulse is cut into partitions of
// AUDIODSP_CONVOLVE_FRAMES taps, each transformed once at load time. Every
// block, the input's transform is pushed into a frequency-delay line and the
// output is the sum of that line against the stored partitions, multiplied
// bin by bin. Direct convolution costs one multiply per tap per sample; this
// costs two transforms plus one complex multiply-accumulate per bin per
// partition, which for a one-second impulse at 48 kHz is roughly forty times
// less arithmetic.
//
// **One partition of latency, and it is unavoidable here.** A block cannot be
// transformed until it is complete, so the output lags the input by
// AUDIODSP_CONVOLVE_FRAMES frames -- 5.3 ms at 48 kHz. Removing it means a
// non-uniform partitioning scheme (a few direct taps, then small partitions,
// then large) which roughly triples the code for a saving that matters only
// when monitoring a live player. Deliberately not done. A reverb or a cabinet
// in a mix does not care, and neither does an offline render.
//
// **What it costs in memory**, which is what decides whether an impulse fits
// on a microcontroller at all: each partition holds
// (AUDIODSP_CONVOLVE_FRAMES + 1) complex floats, so about 2 KB, and there is
// one frequency-delay line per audio channel plus one stored impulse per
// impulse channel. A 4096-tap (85 ms) mono impulse over stereo audio is
// therefore around 100 KB. A one-second impulse is 1.2 MB and belongs on a
// desktop or a board with PSRAM. audiodsp_convolve_float_count() is the exact
// answer; there is no allocation in here.
//
// The pulling loop is not here, for the same reason it is not in
// audiodsp_dynamics.c: each runtime reaches its audio graph differently.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "shared/audiodsp_fft.h"

//: Frames one output block carries, and taps in one partition. The same 256
//: audiodynamics, audiomath and audioecho use, so a chain built out of
//: several of them moves in one block size.
#define AUDIODSP_CONVOLVE_FRAMES 256u

//: The real transform is twice a partition, so a partition's worth of the
//: circular convolution can be thrown away and what is left is linear.
#define AUDIODSP_CONVOLVE_FFT (2u * AUDIODSP_CONVOLVE_FRAMES)
#define AUDIODSP_CONVOLVE_BINS (AUDIODSP_CONVOLVE_FRAMES + 1u)

//: A ceiling on partitions, so a Python-side mistake asks for a refusal
//: rather than a gigabyte. 512 partitions is 2.7 seconds at 48 kHz.
#define AUDIODSP_CONVOLVE_MAX_PARTITIONS 512u

typedef enum {
    AUDIODSP_CONVOLVE_OPT_MIX = 0,
} audiodsp_convolve_option_t;

typedef struct {
    uint32_t sample_rate;
    uint32_t channel_count;
    uint32_t partitions;
    // 1 or 2. A mono impulse is shared by both audio channels, which is both
    // the common case and half the memory; a stereo impulse gives each
    // channel its own, which is what a true-stereo room capture is for.
    uint32_t ir_channels;
    // 0..1 as the caller sets it, held doubled: audiofreeverb's convention,
    // where the dry stays at unity until halfway and the wet is already there.
    // Matching the other reverb matters more than matching the delays.
    float mix;
} audiodsp_convolve_config_t;

typedef struct {
    audiodsp_rfft_t rfft;
    // All of these point into the single block the caller handed over.
    float *window;     // 2 * AUDIODSP_CONVOLVE_FFT: the overlap-save history
    float *pending;    // 2 * AUDIODSP_CONVOLVE_FRAMES: input not yet a block
    float *emitted;    // 2 * AUDIODSP_CONVOLVE_FRAMES: last block's output
    float *fdl;        // 2 * partitions * 2 * BINS
    float *impulse;    // ir_channels * partitions * 2 * BINS
    float *accumulator;  // 2 * BINS
    float *scratch;    // AUDIODSP_CONVOLVE_FFT
    float *block;      // AUDIODSP_CONVOLVE_FFT
    uint32_t phase;    // frames into the block being gathered
    uint32_t cursor;   // newest slot in the frequency-delay line
    uint32_t loaded;   // partitions actually carrying impulse; 0 == bypass
} audiodsp_convolve_state_t;

// Floats the caller must hand to audiodsp_convolve_state_init for this
// configuration. Call it *after* the config is filled in.
uint32_t audiodsp_convolve_float_count(const audiodsp_convolve_config_t *config);

// `partitions` is clamped to 1..AUDIODSP_CONVOLVE_MAX_PARTITIONS and
// `ir_channels` to 1..2, so a caller cannot size a buffer from one number and
// run the DSP against another.
void audiodsp_convolve_config_init(audiodsp_convolve_config_t *config,
    uint32_t sample_rate, uint32_t partitions, uint32_t ir_channels);

void audiodsp_convolve_configure(audiodsp_convolve_config_t *config,
    audiodsp_convolve_option_t option, float value);

void audiodsp_convolve_set_channel_count(audiodsp_convolve_config_t *config,
    uint32_t channel_count);

// `storage` must hold audiodsp_convolve_float_count() floats and outlive the
// state. Leaves the impulse empty, which is a bypass rather than silence:
// a convolver with nothing loaded passes its input through, because an
// impulse that has not arrived yet is a missing setting, not a null room.
void audiodsp_convolve_state_init(audiodsp_convolve_state_t *state,
    const audiodsp_convolve_config_t *config, float *storage);

// Drops the history, the frequency-delay line and the block in flight. Keeps
// the loaded impulse -- that is a setting, not audio.
void audiodsp_convolve_reset(audiodsp_convolve_state_t *state,
    const audiodsp_convolve_config_t *config);

// Cuts `taps` into partitions and transforms each. `channels` is 1 or 2 and
// need not match the config's: a stereo impulse handed to a mono
// configuration keeps its left channel, and a mono impulse handed to a
// stereo one is copied to both. `gain` scales every tap; the taps themselves
// are int16 and are read as a fraction of full scale, so a gain of 1 with an
// impulse that peaks at full scale is unity at the loudest tap.
//
// Frames beyond `partitions * AUDIODSP_CONVOLVE_FRAMES` are dropped: the
// storage was sized from the config, and quietly reallocating from the DSP
// layer is not this code's job.
void audiodsp_convolve_load_s16(audiodsp_convolve_state_t *state,
    const audiodsp_convolve_config_t *config, const int16_t *taps,
    uint32_t tap_frames, uint32_t channels, float gain);

// Builds a decaying-noise impulse in place, so a reverb is available without
// a file to load. `decay_seconds` is the -60 dB time; `damping_hz` rolls the
// tail's top off as it decays (0 leaves it bright); `predelay_ms` is silence
// before anything arrives; `diffusion_ms` fades the tail in rather than
// starting it at full amplitude, which is what stops a synthetic impulse
// reading as a burst of noise. `seed` picks the room: two seeds are two
// different halls of the same size.
//
// Deterministic to the last bit on every interpreter -- xorshift for the
// noise and series for the exponentials, never libm -- because the parity
// goldens hash the result.
void audiodsp_convolve_synthesize(audiodsp_convolve_state_t *state,
    const audiodsp_convolve_config_t *config, float decay_seconds,
    float damping_hz, float predelay_ms, float diffusion_ms, uint32_t seed);

// Interleaved stereo frames in and out, any count. `out` may alias `in`.
// Blocks internally, so the output lags the input by AUDIODSP_CONVOLVE_FRAMES
// however the caller sizes its calls.
void audiodsp_convolve_process_s16(const audiodsp_convolve_config_t *config,
    audiodsp_convolve_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames);
