// Runtime-neutral six-operator FM synthesis: the operators, their envelopes,
// the LFO and pitch envelope, the 32 algorithms with feedback, and a pool of
// voices that plays the classic six-operator voice format.
//
// Adapted from Music Synthesizer for Android (MSFA),
// https://github.com/google/music-synthesizer-for-android at f67d41d, from
// dx7note, env, lfo, pitchenv, fm_core, fm_op_kernel, exp2, sin, freqlut,
// patch and synth_unit. Changes from those files: ported from C++ to C;
// every table that depended on the sample rate moved into a per-synth
// struct, so two synths at different rates can run side by side; the
// sine and exp2 tables are precomputed constants (audiodsp_fm_tables.h);
// the frequency lookup is computed from the exp2 table; envelope steps
// scale with the sample rate; a detune step varies with the key, as
// measured (tools/fm_tables.py); the feedback loops
// that span two or three operators (algorithms 4 and 6) now run; added transpose,
// oscillator key sync, operator on/off, amplitude modulation, the mod wheel,
// a pitch-bend range, voice stealing and an end-of-note test; parameters are
// clamped to their ranges before any table is indexed with them; and the
// voice-format packing is written out in both directions.
//
// Like the other audiodsp kernels, this file never allocates: the binding
// hands over the voice array, and the pulling loop lives in each runtime's
// binding.
//
// Copyright 2012 Google Inc.
// Copyright 2026 Brad Barnett
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//: Frames one output block carries, the same 256 every audiodsp node uses.
#define AUDIODSP_FM_FRAMES 256u

//: Frames between control updates (envelopes, LFO, pitch). Every render is a
//: whole number of these.
#define AUDIODSP_FM_LG_N 6
#define AUDIODSP_FM_N (1 << AUDIODSP_FM_LG_N)

//: The most voices one synth may hold.
#define AUDIODSP_FM_MAX_VOICES 64u

//: One voice in the unpacked format: 155 parameters, then one byte of
//: operator on/off bits this module keeps beside them.
#define AUDIODSP_FM_VOICE_BYTES 155u
#define AUDIODSP_FM_PATCH_BYTES 156u
//: One voice packed the way a 32-voice bank stores it.
#define AUDIODSP_FM_PACKED_BYTES 128u
//: A 32-voice bank: the packed voices, and the same wrapped in its system
//: exclusive header, checksum and end byte.
#define AUDIODSP_FM_BANK_VOICES 32u
#define AUDIODSP_FM_BANK_DATA_BYTES 4096u
#define AUDIODSP_FM_BANK_SYSEX_BYTES 4104u
//: One voice wrapped in its single-voice system exclusive message.
#define AUDIODSP_FM_VOICE_SYSEX_BYTES 163u

//: Byte offsets in the unpacked format. Operators are stored sixth first:
//: operator n (1..6) starts at (6 - n) * 21.
#define AUDIODSP_FM_OP_BYTES 21u
#define AUDIODSP_FM_PITCH_RATES 126u
#define AUDIODSP_FM_PITCH_LEVELS 130u
#define AUDIODSP_FM_ALGORITHM 134u
#define AUDIODSP_FM_FEEDBACK 135u
#define AUDIODSP_FM_KEY_SYNC 136u
#define AUDIODSP_FM_LFO_SPEED 137u
#define AUDIODSP_FM_LFO_DELAY 138u
#define AUDIODSP_FM_LFO_PITCH_DEPTH 139u
#define AUDIODSP_FM_LFO_AMP_DEPTH 140u
#define AUDIODSP_FM_LFO_KEY_SYNC 141u
#define AUDIODSP_FM_LFO_WAVE 142u
#define AUDIODSP_FM_LFO_PITCH_SENS 143u
#define AUDIODSP_FM_TRANSPOSE 144u
#define AUDIODSP_FM_NAME 145u
#define AUDIODSP_FM_NAME_BYTES 10u
#define AUDIODSP_FM_OP_ENABLE 155u

//: Offsets inside one operator's 21 bytes.
#define AUDIODSP_FM_OP_RATES 0u
#define AUDIODSP_FM_OP_LEVELS 4u
#define AUDIODSP_FM_OP_BREAKPOINT 8u
#define AUDIODSP_FM_OP_LEFT_DEPTH 9u
#define AUDIODSP_FM_OP_RIGHT_DEPTH 10u
#define AUDIODSP_FM_OP_LEFT_CURVE 11u
#define AUDIODSP_FM_OP_RIGHT_CURVE 12u
#define AUDIODSP_FM_OP_RATE_SCALING 13u
#define AUDIODSP_FM_OP_AMP_MOD 14u
#define AUDIODSP_FM_OP_VELOCITY 15u
#define AUDIODSP_FM_OP_LEVEL 16u
#define AUDIODSP_FM_OP_FIXED 17u
#define AUDIODSP_FM_OP_COARSE 18u
#define AUDIODSP_FM_OP_FINE 19u
#define AUDIODSP_FM_OP_DETUNE 20u

//: The highest value each of the 156 bytes may hold.
extern const uint8_t audiodsp_fm_patch_max[AUDIODSP_FM_PATCH_BYTES];

