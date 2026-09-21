// Ported from CircuitPython's shared-bindings/synthio/__init__.h and
// shared-module/synthio/__init__.h (upstream repo:
// https://github.com/adafruit/circuitpython, MIT), merged into one file
// (this port doesn't keep CP's shared-bindings/shared-module split).
// `py/enum.h` -> `cp_compat/enum.h`; `py/objnamedtuple.h` -> our own
// `cp_compat/namedtuple.h` for the Envelope type (see synthio/__init__.c).
//
// CIRCUITPY_SYNTHIO_MAX_CHANNELS: CP wires this from
// py/circuitpy_mpconfig.mk (default 2 -- the minimum polyphony CP
// guarantees on every port; its own boards raise it to 12, and
// raspberrypi to 24). This port deliberately does NOT inherit that 2.
//
// Raised 2 -> 14 on 2026-09-03 (Brad: "14 everywhere"). 2 was carried over
// as an upstream-matching marker, but micropython.cmake's own comment
// already calls it "*broken* for essentially any real patch" -- two notes
// held on a detuned 2-oscillator voice is 4 concurrent Notes -- and the
// excess is silently REFUSED, never stolen (see find_channel_with_note,
// __init__.c:361, and the dropped press at :425). Keeping a value our own
// build files describe as broken, purely to match upstream's number, buys
// a marker at the cost of any build that sets nothing starting broken.
//
// Raised 14 -> 64 on 2026-09-03 (Brad, #31). 14 was the number the drum
// kits needed -- cr78 holds exactly 14 permanent Notes. It was never the
// number the MELODIC library needs, because most of those press more than
// one Note per key: solina 9, jp8000 7, b3 and vp330 6, farfisa 5, four
// instruments 4, ten 3, twenty-four 2. A seven-key chord on solina wants
// 63 Notes. At 14 you got one key of it.
//
// The cost of 14 was not the occasional refused note. Measured over the
// parity sequence, 4743 of 7335 presses got a channel only by evicting a
// still-decaying note, and 69% of those victims were above 10% of full
// envelope level (median 51%). The ceiling was continuously chopping
// release tails. A b3 playing a seven-note chord measured 4.8 dB quieter
// at 14 than at 36 -- nearly half its energy missing.
//
// 64 covers a ten-finger chord on every instrument in the library plus
// headroom. It is a CHOICE, not a convergence point: steals keep falling
// until roughly N=196, so any value here is a sonic baseline rather than a
// fix that settles.
//
// What it costs: 18 bytes per channel on ARM, 20 on ESP32, 24 on 64-bit,
// so 14 -> 64 is about +900 bytes per Synthesizer on ARM -- 0.34% of a
// Pico's 264 KiB -- against render buffers each Synthesizer already
// allocates unconditionally (__init__.c:327-329). An idle channel costs a
// compare and a continue per block; unused voices are close to free.
//
// FIVE copies of this number exist and nothing makes them agree except
// tests/test_voice_ceiling_consistency.py. The other four:
// micropython.mk, micropython.cmake, src/cpython/synthio.py, and the
// default argument in src/cpython/_audiodsp.c. The CPython target does not
// read this header -- setup.py builds _audiodsp from src/cpython/ and
// src/shared/ only -- so THIS value is watched by no parity gate at all.
// Since 2026-09-06 tests/parity/verify_mixdown_knee.py does cross the
// mix-down knee and so notices a ceiling change (audiodsp#27), but only in
// src/cpython/synthio.py, the one copy the CPython target reads. A change
// that moves this line and not that one is still invisible to CI.
//
// NOT a sixth copy: _audiodsp.c's `0x0fffffff / (32768 * 2 - 28000)` is
// SYNTHIO_MIX_DOWN_SCALE(2) mirroring upstream CircuitPython's own
// two-channel default. A literal 2 that stays 2.
//
// The pinned oracle stays at 14, so material that crosses this ceiling is
// non-parity by construction. tests/parity/verify_mixdown_knee.py is that
// material, which is why its above-knee half is enforced against this port
// rather than against the oracle. The deviation is NOT yet recorded in
// docs/upstream-diff.md; writing it there needs Brad's word (open
// 2026-09-06).
//
// Override via CFLAGS_EXTRA=-DCIRCUITPY_SYNTHIO_MAX_CHANNELS=N for a board
// that needs fewer voices; on CMake ports that must be an environment
// variable (cmods/micropython/py/mkrules.cmake:79-86).
//
// SPDX-FileCopyrightText: Copyright (c) 2021 Artyom Skrobov
// SPDX-FileCopyrightText: Copyright (c) 2023 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#pragma once

