// Runtime-neutral algorithmic reverberation tank: Dattorro's plate topology,
// with the line-length table and the output taps handed in from Python.
//
// New code -- not a CircuitPython port, and not from vstaudio either.
// `audiofreeverb.Freeverb` is the only reverberation network the palette had,
// and it is a *fixed* Schroeder/Moorer comb-and-all-pass bank: its line
// lengths are constants inside a CircuitPython-ported binding
// (`audiodsp_freeverb.c`), there are no modulated taps, no dispersive input
// chain, and no way to re-cut the topology per character. So one thing the
// palette could not do at all:
//
//   * **Re-tune the network.** A plate, a hall, a room and a chamber are not
//     one topology at four mix settings; they are different line-length sets
//     and different tap positions. Freeverb's are compiled in.
//   * **Recirculate at all, from Python.** Composing a tank out of palette
//     nodes needs a *cycle* in the pull graph, and there is none to be had:
//     `d1.play(s); d2.play(d1); d1.play(d2)` constructs happily and the first
//     `get_buffer(d1)` raises `RecursionError`. The only Python-side loop
//     quantises every line to one 256-frame block (5.33 ms at 48 kHz), longer
//     than most of a plate's lines.
//   * **Modulate the tank's own all-passes.** The picket-fence ringing of a
//     static plate is broken by wobbling the first all-pass in each half by a
//     fraction of a millisecond. Nothing outside the loop can reach it.
//
// `audiofreeverb` is untouched by this module: it is CircuitPython's, and a
// new module either installs whole or is absent and says so on import.
//
// The pulling loop is not here, for the same reason it is not in
// audiodsp_feedback_delay.c: each runtime reaches its audio graph differently.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

//: Frames one output block carries. The same 256 the rest of audiodsp's own
//: nodes use, so a chain built out of several of them moves in one block size.
#define AUDIODSP_TANK_FRAMES 256u

//: Delay lines the network owns, in the order the bindings' `delays` table
//: lists them:
//:
//:   0..3   the input diffusers (four Schroeder all-passes)
//:   4..7   tank half A: modulated all-pass, delay, all-pass, delay
//:   8..11  tank half B: the same four
//:
//: The two halves cross into each other, which is what makes it a tank rather
//: than two reverberators side by side.
#define AUDIODSP_TANK_LINES 12u
#define AUDIODSP_TANK_DIFFUSERS 4u

//: Output taps, each a (channel, line, offset, gain) quadruple. Dattorro's own
//: table is 7 per channel; 32 leaves room for a denser character without
//: making the config enormous.
#define AUDIODSP_TANK_MAX_TAPS 32u

//: The shortest line the interpolator and the tap reader can work with.
#define AUDIODSP_TANK_MIN_LINE 4u

//: The settable options, in the order the bindings' keyword tables list them.
typedef enum {
    AUDIODSP_TANK_OPT_DECAY = 0,
    AUDIODSP_TANK_OPT_DIFFUSION,
    AUDIODSP_TANK_OPT_DAMPING_HZ,
    AUDIODSP_TANK_OPT_BANDWIDTH_HZ,
    AUDIODSP_TANK_OPT_LOW_CUT_HZ,
    AUDIODSP_TANK_OPT_PREDELAY_MS,
    AUDIODSP_TANK_OPT_MOD_DEPTH_MS,
    AUDIODSP_TANK_OPT_MOD_RATE_HZ,
    AUDIODSP_TANK_OPT_DRIVE,
    AUDIODSP_TANK_OPT_WIDTH,
    AUDIODSP_TANK_OPT_TONE_DB,
    AUDIODSP_TANK_OPT_MIX,
} audiodsp_tank_option_t;

//: What `audiodsp_tank_set_delays` / `_set_taps` report back. The bindings turn
//: these into the runtime's own exception; the DSP layer raises nothing.
typedef enum {
    AUDIODSP_TANK_OK = 0,
    AUDIODSP_TANK_ERR_COUNT,     // wrong number of values
    AUDIODSP_TANK_ERR_LENGTH,    // a line shorter than AUDIODSP_TANK_MIN_LINE
    AUDIODSP_TANK_ERR_TOTAL,     // the whole network would not fit in memory
    AUDIODSP_TANK_ERR_CHANNEL,   // a tap on a channel that does not exist
    AUDIODSP_TANK_ERR_LINE,      // a tap on a line index that does not exist
    AUDIODSP_TANK_ERR_OFFSET,    // a tap past the end of its line
} audiodsp_tank_status_t;

