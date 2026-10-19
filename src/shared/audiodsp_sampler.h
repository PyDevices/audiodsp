// Runtime-neutral sample playback: N voices, each reading a mono 16-bit
// buffer at its own pitch through a cubic interpolator and a per-sample
// amplitude envelope, summed in float and quantised once at the output.
//
// New code -- not a CircuitPython port. `audiomixer.Mixer` plays sources at
// their own rate and nothing else; `synthio` plays waveforms, which are
// single cycles. Neither can take a recorded note, play it a fourth up, loop
// a region of it while the key is held and let the rest of it out when the
// key lifts. That is what a sampler is, and what SoundFont instruments and
// every drum machine need. `audiocomponents` builds the reader and the
// instrument on top of this; the engine here is the only part that has to be
// fast.
//
// WHAT THE POSITION IS. A 64-bit fixed-point frame position, 32 bits of
// integer and 32 of fraction, stepped by `pitch * rate / sample_rate` in the
// same format. It is integer arithmetic so that three interpreters on three
// architectures land on the same frame with the same fraction every time,
// and so a loop of a few hundred frames played for an hour does not drift
// the way a float accumulator would. Only the fraction becomes a float, for
// the interpolator.
//
// WHY CUBIC. Linear interpolation of a sample played a fifth up puts the
// image of every partial above half the output rate back into the band,
// about 30 dB down; a 4-point Hermite puts it about 60 dB down for four taps
// and two more multiplies a sample. Measured on the desktop, 24 looping
// voices at 48 kHz stereo cost 26 us of a 5333 us block with the cubic
// (docs/upstream-diff.md), so there was no budget case for the cheaper one
// and the better one is the only one. The polynomial is the x-form 3rd-order
// Hermite, the one every sampler since the 1990s has used, chosen because
// it passes through the samples exactly: an integer position reads the
// buffer's own value, so a loop at pitch 1.0 is a wire.
//
// WHY THE ENVELOPE RUNS PER SAMPLE. An attack of two milliseconds is 96
// samples at 48 kHz, which is well under a block; an envelope that stepped
// per block would turn it into a stair of one step, and a release of ten
// milliseconds into a click. Six segments, linear in time, in the units a
// SoundFont writes them: delay, attack, hold, decay, sustain, release.
// `delay` postpones the start of the sound itself rather than holding the
// envelope at zero over a playing sample, so a drum machine can use it for a
// flam without a second voice.
//
// WHY LEVEL AND PAN RAMP ACROSS THE BLOCK. A fader moved from Python lands
// once a block; applied as a step it is a 190 Hz buzz at the fader's rate.
// The gain each sample uses walks from the value the last block ended on to
// the one asked for, so a move is a short slope rather than an edge.
//
// WHY STOP FADES, AND WHERE THE FADE PLAYS. A voice cut from mid-waveform
// to nothing is a step, and a step is a click: a choked hi-hat ticks, and a
// voice stolen for a new note ticks under it. So `stop()` hands what the
// voice is sounding to the voice's tail -- a second slot behind it -- which
// ramps it to zero over AUDIODSP_SAMPLER_FADE, while the voice itself is
// free at once. `play()` on a voice still sounding does the same, so the old
// note fades under the new one rather than being cut by it. The tail is a
// slot per voice rather than a pool so the binding knows, without asking,
// which buffer each one is reading.
//
// The pulling loop is not here, for the same reason it is not in
// audiodsp_modal.c: each runtime reaches its audio graph differently. Nor is
// any memory: the binding owns the voice table, the mix accumulator and the
// sample buffers, and tells the kernel where they are.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//: Frames one output block carries by default -- the 256 every other audiodsp
//: node uses -- and the most a binding may ask for.
#define AUDIODSP_SAMPLER_FRAMES 256u
#define AUDIODSP_SAMPLER_MAX_FRAMES 1024u

