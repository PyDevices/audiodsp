// Runtime-neutral feedback delay with everything a repeat needs inside the
// loop: a low-pass, a high-pass, a soft-clip, per-sample delay modulation,
// and a cross-feed between the channels.
//
// New code -- not a CircuitPython port, and not from vstaudio either.
// `audiodelays.Echo` exists upstream and has none of it: its feedback path is
// `echo * decay` and nothing else. Everything a delay is actually *called*
// falls out of what happens in that path, so without it there is one delay
// with a level knob:
//
//   * **Tape.** Each pass through a tape machine loses top and bottom and
//     softens, so the repeats darken and blur into the reverb tail. Filtering
//     the whole wet signal once, after the delay, is not the same thing at
//     all -- it darkens the first repeat as much as the tenth.
//   * **Analog / BBD.** The same, further: a bucket-brigade line is noisy and
//     band-limited by construction, and its clock drifts.
//   * **Ping-pong.** Repeats alternating between the speakers needs each
//     channel's output fed into the *other* channel's line. Panning two taps
//     hard left and right, which is all the palette could do before, gives
//     two independent delays that happen to sit either side.
//
// The delay modulation is per sample rather than per block, which is the
// second thing upstream cannot reach: an LFO-driven `delay_ms` updates once
// per buffer, about 187 Hz at 48 kHz, so it steps rather than glides and the
// doppler that makes tape wow sound like tape is not there.
//
// Four later options, every one of them off by default, so a node built the
// way it was before them renders the same bytes (tests/parity's `wow`,
// `everything` and every other existing case are unchanged across them):
//
//   * **`wow_shape`.** The modulation was a sine and only a sine. A bucket
//     brigade's delay is its line length over twice its clock, so a triangle
//     on the *clock* -- the Small Clone -- arrives as a reciprocal on the
//     delay, which no sine is. The shape is one period of int16 Q15 the
//     caller computes in Python; C keeps a phase accumulator and looks it up.
//   * **`delay_slew`.** A change of `delay_ms` used to land in one jump, read
//     unsmoothed, which is a click on the block boundary and no pitch bend at
//     all. With a slew the read head walks to the new time at a constant
//     rate, which is a constant pitch offset for exactly as long as the move
//     lasts -- an Echoplex's varispeed, and the reason a delay-time knob can
//     be turned while something is playing.
//   * **`wow_am_depth`.** The wow oscillator on the wet gain as well as on
//     the delay, dipping only, never boosting, and outside the loop.
//   * **`loop_semitones`.** The line read pitch-shifted *inside* the loop, by
//     two taps half a window apart under a triangular crossfade, so every
//     pass rises again. That is a shimmer, and no arrangement of the existing
//     nodes is one: `audiodelays.PitchShift` after a delay shifts the sum of
//     every repeat once, not each repeat one more time.
//
// The pulling loop is not here, for the same reason it is not in
// audiodsp_dynamics.c: each runtime reaches its audio graph differently.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

//: Frames one output block carries. The same 256 audiodynamics and audiomath
//: use, so a chain built out of several of them moves in one block size.
#define AUDIODSP_FEEDBACK_DELAY_FRAMES 256u

//: The settable options, in the order the bindings' keyword tables list them.
typedef enum {
    AUDIODSP_FEEDBACK_DELAY_OPT_DELAY_MS = 0,
    AUDIODSP_FEEDBACK_DELAY_OPT_FEEDBACK,
    AUDIODSP_FEEDBACK_DELAY_OPT_MIX,
    AUDIODSP_FEEDBACK_DELAY_OPT_DAMPING_HZ,
    AUDIODSP_FEEDBACK_DELAY_OPT_CUT_HZ,
    AUDIODSP_FEEDBACK_DELAY_OPT_WOW_HZ,
    AUDIODSP_FEEDBACK_DELAY_OPT_WOW_DEPTH_MS,
    AUDIODSP_FEEDBACK_DELAY_OPT_CROSS_FEED,
    AUDIODSP_FEEDBACK_DELAY_OPT_LOOP_DRIVE,
    AUDIODSP_FEEDBACK_DELAY_OPT_INPUT_PAN,
    // Appended, never inserted: `audioecho.py` maps option names to these
    // numbers and the CPython binding range-checks against the last one, so
    // renumbering an existing option would silently change what an installed
    // wheel configures.
    AUDIODSP_FEEDBACK_DELAY_OPT_DELAY_SLEW,
    AUDIODSP_FEEDBACK_DELAY_OPT_WOW_AM_DEPTH,
    AUDIODSP_FEEDBACK_DELAY_OPT_LOOP_SEMITONES,
    AUDIODSP_FEEDBACK_DELAY_OPT_LOOP_WINDOW_MS,
} audiodsp_feedback_delay_option_t;

//: A `wow_shape` table is one period, so its length has to be a power of two
//: for the phase accumulator's shift-and-mask to index it. Two is the
//: shortest thing that is still a waveform; 4096 is 8 KB, which is already
//: more than a board wants to spend on an LFO.
#define AUDIODSP_FEEDBACK_DELAY_WOW_SHAPE_MIN 2u
#define AUDIODSP_FEEDBACK_DELAY_WOW_SHAPE_MAX 4096u

