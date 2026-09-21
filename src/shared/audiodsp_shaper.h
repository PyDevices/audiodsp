// Runtime-neutral table-driven waveshaper with oversampling.
//
// New code -- not a CircuitPython port, and not from vstaudio either. What
// the palette has instead is `audiofilters.Distortion`, and two things about
// it put every drive circuit out of reach:
//
//   * **The curve is not yours.** Its whole argument list is `drive`,
//     `pre_gain`, `post_gain`, `mode`, `soft_clip` and `mix`
//     (audiodsp_distortion.c:7-9), and `mode` picks one of four fixed shapes
//     written for a game engine. A diode pair in a feedback loop, diodes to
//     ground, a biased germanium transistor: each is a specific curve, and
//     none of the four is any of them. Two of the four (CLIP and WAVESHAPE)
//     are odd-symmetric by construction, so they cannot make an even
//     harmonic at all, and CLIP is `pow(fabs(value), drive)` with the sign
//     put back -- homogeneous, so its harmonic profile is the same at -20
//     dBFS as at -6 dBFS. A circuit's is not.
//   * **It runs at the base rate.** A nonlinearity makes harmonics above
//     Nyquist and they fold back onto the signal. Measured on the shipped
//     Fuzz setting, a 1010 Hz sine through CLIP puts its non-harmonic energy
//     19.6 dB below the fundamental -- 40 dB short of the 60 dB the drive
//     dossiers ask for. (Probe at 1000 Hz instead and it reads -319 dB,
//     because at that frequency every alias lands back on a harmonic: an
//     absence that reads as agreement.) Nothing else in the palette
//     resamples either; `audiospeed.SpeedChanger` steps `phase >>
//     SPEED_SHIFT` with no interpolation and no anti-alias filter, and is
//     itself a CircuitPython port.
//
// So: the curve arrives as data -- an int16 Q15 table computed once on
// CPython, where the dossier can show the circuit maths that produced it,
// and never rebuilt on the target, whose float is single-precision where the
// desktop's is double -- and the shaping happens at 2x, 4x or 8x the sample
// rate between a matched pair of half-band filters.
//
// The half-bands are two-path polyphase all-pass cascades rather than FIRs,
// deliberately: an FIR half-band steep enough for this transition costs tens
// of taps and half of them again of group delay, and this node is meant to be
// usable in a live signal path. The all-pass pair is minimum-phase and costs
// one multiply per section per sample. What it is not is free: measured on
// the built extension (a 200 Hz..5 kHz sine, output phase against input, at
// 48 kHz), the whole up/shape/down chain's group delay is 0 samples at 1x,
// 2.2 samples (46 us) at 2x, 3.3 (69 us) at 4x and 3.9 (80 us) at 8x, and
// the magnitude response over 200 Hz..18 kHz is flat to within 0.001 dB.
// It is not an integer and it is not zero, so a class reporting
// `latency_samples` should report the figure for the factor it built with
// rather than assume either.
//
// The optional hysteresis is a *play* (backlash) operator in front of the
// table: the one thing a table cannot do is enclose an area. A static curve's
// output depends only on its present input, so a slow triangle in and out
// traces the same path twice and encloses exactly nothing -- which is the
// disconfirmation condition of Saturation's TP3 met a priori, without a
// measurement. The operator gives the table a position that lags the input by
// a half-width, so the rising and falling branches separate.
//
// The pulling loop is not here, for the same reason it is not in
// audiodsp_dynamics.c: each runtime reaches its audio graph differently.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

//: Frames one output block carries. The same 256 audiodynamics, audiomath and
//: audioecho use, so a chain built out of several of them moves in one block
//: size.
#define AUDIODSP_SHAPER_FRAMES 256u

//: The largest oversampling factor, and the number of half-band stages it
//: takes to reach it (log2). 8x is three doublings.
#define AUDIODSP_SHAPER_MAX_OVERSAMPLE 8u
#define AUDIODSP_SHAPER_MAX_STAGES 3u

//: All-pass sections in each of the two polyphase branches. Four coefficients
//: in total; see audiodsp_shaper.c for the design and the measured stopband.
#define AUDIODSP_SHAPER_HB_SECTIONS 2u

//: The settable options, in the order the bindings' keyword tables list them.
//: Append only, never renumber: the CPython wrapper maps names to these
//: integers and the extension range-checks against the last one, so inserting
//: one would change what an already installed wheel configures.
typedef enum {
    AUDIODSP_SHAPER_OPT_PRE_GAIN = 0,
    AUDIODSP_SHAPER_OPT_BIAS,
    AUDIODSP_SHAPER_OPT_POST_GAIN,
    AUDIODSP_SHAPER_OPT_MIX,
    AUDIODSP_SHAPER_OPT_HYSTERESIS,
    AUDIODSP_SHAPER_OPT_HYSTERESIS_WIDTH,
    AUDIODSP_SHAPER_OPT_HYSTERESIS_BIAS,
} audiodsp_shaper_option_t;