//: The most voices one sampler may carry. The budget target is 24 at 48 kHz
//: stereo; 64 leaves headroom without letting a Python mistake allocate
//: unboundedly.
#define AUDIODSP_SAMPLER_MAX_VOICES 64u

//: Pitch bounds, as a ratio. 1/64 is six octaves down; 64 is six up.
#define AUDIODSP_SAMPLER_MIN_PITCH (1.0f / 64.0f)
#define AUDIODSP_SAMPLER_MAX_PITCH 64.0f

//: The longest any one envelope stage may be, in seconds.
#define AUDIODSP_SAMPLER_MAX_STAGE 60.0f

//: How long `stop()`, or a `play()` that takes over a sounding voice, takes
//: to fade the old note to zero, in seconds. A linear ramp of T puts its
//: splatter below about 1/T: 5 ms is 200 Hz, under the band a click is heard
//: in, where 2 ms (500 Hz) still ticks on a loud sample. Longer smears a
//: choke into a release. 5 ms is also 240 frames at 48 kHz, inside one
//: 256-frame block, so the fade is over by the end of the block that heard
//: the stop. A note already releasing faster than this keeps its own pace.
#define AUDIODSP_SAMPLER_FADE 0.005f

//: How many voice slots a binding allocates for `voice_count` voices: each
//: voice, then each voice's tail, which plays out a stopped note's fade.
#define AUDIODSP_SAMPLER_SLOTS(voice_count) (2u * (voice_count))

//: How a voice treats `[loop_start, loop_end)`. The numbers are SoundFont's
//: `sampleModes` so an instrument reader writes the file's value straight
//: in; 2 is unused there and is refused here.
typedef enum {
    AUDIODSP_SAMPLER_LOOP_NONE = 0,          // play to the end and stop
    AUDIODSP_SAMPLER_LOOP_FOREVER = 1,       // loop until release() or stop()
    AUDIODSP_SAMPLER_LOOP_UNTIL_RELEASE = 3, // loop while held, then play on
} audiodsp_sampler_loop_t;

typedef enum {
    AUDIODSP_SAMPLER_STAGE_IDLE = 0,
    AUDIODSP_SAMPLER_STAGE_DELAY,
    AUDIODSP_SAMPLER_STAGE_ATTACK,
    AUDIODSP_SAMPLER_STAGE_HOLD,
    AUDIODSP_SAMPLER_STAGE_DECAY,
    AUDIODSP_SAMPLER_STAGE_SUSTAIN,
    AUDIODSP_SAMPLER_STAGE_RELEASE,
} audiodsp_sampler_stage_t;

//: Everything `play()` takes besides the buffer. `audiodsp_sampler_note_init`
//: fills the defaults; a binding overwrites what its caller named.
typedef struct {
    float rate;          // the buffer's own sample rate; 0 means the sampler's
    float pitch;         // playback ratio, 1.0 is the buffer's own pitch
    uint32_t loop_start; // frames
    uint32_t loop_end;   // frames, exclusive; 0 means the buffer's length
    uint32_t loop_mode;  // audiodsp_sampler_loop_t
    float level;         // 0..1
    float pan;           // -1 (left) .. 1 (right); ignored for mono output
    float delay, attack, hold, decay, release; // seconds
    float sustain;       // 0..1, linear
} audiodsp_sampler_note_t;

//: One voice. Written by the kernel only; a binding reads `stage` to answer
//: `playing` and to know when to let go of the buffer it handed over.
typedef struct {
    const int16_t *data;   // borrowed from the binding; NULL when idle
    uint32_t length;       // frames
    uint32_t loop_start;
    uint32_t loop_end;
    uint32_t loop_mode;
    uint32_t stage;        // audiodsp_sampler_stage_t
    bool looping;          // still wrapping at loop_end
    uint64_t position;     // Q32.32 frames
    uint64_t step;         // Q32.32 frames per output frame
    float rate_ratio;      // rate / sample_rate, so a pitch change is one multiply
    float pitch;
    // The envelope: where it is, how far it moves each sample, and how many
    // samples are left in the stage (unused in sustain).
    float env;
    float env_step;
    uint32_t stage_left;
    uint32_t attack_samples, hold_samples, decay_samples, release_samples;
    float sustain;
    // Where the fader is asked to be, and where the last block left it.
    float level;
    float pan;
    float gain[2];
} audiodsp_sampler_voice_t;