//: Everything derived from the constructor/`set()` arguments. Held apart from
//: the running state so `set()` can rewrite it mid-stream without disturbing
//: the line or the filters.
typedef struct {
    uint32_t sample_rate;
    uint32_t channel_count;
    // Frames of line per channel. Fixed at construction: it is what was
    // allocated, and `delay_ms` is clamped against it.
    uint32_t line_frames;
    float delay_frames;
    float feedback;
    // 0..2, the dry at unity until 1: `audiodelays.Echo`'s convention. See
    // the process loop for why this module follows it rather than fixing it.
    float mix;
    // One-pole coefficients, 0 meaning the filter is out of the loop.
    float damping_coef;
    float cut_coef;
    // Rotation constant for the wow oscillator, and its depth in frames.
    float wow_step;
    float wow_depth_frames;
    float cross_feed;
    float loop_drive;
    // How much of each channel's input reaches its own line and the other
    // one, from `input_pan`. Both lanes need their own pair: panning the
    // input hard left means the left line takes both channels and the right
    // line takes neither, which is not a symmetric operation.
    float feed_own[2];
    float feed_other[2];
    // The frequency-shaped options as they were asked for, so a change of
    // sample rate can recompute their coefficients.
    float damping_hz;
    float cut_hz;
    float wow_hz;
    // One period of the modulation, Q15, or NULL for the built-in sine. The
    // config only *borrows* it: the bindings own the memory and have to keep
    // it alive for as long as the node points at it.
    const int16_t *wow_shape;
    // log2 of the table's length, so indexing is a shift and a mask.
    uint32_t wow_shape_shift;
    // Turns per frame in Q32, from `wow_hz`. The magic-circle pair carries no
    // phase index, so the table needs an accumulator of its own.
    uint32_t wow_phase_step;
    // Frames of delay change per frame -- delay-seconds per second, which is
    // sample-rate independent. 0 means a `delay_ms` change lands in one jump,
    // which is what this node did before the option existed.
    float delay_slew_frames;
    // 0..1. Depth of the wet-gain dip the wow oscillator drives.
    float wow_am_depth;
    // Pitch the loop is shifted by on each pass, -24..+24, 0 = off.
    float loop_semitones;
    float loop_window_ms;
    // The crossfade window in frames, from `loop_window_ms`, the sample rate
    // and the line's length.
    float shift_window_frames;
    // Turns per frame in Q32 for the crossfade, from `loop_semitones` and the
    // window. Signed: shifting down runs it backwards.
    int32_t shift_step;
} audiodsp_feedback_delay_config_t;

typedef struct {
    // 2 * line_frames samples, channel-major: [channel * line_frames + frame].
    // The bindings own it; this struct only borrows the pointer.
    int16_t *line;
    uint32_t write_frame;
    float damping_state[2];
    float cut_state[2];
    // Magic-circle oscillator: two states rotated by `wow_step` per frame,
    // which is two multiplies where sinf() would be a library call.
    float wow_sine;
    float wow_cosine;
    // Phase of the `wow_shape` table, and of the pitch shifter's crossfade.
    uint32_t wow_phase;
    uint32_t shift_phase;
    // Where the read head actually is while `delay_slew` walks it toward
    // `config->delay_frames`. Negative means "never primed": a fresh state
    // starts *on* the target rather than gliding up to it from nowhere.
    float delay_current;
} audiodsp_feedback_delay_state_t;

void audiodsp_feedback_delay_config_init(
    audiodsp_feedback_delay_config_t *config, uint32_t sample_rate,
    uint32_t line_frames);

// Applies immediately, against whatever `sample_rate` is currently set --
// so set that first, as the bindings' keyword tables do.
void audiodsp_feedback_delay_configure(audiodsp_feedback_delay_config_t *config,
    audiodsp_feedback_delay_option_t option, float value);

// Recomputes everything that depends on the sample rate. Call after changing
// it, and once after construction.
void audiodsp_feedback_delay_config_finish(
    audiodsp_feedback_delay_config_t *config);

// Borrows `table` -- the caller keeps it alive. `length` is in samples and
// must be a power of two between AUDIODSP_FEEDBACK_DELAY_WOW_SHAPE_MIN and
// _MAX; a NULL table puts the built-in sine back. Returns false, and changes
// nothing, when the length is not one this can index.
bool audiodsp_feedback_delay_set_wow_shape(
    audiodsp_feedback_delay_config_t *config, const int16_t *table,
    uint32_t length);

void audiodsp_feedback_delay_set_channel_count(
    audiodsp_feedback_delay_config_t *config, uint32_t channel_count);

void audiodsp_feedback_delay_state_init(audiodsp_feedback_delay_state_t *state,
    int16_t *line);

// Clears the line and the filters. Unlike audiodynamics' reset this really
// does drop everything: a delay's whole state is audible, and a chain that
// restarts with the old repeats still in the line plays the previous take.
void audiodsp_feedback_delay_reset(audiodsp_feedback_delay_state_t *state,
    const audiodsp_feedback_delay_config_t *config);

// Interleaved stereo frames in and out. `out` may alias `in`.
void audiodsp_feedback_delay_process_s16(
    const audiodsp_feedback_delay_config_t *config,
    audiodsp_feedback_delay_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames);
