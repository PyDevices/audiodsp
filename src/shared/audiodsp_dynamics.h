// Runtime-neutral dynamics DSP: an envelope-follower gain computer covering
// compression, limiting, downward expansion, gating and transient shaping,
// with an optional high-passed detector for de-essing.
//
// Ported from micropython-vst3's usermods/vstaudio/vstaudio_dsp.c, which is
// where the effects library's compressors have always lived. The arithmetic
// is unchanged, including the `float` working precision -- moving it to
// double would be a better filter and a different one, and the whole point of
// this file is that the ports keep sounding like the original.
//
// The source-pulling loop is not here: each runtime reaches its audio graph
// differently, so the bindings own the loop and call in with runs of frames.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AUDIODSP_DYNAMICS_COMPRESS = 0,
    AUDIODSP_DYNAMICS_LIMIT = 1,
    AUDIODSP_DYNAMICS_EXPAND = 2,
    AUDIODSP_DYNAMICS_GATE = 3,
    AUDIODSP_DYNAMICS_TRANSIENT = 4,
} audiodsp_dynamics_mode_t;

//: What the detector measures. The original had only the first: one absolute
//: value per channel, the largest across channels. A sine and a square of
//: equal RMS therefore get different gain from it, which is the thing an RMS
//: detector is asked for.
typedef enum {
    AUDIODSP_DYNAMICS_DETECT_PEAK = 0,
    AUDIODSP_DYNAMICS_DETECT_RMS = 1,
} audiodsp_dynamics_detector_t;

//: `true_peak` is no longer a flag but a level: off, the original's four-point
//: half-band midpoint estimate, or a 4x polyphase reconstruction.
typedef enum {
    AUDIODSP_DYNAMICS_TP_OFF = 0,
    AUDIODSP_DYNAMICS_TP_HALF_BAND = 1,
    AUDIODSP_DYNAMICS_TP_OVERSAMPLED = 2,
} audiodsp_dynamics_true_peak_t;

//: The 4x reconstruction's shape: four phases of twelve taps, an order-48
//: FIR, which is the shape ITU-R BS.1770 Annex 2 specifies for true-peak
//: metering. The taps themselves are audiodsp's own (see the table in the .c);
//: they are not transcribed from the Recommendation.
#define AUDIODSP_DYNAMICS_TP_PHASES 4u
#define AUDIODSP_DYNAMICS_TP_TAPS 12u

//: Frames one output block carries. The original chose it to match the
//: engine's block size, and the effects library's latency assumptions --
//: notably the Splitter ring's depth -- are written around it.
#define AUDIODSP_DYNAMICS_FRAMES 256u

