// Runtime-neutral float32 filter kernels: an RBJ biquad and a first-order
// all-pass cascade, both of which reach exact zero when the signal stops.
//
// New code -- not a CircuitPython port, and not from vstaudio either. It sits
// beside two frozen integer kernels that do the same jobs and cannot do this
// one:
//
//   * `shared/audiodsp_biquad.c` keeps its output memory in Q12 sample units
//     (`AUDIODSP_BIQUAD_STATE_SHIFT`) and rounds to nearest with no dither and
//     no leak term, so the recursion can land on a state that reproduces
//     itself exactly. Measured on audiodsp#23: `LowPass` 100 Hz parks at +1
//     LSB, `LowPass` 40 Hz q=8 at -4 LSB and the `Phaser` defaults at -4 LSB,
//     held for 3000 blocks / 32 s of silence.
//   * `shared/audiodsp_phaser.c` has its own version of the same defect in its
//     own arithmetic: `allpass_words[]` is `int16_t` in plain sample units
//     (`:28-38`), so the cascade settles on a non-zero word and holds it --
//     -6 LSB at the shipped `audioeffects.Phaser` settings. Fixing the biquad
//     would not move it, which is why one module carries both kernels.
//
// Neither frozen kernel is touched. Both are CircuitPython-ported code, and
// the port's rule is extend, never modify: an argument or an arithmetic
// change on audiodsp's copy of a CircuitPython module would not exist on a
// stock board, so a class written against it would silently be a different
// effect there. This is a new module instead, which either installs whole or
// is absent and says so on import.
//
// What float buys, precisely: with `float` state and a flush of any state
// word below AUDIODSP_FILTER_F32_FLUSH, a decaying tail reaches **exact zero**
// in finite time and silence in gives silence out for ever. That is the
// "Tier 1 silence" invariant every EQ, filter and phaser class in
// audiocomponents is held to. The second thing this carries, and the reason
// the all-pass is here rather than a patch to the biquad, is an **unclamped**
// feedback: `audiofilters/Phaser.c:211` limits it to 0.1..0.9, so a phaser
// built on the ported node can never reach the feedback-free topology the
// script pedals use, and its notches stop 17 dB short of a true null.
//
// The pulling loop is not here, for the same reason it is not in
// audiodsp_dynamics.c or audiodsp_feedback_delay.c: each runtime reaches its
// audio graph differently.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//: Frames one output block carries. The same 256 audiodynamics, audiomath and
//: audioecho use, so a chain built out of several of them moves in one block
//: size.
#define AUDIODSP_FILTER_F32_FRAMES 256u

//: Any state word smaller than this is written as exact zero. Well above the
//: float32 denormal floor (~1e-38) and far below one LSB of a 16-bit sample
//: relative to full scale (~3e-5), so it can only ever catch a tail that has
//: already decayed past -400 dB -- and it is what makes "reaches exact zero"
//: literal rather than "reaches something that rounds to zero".
#define AUDIODSP_FILTER_F32_FLUSH 1e-20f

//: The most stages an `AllPass` may cascade. Twelve is the top of the
//: quantised Stages control the Phaser dossier proposes (4/6/8/10/12); the
//: extra headroom costs nothing since the state is allocated by the binding.
#define AUDIODSP_FILTER_F32_MAX_STAGES 16u

// ---------------------------------------------------------------- biquad --

//: Filter shapes, numbered exactly as `synthio.FilterMode` and
//: `shared/audiodsp_biquad.c` number them, so a caller can hand either kernel
//: the same integer.
typedef enum {
    AUDIODSP_BIQUAD_F32_LOW_PASS = 0,
    AUDIODSP_BIQUAD_F32_HIGH_PASS = 1,
    AUDIODSP_BIQUAD_F32_BAND_PASS = 2,
    AUDIODSP_BIQUAD_F32_NOTCH = 3,
    AUDIODSP_BIQUAD_F32_PEAKING_EQ = 4,
    AUDIODSP_BIQUAD_F32_LOW_SHELF = 5,
    AUDIODSP_BIQUAD_F32_HIGH_SHELF = 6,
} audiodsp_biquad_f32_mode_t;