typedef struct {
    uint32_t sample_rate;
    uint32_t channel_count;
    uint32_t frames;
    uint32_t voice_count;
    uint32_t fade_samples;            // AUDIODSP_SAMPLER_FADE, at least 1
    // Borrowed: AUDIODSP_SAMPLER_SLOTS(voice_count) entries. Voice `i` is
    // `voices[i]` and its tail is `voices[voice_count + i]`.
    audiodsp_sampler_voice_t *voices;
    float *mix;                       // borrowed: frames * channel_count floats
} audiodsp_sampler_t;

void audiodsp_sampler_init(audiodsp_sampler_t *self, uint32_t sample_rate,
    uint32_t channel_count, uint32_t frames, uint32_t voice_count,
    audiodsp_sampler_voice_t *voices, float *mix);

void audiodsp_sampler_note_init(audiodsp_sampler_note_t *note);

// Starts `index` on `data`. A note the voice was still sounding moves to the
// voice's tail and fades out there, under the new one. (mono signed 16-bit, `length` frames). Values are
// clamped here, once, rather than in each binding. Returns false, and starts
// nothing, when the loop bounds do not fit the buffer, when the loop mode is
// not one of the three, or when the buffer is empty -- the bindings raise;
// a refused play() leaves the voice and its tail as they were.
bool audiodsp_sampler_play(audiodsp_sampler_t *self, uint32_t index,
    const int16_t *data, uint32_t length, const audiodsp_sampler_note_t *note);

// Enters the release stage from wherever the envelope is. In loop mode 3 the
// voice also stops wrapping and plays on to the end of the buffer.
void audiodsp_sampler_release(audiodsp_sampler_t *self, uint32_t index);

// Stops the voice: it is idle at once, and what it was sounding fades to
// zero on its tail over AUDIODSP_SAMPLER_FADE, or sooner if it was already
// releasing faster. A voice still in its delay, never heard, just stops.
void audiodsp_sampler_stop(audiodsp_sampler_t *self, uint32_t index);

// Silences the voice and its tail at once, no fade: for a binding letting go
// of every buffer, as deinit() does.
void audiodsp_sampler_cut(audiodsp_sampler_t *self, uint32_t index);

bool audiodsp_sampler_playing(const audiodsp_sampler_t *self, uint32_t index);

// True when a stop() or play() on `index` now would hand a note to its tail:
// the voice is past its delay and its envelope is above zero. A binding asks
// before the call, and on true moves its hold on the voice's buffer to the
// tail's.
bool audiodsp_sampler_audible(const audiodsp_sampler_t *self, uint32_t index);

// True while the voice's tail is still fading a note out. A binding lets go
// of the tail's buffer once a render leaves this false.
bool audiodsp_sampler_fading(const audiodsp_sampler_t *self, uint32_t index);

// Live changes. Level and pan land as a ramp across the next block; pitch
// lands at the next block.
void audiodsp_sampler_set_level(audiodsp_sampler_t *self, uint32_t index,
    float level);
void audiodsp_sampler_set_pan(audiodsp_sampler_t *self, uint32_t index,
    float pan);
void audiodsp_sampler_set_pitch(audiodsp_sampler_t *self, uint32_t index,
    float pitch);

// Mixes every sounding voice, and every fading tail, into `out`: `frames`
// interleaved frames of `channel_count` signed 16-bit samples. Silent when
// nothing sounds. Returns how many voices (tails not counted) are still
// sounding afterwards, so a binding can let go of the buffers of the ones
// that finished.
uint32_t audiodsp_sampler_render_s16(audiodsp_sampler_t *self, int16_t *out);
