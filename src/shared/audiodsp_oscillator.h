// Runtime-neutral band-limited oscillator: saw, pulse, triangle and sine from
// one phase, with hard sync, linear FM, pulse-width modulation and pitch all
// read from audio streams, sample by sample.
//
// New code -- not a CircuitPython port. synthio's oscillator is a wavetable
// read at a pitch that moves once per block, with no band limiting, no sync
// and no linear FM; everything here acts inside the block, which is the point.
//
// Band limiting is PolyBLEP for the steps (the saw's reset, the pulse's two
// edges, and every waveform's jump at a sync) and its integral, PolyBLAMP, for
// the corners (the triangle's two, and the slope a sync changes). Both are
// two-sample kernels, one sample either side of the event, so the output
// trails the phase by one sample: that is what lets a sync edge, which nothing
// can see coming, still correct the sample before it.
//
// The pulling loop is not here, for the same reason it is not in
// audiodsp_multiply.c: each runtime reaches its audio graph differently, so the
// bindings own the loop and call in with runs of frames.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

//: Frames one output block carries unless the caller asks for fewer.
#define AUDIODSP_OSCILLATOR_FRAMES 256u

//: Waveform flags. A channel's `waveform` is any sum of them, and the
//: waveforms it names are added together.
#define AUDIODSP_OSCILLATOR_SAW 1u
#define AUDIODSP_OSCILLATOR_PULSE 2u
#define AUDIODSP_OSCILLATOR_TRIANGLE 4u
#define AUDIODSP_OSCILLATOR_SINE 8u
#define AUDIODSP_OSCILLATOR_WAVES 15u

//: The four audio-rate inputs, in the order process() takes them.
typedef enum {
    AUDIODSP_OSCILLATOR_IN_PITCH = 0,
    AUDIODSP_OSCILLATOR_IN_FM,
    AUDIODSP_OSCILLATOR_IN_PWM,
    AUDIODSP_OSCILLATOR_IN_SYNC,
    AUDIODSP_OSCILLATOR_INPUTS,
} audiodsp_oscillator_input_t;

typedef enum {
    AUDIODSP_OSCILLATOR_OPT_FREQUENCY = 0,
    AUDIODSP_OSCILLATOR_OPT_WAVEFORM,
    AUDIODSP_OSCILLATOR_OPT_WAVEFORM_RIGHT,
    AUDIODSP_OSCILLATOR_OPT_PULSE_WIDTH,
    AUDIODSP_OSCILLATOR_OPT_LEVEL,
    AUDIODSP_OSCILLATOR_OPT_PITCH_OCTAVES,
    AUDIODSP_OSCILLATOR_OPT_FM_DEPTH,
    AUDIODSP_OSCILLATOR_OPT_PWM_DEPTH,
} audiodsp_oscillator_option_t;

typedef struct {
    uint32_t sample_rate;
    uint32_t channel_count;
    // The options as they were asked for.
    float frequency;
    float pulse_width;
    float level;
    float pitch_octaves;
    float fm_depth;
    float pwm_depth;
    uint8_t waveform[2];
    // --- derived, all computed in audiodsp_oscillator_config_finish() ---
    // Which waveforms either channel needs, so the others are not computed.
    uint8_t waves;
    // Phase increments: the base pitch's, and the bounds every sample's is
    // clamped to (0.1 Hz, and 0.49 of the sample rate).
    float inv_rate;
    float base_dt;
    float dt_min;
    float dt_max;
    // Per input count: octaves, phase increment, and pulse width.
    float pitch_scale;
    float fm_scale;
    float pwm_scale;
    // level * 32768: what one unit of waveform is in output counts.
    float gain;
} audiodsp_oscillator_config_t;

typedef struct {
    // 0..1, the phase at the last sample computed.
    float phase;
    // The sample being finished, one per waveform: saw, pulse, triangle,
    // sine. It is complete once the next sample's events have added their
    // half of the kernel, and only then is it written out.
    float pending[4];
    // The last sync input, in counts, so a rising zero crossing can be found
    // and placed between two samples.
    float sync_last;
    // The last pitch input, the scale it was read at, and 2 to the power of
    // their product, so a held pitch costs no exponential.
    int32_t pitch_last;
    float pitch_scale;
    float pitch_ratio;
    // The pulse's level: 1 from the reset until the falling edge.
    uint8_t pulse_high;
} audiodsp_oscillator_state_t;

void audiodsp_oscillator_config_init(audiodsp_oscillator_config_t *config,
    uint32_t sample_rate);

void audiodsp_oscillator_set_channel_count(
    audiodsp_oscillator_config_t *config, uint32_t channel_count);

// Stores one option as asked; out-of-range values are clamped by
// config_finish(), not refused.
void audiodsp_oscillator_configure(audiodsp_oscillator_config_t *config,
    audiodsp_oscillator_option_t option, float value);

void audiodsp_oscillator_config_finish(audiodsp_oscillator_config_t *config);

void audiodsp_oscillator_state_init(audiodsp_oscillator_state_t *state);

// Phase back to 0 and the kernel's history emptied. The next sample starts
// every waveform from its reset value.
void audiodsp_oscillator_reset(audiodsp_oscillator_state_t *state);

// Renders `frames` interleaved frames into `out`. `inputs[k]` is NULL for an
// input that is not connected (or has run dry), otherwise `frames` frames of
// signed 16-bit audio with `strides[k]` samples between frames; only the first
// channel of each is read.
//
//   pitch  out frequency = frequency * 2^(pitch/32768 * pitch_octaves)
//   fm     plus pitch... + fm/32768 * fm_depth Hz (linear, no through-zero)
//   pwm    pulse width = pulse_width + pwm/32768 * pwm_depth, kept in 0.05..0.95
//   sync   a rising zero crossing resets the phase, placed between samples
void audiodsp_oscillator_process_s16(
    const audiodsp_oscillator_config_t *config,
    audiodsp_oscillator_state_t *state, int16_t *out, uint32_t frames,
    const int16_t *const inputs[AUDIODSP_OSCILLATOR_INPUTS],
    const uint32_t strides[AUDIODSP_OSCILLATOR_INPUTS]);