//: The settable options, in the order the bindings' keyword tables list them.
typedef enum {
    AUDIODSP_DYNAMICS_OPT_THRESHOLD_DB = 0,
    AUDIODSP_DYNAMICS_OPT_RATIO,
    AUDIODSP_DYNAMICS_OPT_KNEE_DB,
    AUDIODSP_DYNAMICS_OPT_MAKEUP_DB,
    AUDIODSP_DYNAMICS_OPT_ATTACK_MS,
    AUDIODSP_DYNAMICS_OPT_RELEASE_MS,
    AUDIODSP_DYNAMICS_OPT_ATTACK_GAIN_DB,
    AUDIODSP_DYNAMICS_OPT_SUSTAIN_GAIN_DB,
    AUDIODSP_DYNAMICS_OPT_SIDECHAIN_HZ,
    AUDIODSP_DYNAMICS_OPT_LOOKAHEAD_MS,
    AUDIODSP_DYNAMICS_OPT_TRUE_PEAK,
    // Everything below here is additive and default-off: a Dynamics that
    // sets none of them is the node exactly as micropython-vst3 had it, which
    // is what tests/parity/dynamics_probe.py's unchanged hash asserts. New
    // names go on the end, because the CPython wrapper maps names to these
    // numbers.
    AUDIODSP_DYNAMICS_OPT_TRANSIENT_FAST_ATTACK_MS,
    AUDIODSP_DYNAMICS_OPT_TRANSIENT_FAST_RELEASE_MS,
    AUDIODSP_DYNAMICS_OPT_TRANSIENT_SLOW_ATTACK_MS,
    AUDIODSP_DYNAMICS_OPT_TRANSIENT_SLOW_RELEASE_MS,
    AUDIODSP_DYNAMICS_OPT_DETECTOR,
    AUDIODSP_DYNAMICS_OPT_RMS_MS,
    AUDIODSP_DYNAMICS_OPT_FEEDBACK_DETECTOR,
    AUDIODSP_DYNAMICS_OPT_SIDECHAIN_LP_HZ,
    AUDIODSP_DYNAMICS_OPT_SIDECHAIN_POLES,
    AUDIODSP_DYNAMICS_OPT_KEY_LISTEN,
    AUDIODSP_DYNAMICS_OPT_DEPTH_DB,
    AUDIODSP_DYNAMICS_OPT_HOLD_MS,
    AUDIODSP_DYNAMICS_OPT_HYSTERESIS_DB,
    AUDIODSP_DYNAMICS_OPT_RELATIVE_THRESHOLD,
    AUDIODSP_DYNAMICS_OPT_PROGRAM_ATTACK,
    AUDIODSP_DYNAMICS_OPT_TRANSIENT_DUAL,
    AUDIODSP_DYNAMICS_OPT_SUSTAIN_FAST_ATTACK_MS,
    AUDIODSP_DYNAMICS_OPT_SUSTAIN_FAST_RELEASE_MS,
    AUDIODSP_DYNAMICS_OPT_SUSTAIN_SLOW_ATTACK_MS,
    AUDIODSP_DYNAMICS_OPT_SUSTAIN_SLOW_RELEASE_MS,
    AUDIODSP_DYNAMICS_OPT_SLOW_HOLD_MS,
    AUDIODSP_DYNAMICS_OPT_GAIN_SMOOTH_MS,
    AUDIODSP_DYNAMICS_OPT_FEEDBACK_GAIN_CORRECTED,
    //: One past the last option, for a binding that range-checks.
    AUDIODSP_DYNAMICS_OPT_COUNT,
} audiodsp_dynamics_option_t;

//: How far ahead the detector may be allowed to look. A cap rather than a
//: preference: the buffer is audio in flight, so it is latency the whole
//: chain pays, and past a few milliseconds a limiter stops sounding like a
//: limiter and starts sounding like it is ducking before the note.
#define AUDIODSP_DYNAMICS_MAX_LOOKAHEAD_MS 50.0f