#ifndef CIRCUITPY_SYNTHIO_MAX_CHANNELS
#define CIRCUITPY_SYNTHIO_MAX_CHANNELS (64)
#endif

#include "cp_compat/enum.h"
#include "cp_compat/namedtuple.h"
#include "shared/audiodsp_envelope.h"

#define SYNTHIO_WAVEFORM_SIZE 16384

typedef audiodsp_envelope_kind_t envelope_state_e;
#define SYNTHIO_ENVELOPE_STATE_ATTACK AUDIODSP_ENVELOPE_ATTACK
#define SYNTHIO_ENVELOPE_STATE_DECAY AUDIODSP_ENVELOPE_DECAY
#define SYNTHIO_ENVELOPE_STATE_SUSTAIN AUDIODSP_ENVELOPE_SUSTAIN
#define SYNTHIO_ENVELOPE_STATE_RELEASE AUDIODSP_ENVELOPE_RELEASE

typedef enum synthio_bend_mode_e {
    SYNTHIO_BEND_MODE_STATIC, SYNTHIO_BEND_MODE_VIBRATO, SYNTHIO_BEND_MODE_SWEEP, SYNTHIO_BEND_MODE_SWEEP_IN
} synthio_bend_mode_t;

extern const mp_obj_type_t synthio_note_state_type;
extern const cp_enum_obj_t bend_mode_VIBRATO_obj;
extern const mp_obj_type_t synthio_bend_mode_type;
// Deviation from upstream (see docs/upstream-diff.md, "synthio_synth_t:
// one typedef, not two"): CP's shared-bindings/__init__.h and
// shared-module/__init__.h each separately declare `typedef ...
// synthio_synth_t` once, but since separate translation units never see
// both, the fact that they're the *same* typedef repeated is never
// diagnosed anywhere upstream. This port merges both into this one file,
// which puts both declarations in the same header -- fine under GNU
// extensions/C11, but a hard error under emscripten's strict `-std=c99
// -Werror`. This forward declaration is the only `typedef` for the name;
// the full definition below (struct synthio_synth { ... };) fills in the
// same tag without repeating it.
typedef struct synthio_synth synthio_synth_t;
extern int16_t shared_bindings_synthio_square_wave[];
extern const mp_obj_namedtuple_type_t synthio_envelope_type_obj;
void synthio_synth_envelope_set(synthio_synth_t *synth, mp_obj_t envelope_obj);
mp_obj_t synthio_synth_envelope_get(synthio_synth_t *synth);
mp_float_t common_hal_synthio_midi_to_hz_float(mp_float_t note);
mp_float_t common_hal_synthio_voct_to_hz_float(mp_float_t note);

// --- from shared-module/synthio/__init__.h --------------------------------

#define SYNTHIO_BITS_PER_SAMPLE (16)
#define SYNTHIO_BYTES_PER_SAMPLE (SYNTHIO_BITS_PER_SAMPLE / 8)
#define SYNTHIO_MAX_DUR (256)
#define SYNTHIO_SILENCE (mp_const_none)
#define SYNTHIO_NOTE_IS_SIMPLE(note) (mp_obj_is_small_int(note))
#define SYNTHIO_NOTE_IS_PLAYING(synth, i) ((synth)->envelope_state[(i)].state != SYNTHIO_ENVELOPE_STATE_RELEASE)
#define SYNTHIO_FREQUENCY_SHIFT (16)

#define SYNTHIO_MIX_DOWN_RANGE_LOW (-28000)
#define SYNTHIO_MIX_DOWN_RANGE_HIGH (28000)
#define SYNTHIO_MIX_DOWN_SCALE(x) (0xfffffff / (32768 * x - SYNTHIO_MIX_DOWN_RANGE_HIGH))

