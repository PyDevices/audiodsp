// Runtime-neutral transistor ladder filter: four one-pole stages round a
// global feedback loop with an odd saturator inside the loop.
//
// New code -- not a CircuitPython port, and not from vstaudio either. The
// palette could not reach it, and the gap is not a matter of degree:
//
//   * `audiofilters.Filter` cascades two RBJ biquads. It tracks the analytic
//     resonance of this filter to within a fraction of a dB at every
//     resonance short of self-oscillation -- and it is LINEAR, so from
//     silence it stays silent however far the resonance is pushed. A ladder's
//     defining behaviour is that it does not: at a feedback of 4 the loop
//     sustains a tone of its own, and what stops that tone growing without
//     bound is the saturator inside the loop, which is also where the growl
//     comes from.
//   * Nothing else in the palette closes a loop of the right shape. An
//     audiodsp graph is a pull DAG in which no node accepts its own output, so
//     a loop exists only inside one C kernel. `audioecho`'s loop is
//     `echo * decay + sample` into a delay line -- one tap, no filter in the
//     path (audiodsp_echo.c:24, :31). `audioecho.FeedbackDelay`'s loop is a
//     comb: its resonances are harmonics of 1/delay, not one movable cutoff.
//     And a loop closed in Python runs at block rate, 187 Hz at 48 kHz,
//     which is 2.4 orders of magnitude too slow to be a filter at all.
//
// The pulling loop is not here, for the same reason it is not in
// audiodsp_dynamics.c or audiodsp_feedback_delay.c: each runtime reaches its
// audio graph differently.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

//: Frames one output block carries. The same 256 audiodynamics, audiomath and
//: audioecho use, so a chain built out of several of them moves in one block
//: size.
#define AUDIODSP_LADDER_FRAMES 256u

//: One-pole stages in the ladder. Four is not a parameter: it is what makes
//: the loop's phase reach 180 degrees at the cutoff and nowhere else, which
//: is what puts the self-oscillation at the cutoff. `poles` selects which
//: stage the OUTPUT is taken from; the feedback is always the fourth.
#define AUDIODSP_LADDER_STAGES 4u

//: Iterations of the feedback solver (see audiodsp_ladder.c). Fixed, and
//: fixed on purpose: a convergence test would branch on the signal, and two
//: interpreters that took different branches would render different bytes.
#define AUDIODSP_LADDER_SOLVE_STEPS 4

//: The settable options, in the order the bindings' keyword tables list them.
typedef enum {
    AUDIODSP_LADDER_OPT_CUTOFF_HZ = 0,
    AUDIODSP_LADDER_OPT_RESONANCE,
    AUDIODSP_LADDER_OPT_DRIVE,
    AUDIODSP_LADDER_OPT_POLES,
    AUDIODSP_LADDER_OPT_PASSBAND_COMP,
    AUDIODSP_LADDER_OPT_OVERSAMPLE,
    AUDIODSP_LADDER_OPT_MIX,
} audiodsp_ladder_option_t;

//: Everything derived from the constructor/`set()` arguments. Held apart from
//: the running state so `set()` can rewrite it mid-stream without disturbing
//: the integrators.
typedef struct {
    uint32_t sample_rate;
    uint32_t channel_count;
    // The options as they were asked for, so a change to any one of them can
    // recompute the coefficients the others also depend on -- `oversample`
    // moves the rate the filter runs at, which moves `g`, which moves
    // everything below.
    float cutoff_hz;
    float resonance;
    float drive;
    float passband_comp;
    float mix;
    uint8_t poles;
    uint8_t oversample;
    // --- derived, all computed in ladder_update_coefficients() ---
    // TPT one-pole coefficient: tan(pi*fc/fs') / (1 + tan(pi*fc/fs')), where
    // fs' is sample_rate * oversample. One tanf() per set(), never per
    // sample.
    float g;
    // g to the fourth: the forward gain of the whole chain at DC.
    float g4;
    // resonance * g4 -- the loop gain the solver works against.
    float loop_gain;
    // 1 / (1 + loop_gain), and loop_gain/(3*(1 + loop_gain)). The solver is
    // written so that these two constants are all it needs: no division and
    // no library call per sample.
    float inv_one_plus_loop;
    float loop_over_three;
    // 1 + passband_comp * resonance: what the tapped output is scaled by to
    // give back the passband the feedback subtracts.
    float output_gain;
} audiodsp_ladder_config_t;

typedef struct {
    // TPT integrator state, one per stage per channel. The true state, not a
    // scaled copy: a scaled copy would be wrong the instant `cutoff_hz`
    // moved, which is once a block for anything with a macro on it.
    float integrator[2][AUDIODSP_LADDER_STAGES];
    // The last two solved fourth-stage outputs, per channel. The solver is
    // seeded by extrapolating from them, which is what keeps its fixed
    // iteration count enough.
    float last[2];
    float previous_last[2];
    // The previous input sample, per channel: the 2x upsampler interpolates
    // between it and the current one.
    float previous_input[2];
} audiodsp_ladder_state_t;

void audiodsp_ladder_config_init(audiodsp_ladder_config_t *config,
    uint32_t sample_rate);

// Applies immediately, against whatever `sample_rate` is currently set --
// so set that first, as the bindings' keyword tables do.
void audiodsp_ladder_configure(audiodsp_ladder_config_t *config,
    audiodsp_ladder_option_t option, float value);

// Recomputes everything that depends on the sample rate. Call after changing
// it, and once after construction.
void audiodsp_ladder_config_finish(audiodsp_ladder_config_t *config);

void audiodsp_ladder_set_channel_count(audiodsp_ladder_config_t *config,
    uint32_t channel_count);

void audiodsp_ladder_state_init(audiodsp_ladder_state_t *state);

// Clears the integrators, the solver's history and the upsampler's. Like
// audioecho and unlike audiodynamics, everything goes: a self-oscillating
// filter restarted with its integrators still charged carries the previous
// take's tone into the new one.
void audiodsp_ladder_reset(audiodsp_ladder_state_t *state);

// Interleaved frames in and out, `channel_count` samples per frame. `out` may
// alias `in`.
void audiodsp_ladder_process_s16(const audiodsp_ladder_config_t *config,
    audiodsp_ladder_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames);