//: Everything derived from the constructor/`set()` arguments. Held apart from
//: the running state so `set()` can rewrite it mid-stream without disturbing
//: the detector.
typedef struct {
    uint32_t sample_rate;
    // Interleaved audio width. The bounded state arrays remain two entries so
    // mono and stereo use one implementation.
    uint32_t channel_count;
    int mode;
    float threshold_db;
    float ratio;
    float knee_db;
    float makeup_gain;      // linear
    float attack_coef;
    float release_coef;
    float attack_gain_db;   // transient mode
    float sustain_gain_db;
    float sidechain_coef;   // one-pole low-pass coefficient; 0 = full band
    // Both default off, so a Dynamics built the way the original was is the
    // original. See the process loop for what each costs.
    uint32_t lookahead_frames;
    uint8_t true_peak;      // audiodsp_dynamics_true_peak_t

    // --- the effects program's additions, every one of them default-off ---
    // The transient shaper's four detector time constants. They were literals
    // evaluated at process entry; the defaults here are those literals, so a
    // node that never sets them computes the same coefficients it always did.
    float transient_fast_attack_ms;
    float transient_fast_release_ms;
    float transient_slow_attack_ms;
    float transient_slow_release_ms;
    // A second pair, and the flag that applies both differences at once
    // instead of selecting one by the sign of the first.
    bool transient_dual;
    float sustain_fast_attack_ms;
    float sustain_fast_release_ms;
    float sustain_slow_attack_ms;
    float sustain_slow_release_ms;
    //: Frames the slow envelope holds its peak before it releases. 0 keeps
    //: the plain one-pole the shaper always had.
    uint32_t slow_hold_frames;

    uint8_t detector;       // audiodsp_dynamics_detector_t
    float rms_coef;         // mean-square window, one pole
    //: The detector reads the previous output frame rather than this frame's
    //: input: the feedback topology of a 1176, an LA-2A or a Fairchild, where
    //: the side chain is tapped after the gain cell.
    bool feedback_detector;
    //: The gain computer is driven by the side-chained level minus the
    //: full-band level, so a fixed spectral balance gets the same reduction
    //: wherever it sits in the level range.
    bool relative_threshold;
    //: The attack coefficient is scaled by how far the level overshoots the
    //: envelope, so a bigger overshoot is caught faster.
    bool program_attack;

    //: A low-pass corner beside `sidechain_hz`'s high-pass one, so the key
    //: path is a band; `sidechain_poles` cascades a second pole through both,
    //: taking the skirt from 6 to 12 dB/octave.
    float sidechain_lp_coef;
    uint8_t sidechain_poles;
    //: Put the detector signal on the output instead of the audio, which is
    //: what a gate's Key Listen switch does.
    bool key_listen;

    //: The floor the expander and the gate reach. Positive means unset, and
    //: unset is the original's fixed -60 dB / -80 dB literals: a depth is an
    //: attenuation, so no usable setting is above 0 dB.
    float depth_db;
    //: The gate's four-stage envelope. 0 hold frames keeps the memoryless
    //: gain computer the original had.
    uint32_t hold_frames;
    float hysteresis_db;
    //: A one-pole on the computed gain, between the gain computer and the
    //: multiply. 1.0 is off and is the default, which makes the smoothing an
    //: identity -- every figure measured before this option existed stays
    //: where it is (audiodsp#61).
    //:
    //: What it is for: with a short release the per-sample gain follows the
    //: waveform on low-frequency material, which is intermodulation rather
    //: than compression. Measured at 10 dB of gain reduction on a 50 Hz tone,
    //: five of `audioeffects.Compressor`'s fourteen shipped patches were over
    //: their 0.5 % THD bar -- the constructor's own defaults among them --
    //: while at 1 kHz only the patch that is *meant* to be dirty was. Nothing
    //: downstream of this node can remove that ripple without also removing
    //: the compression, which is why it belongs here.
    float gain_smooth_coef;
    //: Only meaningful with `feedback_detector`, and only in COMPRESS mode.
    //: Off, the detector reads the reduced output, so the loop reduces its own
    //: input and settles at its own fixed point rather than at the ratio asked
    //: for: measured against a 20 dB overshoot, 4:1 through 20:1 all land
    //: between 8.6 and 9.7 dB of reduction, i.e. about 2:1 whatever the knob
    //: says. On, the gain computer's slope becomes (R - 1) rather than
    //: (1 - 1/R), which is the algebra that makes an output-referred detector
    //: settle on the input-referred curve -- so the loop keeps its own
    //: dynamics and lands where it was told to (audiodsp#62).
    bool feedback_gain_corrected;
} audiodsp_dynamics_config_t;

//: What the detector remembers between blocks.
typedef struct {
    uint32_t channel_count;
    float sidechain_lp[2];
    float envelope;         // main detector, linear 0..1
    float fast_env;         // transient mode detectors
    float slow_env;
    float gain_reduction_db;
    // The audio held back while the detector reads ahead of it. The buffer
    // belongs to the bindings, which allocate it only when someone asks for
    // lookahead -- an unconditional 50 ms of stereo is 9.6 KB per instance,
    // and audioeffects builds nine of these.
    int16_t *lookahead;
    uint32_t lookahead_capacity;
    uint32_t lookahead_write;
    // Three detector samples back per channel, for the half-sample estimate.
    float peak_history[2][3];

    // --- state the additions above need, all of it inert while they are off ---
    float sidechain_hp2[2];     // second high-pass pole
    float sidechain_low[2];     // the low-pass arm of the key band
    float sidechain_low2[2];
    float rms_env[2];           // mean square, per channel
    float prev_output[2];       // what the feedback detector reads
    float full_env;             // full-band envelope, for relative threshold
    float fast_env2;            // the second transient pair
    float slow_env2;
    float slow_peak;            // the slow envelope's held peak
    uint32_t slow_hold_left;
    uint8_t gate_stage;         // audiodsp_dynamics_gate_stage_t
    float gate_gain;            // linear, the four-stage envelope's output
    uint32_t hold_left;
    // Twelve detector samples back per channel, for the 4x reconstruction.
    float tp_history[2][AUDIODSP_DYNAMICS_TP_TAPS];
    // The smoothed gain, and whether it holds one yet. The first sample after
    // a reset takes the computed gain whole rather than ramping to it from an
    // assumed resting value: the resting gain is makeup, not unity, so a
    // fixed starting point would put a short fade on the front of every
    // reset.
    float smoothed_gain;
    bool gain_smooth_primed;
} audiodsp_dynamics_state_t;