//: The whole network as it was asked for. Held apart from the running state so
//: `set()` can rewrite it mid-stream without emptying the lines. The line
//: table and the tap table are the exception: they size the allocation, so
//: they are fixed once the state is initialised.
typedef struct {
    uint32_t sample_rate;
    uint32_t channel_count;
    uint32_t line_frames[AUDIODSP_TANK_LINES];
    // Frames of predelay line. Allocated once, from `max_predelay_ms`.
    uint32_t predelay_frames_max;
    uint32_t predelay_frames;
    float decay;
    float diffusion;
    // One-pole coefficients, 0 meaning the filter is out of the path.
    float damping_coef;
    float bandwidth_coef;
    float low_cut_coef;
    float tone_coef;
    // The tilt's two ends, 10^(+/- tone_db/40).
    float tone_low_gain;
    float tone_high_gain;
    // Rotation constant for the modulation oscillator, and its depth.
    float mod_step;
    float mod_depth_frames;
    float drive;
    float width;
    // 0..2, the dry at unity until 1: `audiodelays.Echo`'s convention, and
    // `audioecho.FeedbackDelay` follows it too. See the process loop.
    float mix;
    uint8_t tap_channel[AUDIODSP_TANK_MAX_TAPS];
    uint8_t tap_line[AUDIODSP_TANK_MAX_TAPS];
    uint32_t tap_offset[AUDIODSP_TANK_MAX_TAPS];
    float tap_gain[AUDIODSP_TANK_MAX_TAPS];
    uint32_t tap_count;
    // The frequency- and time-shaped options as they were asked for, so a
    // change of sample rate can recompute their coefficients.
    float damping_hz;
    float bandwidth_hz;
    float low_cut_hz;
    float mod_rate_hz;
    float mod_depth_ms;
    float predelay_ms;
    float tone_db;
} audiodsp_tank_config_t;

typedef struct {
    // One allocation, carved up here: the twelve lines in table order, then
    // the predelay. The bindings own it; this struct only borrows pointers.
    int16_t *lines[AUDIODSP_TANK_LINES];
    uint32_t write[AUDIODSP_TANK_LINES];
    int16_t *predelay;
    uint32_t predelay_write;
    float bandwidth_state;
    float low_cut_state;
    float damping_state[2];
    float tone_state[2];
    // Magic-circle oscillator, the same two-multiply resonator
    // audiodsp_feedback_delay.c uses. Its sine and cosine are the quadrature
    // pair the two tank halves modulate against.
    float mod_sine;
    float mod_cosine;
} audiodsp_tank_state_t;

// Installs Dattorro's own line lengths and output taps, scaled to
// `sample_rate`, and the defaults for every option. `max_predelay_ms` sizes
// the predelay line and cannot change afterwards.
void audiodsp_tank_config_init(audiodsp_tank_config_t *config,
    uint32_t sample_rate, float max_predelay_ms);

// The published table at 29761 Hz, scaled to `sample_rate`. `delays` takes
// AUDIODSP_TANK_LINES values; `taps` takes 4 * AUDIODSP_TANK_DEFAULT_TAPS floats
// and the return value is that count of quadruples.
void audiodsp_tank_default_delays(uint32_t sample_rate, uint32_t *delays);
uint32_t audiodsp_tank_default_taps(uint32_t sample_rate, float *taps);
#define AUDIODSP_TANK_DEFAULT_TAPS 14u

// Re-cuts the topology. Both size the allocation, so both are applied before
// audiodsp_tank_state_init and are refused afterwards by construction: nothing
// re-reads them per frame except through the state's own pointers.
audiodsp_tank_status_t audiodsp_tank_set_delays(audiodsp_tank_config_t *config,
    const uint32_t *frames, uint32_t count);
// `values` is 4 floats per tap: channel, line index, offset in frames, gain.
audiodsp_tank_status_t audiodsp_tank_set_taps(audiodsp_tank_config_t *config,
    const float *values, uint32_t count);

void audiodsp_tank_set_channel_count(audiodsp_tank_config_t *config,
    uint32_t channel_count);

// Applies immediately, against whatever `sample_rate` is currently set --
// so set that first, as the bindings' keyword tables do.
void audiodsp_tank_configure(audiodsp_tank_config_t *config,
    audiodsp_tank_option_t option, float value);

// Recomputes everything that depends on the sample rate. Call after changing
// it, and once after construction.
void audiodsp_tank_config_finish(audiodsp_tank_config_t *config);

// int16 samples the bindings must hand to audiodsp_tank_state_init: every line
// plus the predelay. Mono throughout -- the tank sums its input to one signal
// and the taps make the stereo back.
uint32_t audiodsp_tank_buffer_samples(const audiodsp_tank_config_t *config);

void audiodsp_tank_state_init(audiodsp_tank_state_t *state,
    const audiodsp_tank_config_t *config, int16_t *buffer);

// Clears every line and every filter. Like audioecho and unlike audiodynamics
// this really does drop everything: a reverberation tail is entirely state,
// and a chain restarted with the old tail still in the lines plays the
// previous take underneath the new one.
void audiodsp_tank_reset(audiodsp_tank_state_t *state,
    const audiodsp_tank_config_t *config);

// Interleaved frames in and out, `channel_count` samples each. `out` may
// alias `in`.
void audiodsp_tank_process_s16(const audiodsp_tank_config_t *config,
    audiodsp_tank_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames);
