// Runtime-neutral float32 modal resonator bank: N two-pole resonators in
// parallel, summed in float and quantised once at the output.
//
// New code -- not a CircuitPython port, and not from vstaudio either. A
// struck object (a drum head, a bar, a plate) rings as a sum of decaying
// sinusoids at inharmonic frequencies, and this is that sum as one node.
//
// WHY IT IS NOT A CHAIN OF `audiobiquad.Biquad`, MEASURED. A band-pass at
// high Q is a resonator, so the obvious construction is N of them in parallel
// behind `audioroute.Splitter` into `audiomixer.Mixer`. Two things stop it,
// and neither is a matter of taste:
//
//   * `audioroute.Splitter` carries at most AUDIOROUTE_MAX_TAPS = 4 taps, so
//     a bank wider than four modes is not expressible without a tree of
//     splitters feeding a tree of mixers.
//   * Every node in such a tree quantises to int16 on the way out. A mode 40
//     dB down on the fundamental is then carried in about five bits, and the
//     bank's output is dominated by the quantisation noise of its own quiet
//     modes. Measured on a six-mode 58 Hz kick, 48 kHz, identical excitation:
//     spectral centroid **4113 Hz** built from `Biquad` nodes against **73.8
//     Hz** with the same six modes summed in float -- 0.86% of the energy
//     above 1 kHz against 0.00%. The first is not a duller version of the
//     second, it is noise wearing its envelope.
//
// So the sum has to happen before the quantiser, which means one node.
//
// WHY IT IS PARAMETERISED BY DECAY TIME AND NOT Q. `audioif_filter_f32.c`
// clamps Q to AUDIOIF_FILTER_F32_MAX_Q = 60, and says why: an RBJ section's
// pole radius goes to 1 as Q rises, so a high-Q section rings past any bound
// worth having. That rail is right for a filter and fatal for a resonator
// bank -- a 20" ride cymbal's modes ring for seconds, and Q = pi*f*T60/ln(1e3)
// puts a 3 kHz mode with a 3 s decay at Q = 4093, sixty-eight times the cap.
// A bank asks for `decay` in seconds instead. The bound is then explicit and
// stated in the units the caller is actually thinking in, and
// AUDIOIF_MODAL_MAX_DECAY is a number a reader can check against the music.
//
// WHAT A SILENT MODE COSTS. Nothing, and that is load-bearing rather than an
// optimisation. Because every state word is flushed to exact zero below
// AUDIOIF_MODAL_FLUSH, a mode that has finished ringing is *exactly* zero,
// not nearly zero -- so the process loop can skip it on a test that can never
// be wrong by a fraction of an LSB. A kit holding ten drums resident pays for
// the modes that are actually sounding, which is what makes one bank per drum
// affordable on a microcontroller.
//
// The pulling loop is not here, for the same reason it is not in
// audioif_filter_f32.c or audioif_dynamics.c: each runtime reaches its audio
// graph differently.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//: Frames one output block carries. The same 256 every other audioif node
//: uses, so a chain built out of several of them moves in one block size.
#define AUDIOIF_MODAL_FRAMES 256u

//: The most modes one bank may carry. A 20" ride is the hungriest thing in a
//: drum kit and the literature counts its prominent partials in the dozens;
//: 64 leaves room above that without letting a Python mistake allocate
//: unboundedly. The state is allocated by the binding, so the ceiling costs
//: nothing when it is not used.
#define AUDIOIF_MODAL_MAX_MODES 64u

//: Any state word smaller than this is written as exact zero. Same threshold
//: and same reasoning as AUDIOIF_FILTER_F32_FLUSH: well above the float32
//: denormal floor (~1e-38) and far below one LSB of a 16-bit sample relative
//: to full scale (~3e-5), so it can only catch a tail already past -400 dB.
//: Here it does a second job -- it is what makes "this mode is finished" a
//: test the process loop can trust.
#define AUDIOIF_MODAL_FLUSH 1e-20f

//: Decay bounds, in seconds, for the 60 dB decay time of one mode. The floor
//: is one millisecond: shorter than that is a click rather than a mode, and
//: it keeps the exp() argument inside the range the series below is accurate
//: over. The ceiling is thirty seconds, which is longer than any struck
//: instrument sustains and short enough to be a real bound.
#define AUDIOIF_MODAL_MIN_DECAY 0.001f
#define AUDIOIF_MODAL_MAX_DECAY 30.0f