//: Everything derived from the constructor/`set()` arguments. Held apart from
//: the running state so `set()` can rewrite it mid-stream without disturbing
//: the half-band memories or the play operator's position.
typedef struct {
    uint32_t sample_rate;
    uint32_t channel_count;
    //: 1, 2, 4 or 8. Fixed at construction: it decides how many half-band
    //: stages carry state, and changing it mid-stream would step.
    uint32_t oversample;
    //: log2(oversample): 0, 1, 2 or 3.
    uint32_t stages;
    //: The curve, Q15, `curve_points` entries spanning -1..+1 of input. The
    //: bindings own the memory; this struct only borrows the pointer.
    const int16_t *curve;
    uint32_t curve_points;
    //: Gain into the curve -- the drive knob. The curve is normalised once,
    //: in Python, and never rebuilt to change drive.
    float pre_gain;
    //: Added after `pre_gain` and before the curve: the operating point. A
    //: bias that has to move per sample is a summed second stream in front of
    //: this node (audiomixer.Mixer adds sample by sample), not an argument.
    float bias;
    float post_gain;
    //: 0..1, a straight crossfade -- `audiofilters.Distortion`'s convention
    //: (audiodsp_distortion.c:47), which is the node the drive family is
    //: leaving. Not `audiodelays.Echo`'s 0..2.
    float mix;
    //: 0 switches the play operator off entirely, and the node is then a
    //: static table again, sample for sample.
    float hysteresis;
    //: The play operator's half-width at `hysteresis` 1, as a fraction of
    //: full scale **at the node's input** -- a coercivity, not a post-gain
    //: number. See config_finish for why that distinction is the whole
    //: difference between a loop that grows with drive and one that shrinks.
    float hysteresis_width;
    //: -1..+1: splits the half-width between the rising and falling branches,
    //: so the loop may be asymmetric.
    float hysteresis_bias;
    //: hysteresis * hysteresis_width * |pre_gain|, split by hysteresis_bias:
    //: the half-widths the operator actually compares against, in the units
    //: it sees.
    float width_up;
    float width_down;
} audiodsp_shaper_config_t;

//: One two-path polyphase half-band's memory: per branch, per section, the
//: previous input and output of the first-order all-pass
//: (a + z^-1)/(1 + a z^-1) running at that branch's own rate.
typedef struct {
    float x_prev[2][AUDIODSP_SHAPER_HB_SECTIONS];
    float y_prev[2][AUDIODSP_SHAPER_HB_SECTIONS];
    //: The decimator's odd branch is one output sample behind the even one
    //: (H(z) = (A0(z^2) + z^-1 A1(z^2)) / 2), so its result is held here.
    float hold;
} audiodsp_shaper_halfband_t;

typedef struct {
    audiodsp_shaper_halfband_t up[2][AUDIODSP_SHAPER_MAX_STAGES];
    audiodsp_shaper_halfband_t down[2][AUDIODSP_SHAPER_MAX_STAGES];
    //: The play operator's position, per channel.
    float play[2];
} audiodsp_shaper_state_t;

void audiodsp_shaper_config_init(audiodsp_shaper_config_t *config,
    uint32_t sample_rate, uint32_t oversample);

//: 1, 2, 4 and 8 are accepted; anything else is rounded down to the nearest
//: of them (and 0 up to 1). The bindings reject it before it gets here.
void audiodsp_shaper_set_oversample(audiodsp_shaper_config_t *config,
    uint32_t oversample);

void audiodsp_shaper_set_channel_count(audiodsp_shaper_config_t *config,
    uint32_t channel_count);

//: The curve is borrowed, not copied: the bindings keep it alive for as long
//: as the config does. `points` under 2 leaves the previous curve in place.
void audiodsp_shaper_set_curve(audiodsp_shaper_config_t *config,
    const int16_t *curve, uint32_t points);

void audiodsp_shaper_configure(audiodsp_shaper_config_t *config,
    audiodsp_shaper_option_t option, float value);

//: Recomputes everything derived from more than one option. Call after
//: construction and after any `set()`.
void audiodsp_shaper_config_finish(audiodsp_shaper_config_t *config);

void audiodsp_shaper_state_init(audiodsp_shaper_state_t *state);

//: Clears the half-band memories and the play position. The node has no
//: delay line, so this is the whole of its state.
void audiodsp_shaper_reset(audiodsp_shaper_state_t *state);

//: Interleaved 16-bit frames in and out. `out` may alias `in`.
void audiodsp_shaper_process_s16(const audiodsp_shaper_config_t *config,
    audiodsp_shaper_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames);
