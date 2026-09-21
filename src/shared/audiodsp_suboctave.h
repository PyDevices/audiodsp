// Runtime-neutral analog-style octave divider: the Boss OC-2's front end,
// which is a comparator into a chain of flip-flops and nothing more.
//
// New code -- not a CircuitPython port, and not from vstaudio either. Nothing
// in the palette divides a *frequency*:
//
//   * **`audiodelays.PitchShift`** is granular. It resamples grains and
//     crossfades them, so an octave down arrives with the smear and the comb
//     that windowing costs, and it costs a grain buffer per instance. That is
//     a pitch shifter; a divider pedal is not one, and does not sound like
//     one.
//   * **`audiospeed.SpeedChanger`** moves the whole stream in time. An octave
//     down there is the take played at half speed, which is not an effect a
//     player can perform through.
//   * **`audiomath.Multiply`** rings the signal against a *free-running*
//     table. Multiplying by a square that is not locked to the input is a ring
//     modulator, and the sum and difference tones are exactly what a divider
//     must not produce.
//
// What an analog divider does instead is trivially cheap and has no window at
// all: square the input up with a comparator, halve that square with a
// flip-flop, and multiply the *original* signal by the halved square, which
// -- the square being +/-1 -- is a negate. The output is the input with every
// other cycle inverted, so its period is two input periods and its timbre is
// the input's, tracked sample by sample with zero latency. Two flip-flops in
// series give two octaves down.
//
// The comparator has hysteresis (a Schmitt trigger) and the clock has a
// re-trigger lockout, and both are there for the same reason the hardware has
// them: a guitar's waveform crosses zero more than twice per period as soon
// as the second harmonic is anywhere near the fundamental, and a divider that
// counts those extra crossings drops an octave too far and warbles. The
// hysteresis rejects the crossings near zero; the lockout rejects the ones
// that are big but too soon.
//
// The pulling loop is not here, for the same reason it is not in
// audiodsp_multiply.c: each runtime reaches its audio graph differently, so
// the bindings own the loop and call in with runs of frames.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

//: Frames one output block carries. The same 256 audiodynamics, audiomath's
//: Multiply and audioecho use, so a chain built out of several of them moves
//: in one block size.
#define AUDIODSP_SUBOCTAVE_FRAMES 256u

//: The settable options, in the order the bindings' keyword tables list them.
typedef enum {
    AUDIODSP_SUBOCTAVE_OPT_ORDER = 0,
    AUDIODSP_SUBOCTAVE_OPT_MIX,
    AUDIODSP_SUBOCTAVE_OPT_THRESHOLD,
    AUDIODSP_SUBOCTAVE_OPT_HOLD_MS,
} audiodsp_suboctave_option_t;

//: Everything derived from the constructor/`set()` arguments. Held apart from
//: the running state so `set()` can rewrite it mid-stream without resetting
//: the divider -- changing `mix` must not restart the count and flip the
//: output's polarity under the player.
typedef struct {
    uint32_t sample_rate;
    uint32_t channel_count;
    // 1 (one octave down, f/2) or 2 (two octaves down, f/4). Both flip-flops
    // always run; `order` only chooses which one's output is read, so a
    // change of order mid-note stays phase-locked to the same count.
    uint32_t order;
    // 0..32768: how much of the divided signal replaces the dry signal. Q15
    // like audiodsp_multiply.c's, and for the same reason -- every other term
    // in the process loop is already an integer.
    int32_t mix;
    // 0..32767, Q15 of full scale: how far from zero the comparator's two
    // rails sit. Everything between -threshold and +threshold leaves the
    // comparator where it is.
    int32_t threshold;
    // Frames after an accepted edge during which another is ignored.
    uint32_t hold_frames;
    // `hold_ms` as it was asked for, so a change of sample rate can recompute
    // the frame count.
    float hold_ms;
} audiodsp_suboctave_config_t;

typedef struct {
    // The Schmitt trigger's output: true once the input has been above
    // +threshold and not yet below -threshold.
    bool comparator;
    // The divide-by-two chain. Both start `true`, which is the pass-through
    // polarity, so a node that has seen no edge yet hands back the signal
    // rather than an inverted copy of it.
    bool flip1;
    bool flip2;
    // Frames left on the re-trigger lockout.
    uint32_t hold;
} audiodsp_suboctave_state_t;

void audiodsp_suboctave_config_init(audiodsp_suboctave_config_t *config,
    uint32_t sample_rate);

// Applies immediately, against whatever `sample_rate` is currently set --
// so set that first, as the bindings' keyword tables do.
void audiodsp_suboctave_configure(audiodsp_suboctave_config_t *config,
    audiodsp_suboctave_option_t option, float value);

// Recomputes everything that depends on the sample rate. Call after changing
// it, and once after construction.
void audiodsp_suboctave_config_finish(audiodsp_suboctave_config_t *config);

void audiodsp_suboctave_set_channel_count(audiodsp_suboctave_config_t *config,
    uint32_t channel_count);

void audiodsp_suboctave_state_init(audiodsp_suboctave_state_t *state);

// Back to the pass-through polarity with the comparator and the lockout
// cleared. There is no audio in this state -- the divider holds no signal, so
// unlike a delay's reset this drops nothing anybody can hear; what it does is
// stop a restarted chain from beginning on the inverted half of the count.
void audiodsp_suboctave_reset(audiodsp_suboctave_state_t *state);

// Interleaved frames in and out, `channel_count` samples each. `out` may
// alias `in`. One divider drives every channel, clocked by the mean of the
// frame, so a stereo pair cannot drift apart.
void audiodsp_suboctave_process_s16(const audiodsp_suboctave_config_t *config,
    audiodsp_suboctave_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames);