#include "audiocore/__init__.h"
// Deviation from upstream: CP's shared-module/synthio/__init__.h also
// includes shared-bindings/synthio/Biquad.h here, but nothing in this
// header actually names a Biquad type (only synthio/__init__.c's
// synth_note_into_buffer calls into Biquad, and it includes synthio/Biquad.h
// itself). Our merged Biquad.h (bindings+module in one file, unlike CP's
// split) pulls in block.h, which pulls in this file -- so including it here
// would be circular for no reason, since it isn't needed here.

typedef struct {
    uint16_t dur;
    mp_obj_t note_obj[CIRCUITPY_SYNTHIO_MAX_CHANNELS];
} synthio_midi_span_t;

typedef audiodsp_envelope_definition_t synthio_envelope_definition_t;
typedef audiodsp_envelope_state_t synthio_envelope_state_t;

struct synthio_synth {
    audiosample_base_t base;
    uint32_t total_envelope;
    int16_t *buffers[2];
    uint16_t buffer_length;
    uint16_t last_buffer_length;
    uint8_t other_channel, buffer_index, other_buffer_index;
    mp_buffer_info_t waveform_bufinfo;
    synthio_envelope_definition_t global_envelope_definition;
    mp_obj_t waveform_obj, filter_obj, envelope_obj;
    synthio_midi_span_t span;
    uint32_t accum[CIRCUITPY_SYNTHIO_MAX_CHANNELS];
    uint32_t ring_accum[CIRCUITPY_SYNTHIO_MAX_CHANNELS];
    synthio_envelope_state_t envelope_state[CIRCUITPY_SYNTHIO_MAX_CHANNELS];
    int16_t active_loudness[CIRCUITPY_SYNTHIO_MAX_CHANNELS][2];
};

typedef struct {
    mp_float_t amplitude, frequency;
} synthio_lfo_descr_t;

typedef struct {
    int32_t amplitude_scaled;
    uint32_t offset_scaled, dds, phase;
} synthio_lfo_state_t;


void synthio_synth_synthesize(synthio_synth_t *synth, uint8_t **buffer, uint32_t *buffer_length, uint8_t channel);
void synthio_synth_deinit(synthio_synth_t *synth);
bool synthio_synth_deinited(synthio_synth_t *synth);
void synthio_synth_init(synthio_synth_t *synth, uint32_t sample_rate, int channel_count, mp_obj_t waveform_obj, mp_obj_t envelope);
void synthio_synth_reset_buffer(synthio_synth_t *synth, bool single_channel_output, uint8_t channel);
void synthio_synth_parse_waveform(mp_buffer_info_t *bufinfo_waveform, mp_obj_t waveform_obj);
void synthio_synth_parse_filter(mp_buffer_info_t *bufinfo_filter, mp_obj_t filter_obj);
void synthio_synth_parse_envelope(uint16_t *envelope_sustain_index, mp_buffer_info_t *bufinfo_envelope, mp_obj_t envelope_obj, mp_obj_t envelope_hold_obj);

bool synthio_span_change_note(synthio_synth_t *synth, mp_obj_t old_note, mp_obj_t new_note);

void synthio_envelope_step(synthio_envelope_definition_t *definition, synthio_envelope_state_t *state, int n_samples);
void synthio_envelope_definition_set(synthio_envelope_definition_t *envelope, mp_obj_t obj, uint32_t sample_rate);

int16_t synthio_mix_down_sample(int32_t sample, int32_t scale);

uint64_t synthio_frequency_convert_float_to_scaled(mp_float_t frequency_hz);
uint32_t synthio_frequency_convert_float_to_dds(mp_float_t frequency_hz, int32_t sample_rate);
uint32_t synthio_frequency_convert_scaled_to_dds(uint64_t frequency_scaled, int32_t sample_rate);

void synthio_lfo_set(synthio_lfo_state_t *state, const synthio_lfo_descr_t *descr, uint32_t sample_rate);
int synthio_lfo_step(synthio_lfo_state_t *state, uint16_t dur);
int synthio_sweep_step(synthio_lfo_state_t *state, uint16_t dur);
int synthio_sweep_in_step(synthio_lfo_state_t *state, uint16_t dur);

extern mp_float_t synthio_global_rate_scale, synthio_global_W_scale;
extern uint8_t synthio_global_tick;
void shared_bindings_synthio_lfo_tick(uint32_t sample_rate, uint16_t num_samples);

int16_t synthio_sat16(int32_t n, int rshift);