//: The settable options, in the order the bindings' keyword tables list them.
typedef enum {
    AUDIODSP_BIQUAD_F32_OPT_MODE = 0,
    AUDIODSP_BIQUAD_F32_OPT_FREQUENCY,
    AUDIODSP_BIQUAD_F32_OPT_Q,
    AUDIODSP_BIQUAD_F32_OPT_GAIN_DB,
    AUDIODSP_BIQUAD_F32_OPT_MIX,
} audiodsp_biquad_f32_option_t;

//: Everything derived from the constructor/`set()` arguments, held apart from
//: the running state so a mid-stream change never disturbs the recursion.
typedef struct {
    uint32_t sample_rate;
    uint32_t channel_count;
    int32_t mode;
    float frequency;
    float q;
    float gain_db;
    // 0..1, a crossfade between the untouched signal and the filtered one --
    // `audiofilters.Filter`'s convention (Filter.c:288), not `Echo`'s 0..2.
    float mix;
    // Written by audiodsp_biquad_f32_config_finish() from everything above.
    float b0, b1, b2, a1, a2;
    // What those five were computed from, so a block that changed nothing
    // does not pay for a sine, a cosine and five divisions.
    int32_t derived_mode;
    uint32_t derived_sample_rate;
    float derived_frequency, derived_q, derived_gain_db;
    bool derived;
} audiodsp_biquad_f32_config_t;

//: Direct Form I: two input words and two output words per channel. All
//: float, which is the whole ask.
// Transposed direct form II: two memory words per channel, not four.
//
// Direct form I computed `b0*x0 + b1*x1 + b2*x2 - a1*y1 - a2*y2`, and with the
// poles close to the unit circle that is the difference of two nearly-equal
// large numbers. At 20 Hz / Q 32 on a 48 kHz graph `a1 = -1.99991131` and
// `a2 = +0.99991816`, so `1 + a1 + a2` is 6.9e-06 while `float` resolves
// 1.2e-07 of the magnitudes being differenced -- about two decimal digits of
// headroom -- and a resonator that rings for Q*rate/(pi*f0) samples accumulates
// round-off the whole way.
//
// Measured over an 8x7 f0/Q grid (20 Hz..1 kHz, Q 0.5..32) at three probe
// levels, worst case over levels, peak gain at f0 against RBJ's 0 dB:
//
//     direct form I    7 of 56 cells outside 0.05 dB, worst +0.2034 dB
//     this form        1 of 56,                       worst -0.0785 dB
//
// The remaining cell is 31.5 Hz / Q 32, and it is level-independent, so it is
// the `float` coefficients placing the pole pair slightly beside f0 rather than
// round-off; no recursion form fixes that one. Same five coefficients, same
// arithmetic count, half the state. audiodsp#64.
//
// The 1.0906 dB that issue opened with was mostly the MEASUREMENT: its window
// ended at 2.09 ring-up time constants, and 20*log10(1 - exp(-2.09)) is
// -1.147 dB. See tests/test_cpython_audiobiquad.py.
//
// The all-pass beside this one was already transposed direct form II; the
// biquad was the odd one out. Nothing outside audiodsp_filter_f32.c reads these
// words -- every caller goes through state_init / reset / process.
typedef struct {
    float s1[2], s2[2];
} audiodsp_biquad_f32_state_t;

void audiodsp_biquad_f32_config_init(audiodsp_biquad_f32_config_t *config,
    uint32_t sample_rate, uint32_t channel_count);

// Applies immediately, against whatever `sample_rate` is currently set.
// Values are clamped here, once, rather than in each binding.
void audiodsp_biquad_f32_configure(audiodsp_biquad_f32_config_t *config,
    audiodsp_biquad_f32_option_t option, float value);