//: Where the gate's four-stage envelope is. CLOSED sits at the floor, ATTACK
//: runs up to unity, HOLD stays there while the key is above the hysteresis
//: point, DECAY falls back to the floor -- and a crossing during DECAY
//: re-enters ATTACK from wherever the gain got to, which is the trigger state
//: a memoryless gain computer cannot have.
typedef enum {
    AUDIODSP_DYNAMICS_GATE_CLOSED = 0,
    AUDIODSP_DYNAMICS_GATE_ATTACK = 1,
    AUDIODSP_DYNAMICS_GATE_HOLD = 2,
    AUDIODSP_DYNAMICS_GATE_DECAY = 3,
} audiodsp_dynamics_gate_stage_t;

float audiodsp_dynamics_ms_to_coef(float ms, float sample_rate);
float audiodsp_dynamics_db_to_gain(float db);
float audiodsp_dynamics_gain_to_db(float gain);

// Defaults, with the attack/release coefficients left at zero: apply the
// caller's options next, then call _finish(), which fills in the 10 ms / 120 ms
// defaults only if nothing set them. Splitting it this way keeps the original
// constructor's behaviour, quirk included -- an attack_ms so long that its
// coefficient rounds to zero silently gets the default instead.
void audiodsp_dynamics_config_init(audiodsp_dynamics_config_t *config, int mode,
    uint32_t sample_rate);
void audiodsp_dynamics_config_finish(audiodsp_dynamics_config_t *config);

// Apply one option. `sample_rate` is not among them: the millisecond
// conversions depend on it, so a binding must write it into the config before
// applying anything else, however its own keywords happened to be ordered.
void audiodsp_dynamics_configure(audiodsp_dynamics_config_t *config,
    audiodsp_dynamics_option_t option, float value);

void audiodsp_dynamics_state_init(audiodsp_dynamics_state_t *state);

// Clear only what the additive options remember. Called by
// audiodsp_dynamics_clear_detector, which is the one both state_init and reset
// go through.
void audiodsp_dynamics_clear_extras(audiodsp_dynamics_state_t *state);

// Everything the detector remembers, and nothing that is configuration: the
// side-chain filter memory, the envelopes, the reported gain reduction, the
// peak history and the extras. state_init and reset both call it, so a reset
// cannot come to mean less than a fresh build (audiodsp#56).
void audiodsp_dynamics_clear_detector(audiodsp_dynamics_state_t *state);

// Select mono or stereo processing before configuring lookahead storage.
void audiodsp_dynamics_set_channel_count(
    audiodsp_dynamics_config_t *config, audiodsp_dynamics_state_t *state,
    uint32_t channel_count);

// How many frames of lookahead buffer the current config wants. Bindings call
// this after applying options and hand back storage of at least that size;
// the DSP uses whatever it has, so a binding that allocates nothing simply
// gets no lookahead rather than reading off the end.
uint32_t audiodsp_dynamics_lookahead_frames(
    const audiodsp_dynamics_config_t *config);
void audiodsp_dynamics_set_lookahead(audiodsp_dynamics_state_t *state,
    int16_t *buffer, uint32_t frames);

// What the audiosample protocol's reset_buffer does: drop the detector
// envelopes but keep the sidechain filter's memory and the last reported gain
// reduction, matching the original exactly.
void audiodsp_dynamics_reset(audiodsp_dynamics_state_t *state);

// Process `frames` interleaved stereo frames. Input and output may not alias.
void audiodsp_dynamics_process_s16(const audiodsp_dynamics_config_t *config,
    audiodsp_dynamics_state_t *state, int16_t *output, const int16_t *input,
    uint32_t frames);

// The same, with an external key: the detector reads `key` (interleaved at the
// same width and rate) instead of the input, while the gain still lands on the
// input. `key == NULL` is exactly audiodsp_dynamics_process_s16.
void audiodsp_dynamics_process_s16_key(const audiodsp_dynamics_config_t *config,
    audiodsp_dynamics_state_t *state, int16_t *output, const int16_t *input,
    const int16_t *key, uint32_t frames);