typedef struct {
    int32_t level;      // Q24 log2, 16 bits finer than the format's own
    int32_t target;
    int32_t inc;
    int32_t outlevel;
    uint32_t unit;      // 44100 / sample rate, Q16: scales every step
    uint8_t rates[4];
    uint8_t levels[4];
    int8_t rate_scaling;
    uint8_t stage;      // 0..3 while moving, 4 when finished
    bool rising;
    bool down;
} audiodsp_fm_env_t;

typedef struct {
    int32_t level;      // Q24 per octave
    int32_t target;
    int32_t inc;
    uint8_t rates[4];
    uint8_t levels[4];
    uint8_t stage;
    bool rising;
    bool down;
} audiodsp_fm_pitchenv_t;

typedef struct {
    int32_t gain[2];    // the last block's gain and this one's, ramped across
    int32_t freq;       // phase step per frame, Q24 of a cycle
    int32_t phase;
} audiodsp_fm_op_t;

typedef struct {
    audiodsp_fm_env_t env[6];
    audiodsp_fm_op_t op[6];
    audiodsp_fm_pitchenv_t pitchenv;
    int32_t basepitch[6];
    int32_t fb[2];
    uint32_t age;
    int16_t fb_shift;
    uint8_t algorithm;
    uint8_t enabled;    // bit k: the operator stored at k * 21
    uint8_t amp_mod[6];
    uint8_t pitch_depth;
    uint8_t amp_depth;
    uint8_t pitch_sens;
    uint8_t note;       // the key as pressed, before transpose
    bool keydown;
    bool sustained;
    bool live;
} audiodsp_fm_voice_t;

typedef struct {
    uint32_t phase;
    uint32_t delta;
    uint32_t delay_state;
    uint32_t delay_inc;
    uint32_t delay_inc2;
    uint8_t wave;
    uint8_t random;
    bool sync;
} audiodsp_fm_lfo_t;

typedef struct {
    uint32_t sample_rate;
    uint32_t channel_count;
    uint32_t voice_count;
    // Rate-dependent constants, set once by audiodsp_fm_init().
    uint32_t lfo_unit;
    uint32_t env_unit;      // 44100 / sample_rate, Q16
    int32_t pitchenv_unit;
    uint32_t freq_scale;    // 2**40 / sample_rate
    audiodsp_fm_lfo_t lfo;
    int32_t bend;           // Q24 per octave
    uint8_t mod_wheel;      // 0..127
    bool sustain;
    uint32_t next_voice;
    uint32_t clock;
    audiodsp_fm_voice_t *voices;    // borrowed, voice_count of them
    int32_t bus[2][AUDIODSP_FM_N];
    int32_t voice_out[AUDIODSP_FM_N];
    int32_t mix[AUDIODSP_FM_N];
} audiodsp_fm_synth_t;

// Sets up a synth over `voices` (voice_count of them, owned by the binding).
// Every voice starts silent.
void audiodsp_fm_init(audiodsp_fm_synth_t *synth, uint32_t sample_rate,
    uint32_t channel_count, audiodsp_fm_voice_t *voices,
    uint32_t voice_count);

// Starts `note` (0..127) at `velocity` (0..127) with `patch`, which is
// AUDIODSP_FM_PATCH_BYTES in the unpacked format. Values past a parameter's
// range are clamped. A free voice is taken if there is one; otherwise the
// oldest released voice, then the oldest held one.
void audiodsp_fm_press(audiodsp_fm_synth_t *synth, const uint8_t *patch,
    uint8_t note, uint8_t velocity);

// Releases every held voice playing `note`, or marks it sustained while the
// sustain pedal is down.
void audiodsp_fm_release(audiodsp_fm_synth_t *synth, uint8_t note);

// Releases every held voice.
void audiodsp_fm_release_all(audiodsp_fm_synth_t *synth);

// Silences every voice at once and clears the feedback memory.
void audiodsp_fm_reset(audiodsp_fm_synth_t *synth);

// The pedal. Lifting it releases every voice it was holding.
void audiodsp_fm_set_sustain(audiodsp_fm_synth_t *synth, bool down);

// Pitch bend as a Q24-per-octave offset, applied to every voice.
void audiodsp_fm_set_bend(audiodsp_fm_synth_t *synth, int32_t bend_q24);

// The mod wheel, 0..127. It adds LFO pitch and amplitude depth on top of the
// voice's own, up to full depth.
void audiodsp_fm_set_mod_wheel(audiodsp_fm_synth_t *synth, uint8_t value);

// How many voices are sounding.
uint32_t audiodsp_fm_active(const audiodsp_fm_synth_t *synth);

// Renders `frames` frames of interleaved 16-bit audio. `frames` is rounded
// down to a whole number of AUDIODSP_FM_N.
void audiodsp_fm_render_s16(audiodsp_fm_synth_t *synth, int16_t *out,
    uint32_t frames);

// Clamps every byte of an unpacked voice to its parameter's range, in place.
void audiodsp_fm_clamp(uint8_t *patch);

// Packs one unpacked voice (the first 155 bytes are read) into the 128 bytes
// a bank stores, and back. Unpacking sets every operator on.
void audiodsp_fm_pack_voice(const uint8_t *patch, uint8_t *packed);
void audiodsp_fm_unpack_voice(const uint8_t *packed, uint8_t *patch);

// The system exclusive checksum over `length` data bytes.
uint8_t audiodsp_fm_checksum(const uint8_t *data, size_t length);

// Writes the initial voice: one sine on operator 1 at full level.
void audiodsp_fm_init_voice(uint8_t *patch);