// Recomputes the coefficients if anything they depend on moved. Call after
// configuring and before processing; it is cheap when nothing changed.
void audiodsp_biquad_f32_config_finish(audiodsp_biquad_f32_config_t *config);

void audiodsp_biquad_f32_state_init(audiodsp_biquad_f32_state_t *state);

// Zeroes the recursion. Same thing as state_init; named for the bindings,
// which offer it as `clear()` and call it from `reset_buffer`.
void audiodsp_biquad_f32_reset(audiodsp_biquad_f32_state_t *state);

// Interleaved 16-bit frames in and out. `out` may alias `in`.
void audiodsp_biquad_f32_process_s16(const audiodsp_biquad_f32_config_t *config,
    audiodsp_biquad_f32_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames);

// -------------------------------------------------------------- all-pass --

//: The settable options, in the order the bindings' keyword tables list them.
typedef enum {
    AUDIODSP_ALLPASS_F32_OPT_FREQUENCY = 0,
    AUDIODSP_ALLPASS_F32_OPT_FEEDBACK,
    AUDIODSP_ALLPASS_F32_OPT_MIX,
} audiodsp_allpass_f32_option_t;

typedef struct {
    uint32_t sample_rate;
    uint32_t channel_count;
    uint32_t stages;
    // The frequency at which one stage's phase passes -90 degrees. Unlike
    // `audiofilters.Phaser`, this really is that frequency: the coefficient
    // is the bilinear (tan(pi f / fs) - 1) / (tan(pi f / fs) + 1), where the
    // ported node uses the small-angle form of it, so nothing here has to
    // pre-warp in Python.
    float frequency;
    // -0.99..0.99. Zero is exactly zero -- the reason this kernel exists --
    // and negative is the Electric Mistress's inverted loop. The magnitude
    // bound is stability, not taste: at |feedback| >= 1 the loop grows
    // without limit.
    float feedback;
    // 0..1, a crossfade. 0 is a wire, 0.5 is the equal sum the script phasers
    // make (and where the notches are deepest), 1 is the bare cascade, which
    // is magnitude-flat and so sounds like nothing at all.
    float mix;
    // Written by audiodsp_allpass_f32_config_finish().
    float coefficient;
    float derived_frequency;
    uint32_t derived_sample_rate;
    bool derived;
} audiodsp_allpass_f32_config_t;

typedef struct {
    // channel_count * stages floats, channel-major:
    // [channel * stages + stage]. The bindings own it; this struct only
    // borrows the pointer.
    float *stage_state;
    uint32_t stage_state_count;
    // The cascade's own output, fed back into its input next frame.
    float loop[2];
    // The coefficient actually in use, which walks to config->coefficient
    // across a block rather than stepping to it at the block boundary. A
    // block-rate step is what makes the ported phaser's sweep buzz.
    float coefficient;
    bool primed;
} audiodsp_allpass_f32_state_t;

void audiodsp_allpass_f32_config_init(audiodsp_allpass_f32_config_t *config,
    uint32_t sample_rate, uint32_t channel_count, uint32_t stages);

void audiodsp_allpass_f32_configure(audiodsp_allpass_f32_config_t *config,
    audiodsp_allpass_f32_option_t option, float value);

void audiodsp_allpass_f32_config_finish(audiodsp_allpass_f32_config_t *config);

// `stage_state` holds channel_count * stages floats and is owned by the
// caller. Passing NULL is legal and makes the node a wire.
void audiodsp_allpass_f32_state_init(audiodsp_allpass_f32_state_t *state,
    float *stage_state, uint32_t stage_state_count);

void audiodsp_allpass_f32_reset(audiodsp_allpass_f32_state_t *state);

// Interleaved 16-bit frames in and out. `out` may alias `in`.
void audiodsp_allpass_f32_process_s16(
    const audiodsp_allpass_f32_config_t *config,
    audiodsp_allpass_f32_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames);