//: The settable per-bank options, in the order the bindings' keyword tables
//: list them.
typedef enum {
    AUDIOIF_MODAL_OPT_MIX = 0,
    AUDIOIF_MODAL_OPT_GAIN,
} audioif_modal_option_t;

//: One mode: where it sits, how long it rings, and how loud it is. `gain` is
//: the peak of this mode's impulse response, so a caller reading a modal
//: table out of the literature can write the published relative amplitudes
//: straight in without solving for a filter gain first.
typedef struct {
    float frequency;
    float decay;
    float gain;
} audioif_modal_mode_t;

//: Per-mode recursion coefficients, written by audioif_modal_config_finish()
//: from the mode table above.
typedef struct {
    float b0;       // gain * sin(w0) -- normalises the IR peak to `gain`
    float a1, a2;   // -2*r*cos(w0), r*r
} audioif_modal_coeff_t;

//: Everything derived from the constructor and `set()`, held apart from the
//: running state so a mid-stream change never disturbs a ringing mode.
typedef struct {
    uint32_t sample_rate;
    uint32_t channel_count;
    uint32_t mode_count;
    // 0..1, a crossfade between the untouched signal and the resonated one --
    // `audiofilters.Filter`'s convention, not `Echo`'s 0..2.
    float mix;
    // Applied to the summed bank before the crossfade, so a caller can write
    // the literature's relative amplitudes per mode and set the level once.
    float gain;
    // Borrowed from the binding: mode_count entries each.
    audioif_modal_mode_t *modes;
    audioif_modal_coeff_t *coeffs;
    bool derived;
} audioif_modal_config_t;

//: Transposed direct form II, two memory words per mode per channel -- the
//: same form and for the same reason as audioif_filter_f32.c's biquad
//: (audioif#64): with the poles this close to the unit circle, direct form I
//: differences two nearly-equal large numbers and `float` has about two
//: decimal digits of headroom left to do it in. A resonator bank lives
//: further into that corner than any filter does, because a long decay is
//: precisely a pole close to the circle held there for a long time.
typedef struct {
    float *s1;      // mode_count * channels
    float *s2;      // mode_count * channels
    uint32_t words; // how many each of s1/s2 has room for
} audioif_modal_state_t;

void audioif_modal_config_init(audioif_modal_config_t *config,
    uint32_t sample_rate, uint32_t channel_count, uint32_t mode_count,
    audioif_modal_mode_t *modes, audioif_modal_coeff_t *coeffs);

// Applies immediately. Values are clamped here, once, rather than in each
// binding.
void audioif_modal_configure(audioif_modal_config_t *config,
    audioif_modal_option_t option, float value);

// Sets one mode. `index` past mode_count is ignored rather than an error --
// the bindings check and raise; the kernel raises nothing.
void audioif_modal_set_mode(audioif_modal_config_t *config, uint32_t index,
    float frequency, float decay, float gain);

// Recomputes every mode's coefficients if anything moved. Call after
// configuring and before processing; cheap when nothing changed.
void audioif_modal_config_finish(audioif_modal_config_t *config);

// How many floats each of s1/s2 needs for this configuration. The binding
// allocates and hands the blocks over; the kernel never allocates.
uint32_t audioif_modal_state_floats(const audioif_modal_config_t *config);

void audioif_modal_state_init(audioif_modal_state_t *state,
    const audioif_modal_config_t *config, float *s1, float *s2,
    uint32_t words);

// Zeroes every recursion -- every mode stops ringing at once. Named for the
// bindings, which offer it as `clear()` and call it from `reset_buffer`.
void audioif_modal_reset(audioif_modal_state_t *state);

// True when no mode holds any energy at all, which after the flush above is
// an exact test rather than a threshold. The bindings use it to answer
// `ringing`.
bool audioif_modal_silent(const audioif_modal_state_t *state);

// Interleaved 16-bit frames in and out. `out` may alias `in`.
void audioif_modal_process_s16(const audioif_modal_config_t *config,
    audioif_modal_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames);
