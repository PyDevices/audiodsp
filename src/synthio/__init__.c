// Ported from CircuitPython's shared-module/synthio/__init__.c and
// shared-bindings/synthio/__init__.c (upstream repo:
// https://github.com/adafruit/circuitpython, MIT), merged into one file.
// Docstrings dropped. `m_malloc_without_collect` -> `m_malloc` (mainline
// has no "won't be GC-scanned" allocation variant; see docs/upstream-diff.md
// tier-1 notes on the same substitution in audiocore's WaveFile). The
// `synthio.from_file()` MIDI-SMF loader is adapted to read through
// MicroPython's generic stream protocol instead of CP's direct FatFS
// calls, exactly like this port's audiocore.WaveFile -- same rationale,
// see docs/upstream-diff.md. `lfo_tick()` is ported unconditionally (CP
// gates it behind CIRCUITPY_AUDIOCORE_DEBUG) since it's exactly the
// primitive this port's oracle-diff test strategy needs for Math/LFO, the
// same reasoning as audiocore.get_buffer in tier 1.
//
// SPDX-FileCopyrightText: Copyright (c) 2021 Artyom Skrobov
// SPDX-FileCopyrightText: Copyright (c) 2023 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cp_compat/argcheck.h"
#include "cp_compat/enum.h"
#include "cp_compat/namedtuple.h"
#include "cp_compat/objproperty.h"
#include "cp_compat/util.h"
#include "synthio/Biquad.h"
#include "synthio/LFO.h"
#include "synthio/Math.h"
#include "synthio/MidiTrack.h"
#include "synthio/Note.h"
#include "synthio/Synthesizer.h"
#include "synthio/__init__.h"
#include "shared/audiodsp_synth_dsp.h"

#include "py/builtin.h"
#include "py/mperrno.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "shared/audiodsp_pump_lock.h"

#define MP_PI MICROPY_FLOAT_CONST(3.14159265358979323846)

// --- from shared-module/synthio/__init__.c --------------------------------

mp_float_t synthio_global_rate_scale, synthio_global_W_scale;
uint8_t synthio_global_tick;

static const int16_t square_wave[] = {-32768, 32767};

static const uint16_t notes[] = {8372, 8870, 9397, 9956, 10548, 11175, 11840,
                                 12544, 13290, 14080, 14917, 15804}; // 9th octave

// cleaner sat16 by http://www.moseleyinstruments.com/
int16_t synthio_sat16(int32_t n, int rshift) {
    return audiodsp_sat16(n, rshift);
}

static int64_t round_float_to_int64(mp_float_t f) {
    return (int64_t)(f + MICROPY_FLOAT_CONST(0.5));
}

mp_float_t common_hal_synthio_midi_to_hz_float(mp_float_t arg) {
    return common_hal_synthio_voct_to_hz_float(arg / MICROPY_FLOAT_CONST(12.) - 3);
}

mp_float_t common_hal_synthio_voct_to_hz_float(mp_float_t octave) {
    return notes[0] * MICROPY_FLOAT_C_FUN(pow)(2., octave - 7);
}

// Five fields -- attack_step, decay_step, release_step, attack_level,
// sustain_level -- that synthio_envelope_step reads together every block for
// every sounding channel. Half old and half new is an envelope that is not
// any envelope. The float reads and the type check happen first, outside the
// lock, because both can raise; then the five writes are one act.
//
// Reached from three places (the Synthesizer's envelope setter, Note's, and
// synthio_note_start on the PULL path) -- which is why the mutex is
// recursive: the pump is already holding it when the third one calls.
void synthio_envelope_definition_set(synthio_envelope_definition_t *envelope, mp_obj_t obj, uint32_t sample_rate) {
    if (obj == mp_const_none) {
        audiodsp_pump_lock_acquire();
        audiodsp_envelope_definition_init(envelope, sample_rate, false,
            0, 0, 0, 1, 1);
        audiodsp_pump_lock_release();
        return;
    }
    mp_arg_validate_type(obj, (mp_obj_type_t *)&synthio_envelope_type_obj, MP_QSTR_envelope);

    size_t len;
    mp_obj_t *fields;
    mp_obj_tuple_get(obj, &len, &fields);

    const mp_float_t a = mp_obj_get_float(fields[0]);
    const mp_float_t d = mp_obj_get_float(fields[1]);
    const mp_float_t r = mp_obj_get_float(fields[2]);
    const mp_float_t al = mp_obj_get_float(fields[3]);
    const mp_float_t sl = mp_obj_get_float(fields[4]);
    audiodsp_pump_lock_acquire();
    audiodsp_envelope_definition_init(envelope, sample_rate, true,
        a, d, r, al, sl);
    audiodsp_pump_lock_release();
}

static void synthio_envelope_state_step(synthio_envelope_state_t *state, synthio_envelope_definition_t *def, size_t n_steps) {
    audiodsp_envelope_state_step(state, def, n_steps);
}

static void synthio_envelope_state_init(synthio_envelope_state_t *state, synthio_envelope_definition_t *def) {
    audiodsp_envelope_state_init(state, def);
}

static void synthio_envelope_state_release(synthio_envelope_state_t *state, synthio_envelope_definition_t *def) {
    (void)def;
    audiodsp_envelope_state_release(state);
}

static synthio_envelope_definition_t *synthio_synth_get_note_envelope(synthio_synth_t *synth, mp_obj_t note_obj) {
    synthio_envelope_definition_t *def = &synth->global_envelope_definition;
    if (!mp_obj_is_small_int(note_obj)) {
        synthio_note_obj_t *note = MP_OBJ_TO_PTR(note_obj);
        if (note->envelope_obj != mp_const_none) {
            def = &note->envelope_def;
        }
    }
    return def;
}


#define RANGE_SHIFT (16)

int16_t synthio_mix_down_sample(int32_t sample, int32_t scale) {
    return audiodsp_mix_down_sample(sample, scale,
        SYNTHIO_MIX_DOWN_RANGE_LOW, SYNTHIO_MIX_DOWN_RANGE_HIGH);
}

static bool synth_note_into_buffer(synthio_synth_t *synth, int chan, int32_t *out_buffer32, int16_t dur, int16_t loudness[2]) {
    mp_obj_t note_obj = synth->span.note_obj[chan];

    int32_t sample_rate = synth->base.sample_rate;


    uint32_t dds_rate;
    const int16_t *waveform = synth->waveform_bufinfo.buf;
    uint32_t waveform_start = 0;
    uint32_t waveform_length = synth->waveform_bufinfo.len;

    uint32_t ring_dds_rate = 0;
    const int16_t *ring_waveform = NULL;
    uint32_t ring_waveform_start = 0;
    uint32_t ring_waveform_length = 0;

    if (mp_obj_is_small_int(note_obj)) {
        uint8_t note = mp_obj_get_int(note_obj);
        uint8_t octave = note / 12;
        uint16_t base_freq = notes[note % 12];
        dds_rate = (sample_rate / 2 + ((uint64_t)(base_freq * waveform_length) << (SYNTHIO_FREQUENCY_SHIFT - 10 + octave))) / sample_rate;
    } else {
        synthio_note_obj_t *note = MP_OBJ_TO_PTR(note_obj);
        int32_t frequency_scaled = synthio_note_step(note, sample_rate, dur, loudness);
        if (note->waveform_buf.buf) {
            waveform = note->waveform_buf.buf;
            waveform_length = note->waveform_buf.len;
            waveform_start = (uint32_t)synthio_block_slot_get_limited(&note->waveform_loop_start, 0, waveform_length - 1);
            waveform_length = (uint32_t)synthio_block_slot_get_limited(&note->waveform_loop_end, waveform_start + 1, waveform_length);
        }
        dds_rate = synthio_frequency_convert_scaled_to_dds((uint64_t)frequency_scaled * (waveform_length - waveform_start), sample_rate);
        if (note->ring_frequency_scaled != 0 && note->ring_waveform_buf.buf) {
            ring_waveform = note->ring_waveform_buf.buf;
            ring_waveform_length = note->ring_waveform_buf.len;
            ring_waveform_start = (uint32_t)synthio_block_slot_get_limited(&note->ring_waveform_loop_start, 0, ring_waveform_length - 1);
            ring_waveform_length = (uint32_t)synthio_block_slot_get_limited(&note->ring_waveform_loop_end, ring_waveform_start + 1, ring_waveform_length);
            ring_dds_rate = synthio_frequency_convert_scaled_to_dds((uint64_t)note->ring_frequency_bent * (ring_waveform_length - ring_waveform_start), sample_rate);
            uint32_t lim = ring_waveform_length << SYNTHIO_FREQUENCY_SHIFT;
            if (ring_dds_rate > lim / sizeof(int16_t)) {
                ring_dds_rate = 0;
            }
        }
    }

    uint32_t lim = waveform_length << SYNTHIO_FREQUENCY_SHIFT;
    if (!audiodsp_oscillator_fill(out_buffer32, waveform, waveform_start,
        waveform_length, dds_rate, &synth->accum[chan], dur,
        SYNTHIO_FREQUENCY_SHIFT)) {
        return false;
    }

    if (ring_dds_rate) {
        uint32_t offset;
        uint32_t accum;
        if (ring_dds_rate > lim / 2) {
            return true;
        }

        accum = synth->ring_accum[chan];
        offset = ring_waveform_start << SYNTHIO_FREQUENCY_SHIFT;
        lim = ring_waveform_length << SYNTHIO_FREQUENCY_SHIFT;

        // Wrap on `>=`, not CircuitPython's `>`; see the note in
        // audiodsp_oscillator_fill() and docs/upstream-diff.md.
        uint32_t ring_span = lim - offset;
        if (accum >= lim) {
            accum = offset + (accum - offset) % ring_span;
        }

        for (uint16_t i = 0; i < dur; i++) {
            accum += ring_dds_rate;
            if (accum >= lim) {
                accum -= ring_span;
            }
            int16_t idx = accum >> SYNTHIO_FREQUENCY_SHIFT;
            int16_t wi = (ring_waveform[idx] * out_buffer32[i]) / 32768;
            out_buffer32[i] = wi;
        }
        synth->ring_accum[chan] = accum;
    }
    return true;
}

static mp_obj_t synthio_synth_get_note_filter(mp_obj_t note_obj) {
    if (note_obj == mp_const_none) {
        return mp_const_none;
    }
    if (!mp_obj_is_small_int(note_obj)) {
        synthio_note_obj_t *note = MP_OBJ_TO_PTR(note_obj);
        return note->filter_obj;
    }
    return mp_const_none;
}

static void sum_with_loudness(int32_t *out_buffer32, int32_t *tmp_buffer32, int16_t active_loudness[2], int16_t loudness[2], size_t dur, int synth_chan) {
    audiodsp_sum_with_loudness(out_buffer32, tmp_buffer32, active_loudness,
        loudness, dur, synth_chan);
}

void synthio_synth_synthesize(synthio_synth_t *synth, uint8_t **bufptr, uint32_t *buffer_length, uint8_t channel) {

    if (channel == synth->other_channel) {
        *buffer_length = synth->last_buffer_length;
        *bufptr = (uint8_t *)(synth->buffers[synth->other_buffer_index] + channel);
        return;
    }

    shared_bindings_synthio_lfo_tick(synth->base.sample_rate, SYNTHIO_MAX_DUR);

    synth->buffer_index = !synth->buffer_index;
    synth->other_channel = 1 - channel;
    synth->other_buffer_index = synth->buffer_index;

    uint16_t dur = MIN(SYNTHIO_MAX_DUR, synth->span.dur);
    synth->span.dur -= dur;

    int32_t out_buffer32[SYNTHIO_MAX_DUR * synth->base.channel_count];
    int32_t tmp_buffer32[SYNTHIO_MAX_DUR];
    memset(out_buffer32, 0, synth->base.channel_count * dur * sizeof(int32_t));

    for (int chan = 0; chan < CIRCUITPY_SYNTHIO_MAX_CHANNELS; chan++) {
        mp_obj_t note_obj = synth->span.note_obj[chan];
        if (note_obj == SYNTHIO_SILENCE) {
            continue;
        }

        if (synth->envelope_state[chan].level == 0) {
            synth->span.note_obj[chan] = SYNTHIO_SILENCE;
            continue;
        }

        int16_t loudness[2] = {synth->envelope_state[chan].level, synth->envelope_state[chan].level};

        if (!synth_note_into_buffer(synth, chan, tmp_buffer32, dur, loudness)) {
            continue;
        }

        mp_obj_t filter_obj = synthio_synth_get_note_filter(note_obj);
        if (filter_obj != mp_const_none) {
            synthio_note_obj_t *note = MP_OBJ_TO_PTR(note_obj);
            if (mp_obj_is_type(filter_obj, &mp_type_tuple) || mp_obj_is_type(filter_obj, &mp_type_list)) {
                // audiodsp extension (#11): serial cascade, one state per stage
                size_t n_stages;
                mp_obj_t *stages;
                mp_obj_get_array(filter_obj, &n_stages, &stages);
                if (n_stages > SYNTHIO_NOTE_MAX_FILTER_STAGES) {
                    n_stages = SYNTHIO_NOTE_MAX_FILTER_STAGES;
                }
                for (size_t s = 0; s < n_stages; s++) {
                    common_hal_synthio_biquad_tick(stages[s]);
                    synthio_biquad_filter_samples(stages[s], &note->filter_state[s], tmp_buffer32, dur);
                }
            } else {
                common_hal_synthio_biquad_tick(filter_obj);
                synthio_biquad_filter_samples(filter_obj, &note->filter_state[0], tmp_buffer32, dur);
            }
        }

        sum_with_loudness(out_buffer32, tmp_buffer32, synth->active_loudness[chan], loudness, dur, synth->base.channel_count);
    }

    int16_t *out_buffer16 = (int16_t *)(void *)synth->buffers[synth->buffer_index];

    for (size_t i = 0; i < dur * synth->base.channel_count; i++) {
        int32_t sample = out_buffer32[i];
        out_buffer16[i] = synthio_mix_down_sample(sample, SYNTHIO_MIX_DOWN_SCALE(CIRCUITPY_SYNTHIO_MAX_CHANNELS));
    }

    for (int chan = 0; chan < CIRCUITPY_SYNTHIO_MAX_CHANNELS; chan++) {
        mp_obj_t note_obj = synth->span.note_obj[chan];
        if (note_obj == SYNTHIO_SILENCE) {
            continue;
        }
        synthio_envelope_state_step(&synth->envelope_state[chan], synthio_synth_get_note_envelope(synth, note_obj), dur);
    }

    *buffer_length = synth->last_buffer_length = dur * SYNTHIO_BYTES_PER_SAMPLE * synth->base.channel_count;
    *bufptr = (uint8_t *)out_buffer16;
}

void synthio_synth_reset_buffer(synthio_synth_t *synth, bool single_channel_output, uint8_t channel) {
    if (single_channel_output && channel == 1) {
        return;
    }
    synth->other_channel = -1;
}

void synthio_synth_deinit(synthio_synth_t *synth) {
    // synthio_synthesizer_get_buffer checks deinited FIRST, so without this
    // the two NULLs land after the check has let a pull through, and
    // synthio_synth_synthesize returns buffers[buffer_index] -- NULL -- and
    // writes into it. Shared with MidiTrack, which is the other caller.
    audiodsp_pump_lock_acquire();
    synth->buffers[0] = NULL;
    synth->buffers[1] = NULL;
    audiosample_mark_deinit(&synth->base);
    audiodsp_pump_lock_release();
}

void synthio_synth_envelope_set(synthio_synth_t *synth, mp_obj_t envelope_obj) {
    synthio_envelope_definition_set(&synth->global_envelope_definition, envelope_obj, synth->base.sample_rate);
    // One word, and the definition it names is already published, so the pull
    // can only see the pair agree or the old pair.
    synth->envelope_obj = envelope_obj;
}

mp_obj_t synthio_synth_envelope_get(synthio_synth_t *synth) {
    return synth->envelope_obj;
}

void synthio_synth_init(synthio_synth_t *synth, uint32_t sample_rate, int channel_count, mp_obj_t waveform_obj, mp_obj_t envelope_obj) {
    synthio_synth_parse_waveform(&synth->waveform_bufinfo, waveform_obj);
    mp_arg_validate_int_range(channel_count, 1, 2, MP_QSTR_channel_count);
    synth->buffer_length = SYNTHIO_MAX_DUR * SYNTHIO_BYTES_PER_SAMPLE * channel_count;
    synth->buffers[0] = m_malloc(synth->buffer_length);
    synth->buffers[1] = m_malloc(synth->buffer_length);
    synth->base.channel_count = channel_count;
    synth->base.single_buffer = false;
    synth->other_channel = -1;
    synth->waveform_obj = waveform_obj;
    synth->base.sample_rate = sample_rate;
    synth->base.bits_per_sample = 16;
    synth->base.samples_signed = true;
    synth->base.max_buffer_length = synth->buffer_length;
    synthio_synth_envelope_set(synth, envelope_obj);

    synth->refused = 0;

    for (size_t i = 0; i < CIRCUITPY_SYNTHIO_MAX_CHANNELS; i++) {
        synth->span.note_obj[i] = SYNTHIO_SILENCE;
    }
}

static void parse_common(mp_buffer_info_t *bufinfo, mp_obj_t o, int16_t what, mp_int_t max_len) {
    if (o != mp_const_none) {
        mp_get_buffer_raise(o, bufinfo, MP_BUFFER_READ);
        if (bufinfo->typecode != 'h') {
            mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be array of type 'h'"), what);
        }
        bufinfo->len /= 2;
        mp_arg_validate_length_range(bufinfo->len, 2, max_len, what);
    }
}

void synthio_synth_parse_waveform(mp_buffer_info_t *bufinfo_waveform, mp_obj_t waveform_obj) {
    *bufinfo_waveform = ((mp_buffer_info_t) { .buf = (void *)square_wave, .len = 2 });
    parse_common(bufinfo_waveform, waveform_obj, MP_QSTR_waveform, SYNTHIO_WAVEFORM_SIZE);
}

static int find_channel_with_note(synthio_synth_t *synth, mp_obj_t note) {
    for (int i = 0; i < CIRCUITPY_SYNTHIO_MAX_CHANNELS; i++) {
        if (synth->span.note_obj[i] == note) {
            return i;
        }
    }
    int result = -1;
    if (note == SYNTHIO_SILENCE) {
        int level = 32768;
        for (int chan = 0; chan < CIRCUITPY_SYNTHIO_MAX_CHANNELS; chan++) {
            if (!SYNTHIO_NOTE_IS_PLAYING(synth, chan)) {
                synthio_envelope_state_t *state = &synth->envelope_state[chan];
                if (state->level < level) {
                    result = chan;
                    level = state->level;
                }
            }
        }
    }
    return result;
}

bool synthio_span_change_note(synthio_synth_t *synth, mp_obj_t old_note, mp_obj_t new_note) {
    // The whole body, because it is the choke point under press(),
    // release(), change(), release_all() and release_all_then_press(),
    // and it writes envelope_state[chan].{level,substep,state}, accum[chan]
    // and span.note_obj[chan] -- which synthio_synth_synthesize reads
    // together on every block. Pure C over the span arrays: it allocates
    // nothing and raises nothing, so it is the final swap by itself.
    //
    // One note is atomic. A press of an ITERABLE of notes is one call per
    // note, so a block can land between two notes of a chord -- a
    // millisecond of arpeggio, not a fault. The timestamped event queue is
    // what makes a chord one act.
    audiodsp_pump_lock_acquire();
    int channel;
    if (new_note != SYNTHIO_SILENCE && (channel = find_channel_with_note(synth, new_note)) != -1) {
        // Re-pressing a note that still holds its slot. Upstream sets ATTACK
        // unconditionally, which is right only while the note is still
        // sounding: it swells back up from where it is, keeping its
        // oscillator phase, and no click is introduced.
        //
        // A note whose envelope has already run down to 0 is a different
        // thing. It is finished, merely not yet collected - and a press on
        // it is a NEW hit, not a swell. Upstream's unconditional ATTACK
        // leaves the level at 0, and the reaper at the top of the render
        // then sees level == 0 and frees the slot before anything steps the
        // envelope, so the note is deleted without ever sounding: the press
        // renders byte-identical to never having pressed. That is a
        // one-block race - a block earlier the level is still above 0, a
        // block later the slot is already free and the press takes the
        // init path below and works.
        //
        // Treating a finished note as a fresh press fixes it at the cause
        // and matches what the CPython target already does. See
        // docs/upstream-diff.md; reported upstream.
        if (synth->envelope_state[channel].level == 0) {
            synthio_envelope_state_init(&synth->envelope_state[channel],
                synthio_synth_get_note_envelope(synth, new_note));
            synth->accum[channel] = 0;
        } else {
            synth->envelope_state[channel].state = SYNTHIO_ENVELOPE_STATE_ATTACK;
        }
        audiodsp_pump_lock_release();
        return true;
    }
    channel = find_channel_with_note(synth, old_note);
    if (channel != -1) {
        if (new_note == SYNTHIO_SILENCE) {
            synthio_envelope_state_release(&synth->envelope_state[channel], synthio_synth_get_note_envelope(synth, old_note));
        } else {
            synth->span.note_obj[channel] = new_note;
            synthio_envelope_state_init(&synth->envelope_state[channel], synthio_synth_get_note_envelope(synth, new_note));
            synth->accum[channel] = 0;
        }
        audiodsp_pump_lock_release();
        return true;
    }
    // Nothing took it. For a press that is a note that will never sound, and
    // the count is the only trace it leaves -- see `refused` in the header.
    // A release that found no held note lands here too and is not counted.
    if (new_note != SYNTHIO_SILENCE) {
        synth->refused++;
    }
    audiodsp_pump_lock_release();
    return false;
}

uint64_t synthio_frequency_convert_float_to_scaled(mp_float_t val) {
    return round_float_to_int64(val * (1 << SYNTHIO_FREQUENCY_SHIFT));
}

uint32_t synthio_frequency_convert_float_to_dds(mp_float_t frequency_hz, int32_t sample_rate) {
    return synthio_frequency_convert_scaled_to_dds(synthio_frequency_convert_float_to_scaled(frequency_hz), sample_rate);
}

uint32_t synthio_frequency_convert_scaled_to_dds(uint64_t frequency_scaled, int32_t sample_rate) {
    return (sample_rate / 2 + frequency_scaled) / sample_rate;
}

void shared_bindings_synthio_lfo_tick(uint32_t sample_rate, uint16_t num_samples) {
    mp_float_t recip_sample_rate = MICROPY_FLOAT_CONST(1.) / sample_rate;
    synthio_global_rate_scale = num_samples * recip_sample_rate;
    synthio_global_W_scale = (2 * MP_PI) * recip_sample_rate;
    synthio_global_tick++;
}

mp_float_t synthio_block_slot_get(synthio_block_slot_t *slot) {
    if (mp_obj_is_float(slot->obj)) {
        return mp_obj_get_float(slot->obj);
    }

    synthio_block_base_t *block = MP_OBJ_TO_PTR(slot->obj);
    if (block->last_tick == synthio_global_tick) {
        return block->value;
    }

    block->last_tick = synthio_global_tick;
    const synthio_block_proto_t *p = MP_OBJ_TYPE_GET_SLOT(mp_obj_get_type(slot->obj), protocol);
    mp_float_t value = p->tick(slot->obj);
    block->value = value;
    return value;
}

mp_float_t synthio_block_slot_get_limited(synthio_block_slot_t *lfo_slot, mp_float_t lo, mp_float_t hi) {
    mp_float_t value = synthio_block_slot_get(lfo_slot);
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

int32_t synthio_block_slot_get_scaled(synthio_block_slot_t *lfo_slot, mp_float_t lo, mp_float_t hi) {
    mp_float_t value = synthio_block_slot_get_limited(lfo_slot, lo, hi);
    return (int32_t)MICROPY_FLOAT_C_FUN(round)(MICROPY_FLOAT_C_FUN(ldexp)(value, 15));
}

bool synthio_block_assign_slot_maybe(mp_obj_t obj, synthio_block_slot_t *slot) {
    if (synthio_obj_is_block(obj)) {
        slot->obj = obj;
        return true;
    }

    mp_float_t value = MICROPY_FLOAT_CONST(0.);
    if (obj != mp_const_none && !mp_obj_get_float_maybe(obj, &value)) {
        return false;
    }

    slot->obj = mp_obj_new_float(value);
    return true;
}

void synthio_block_assign_slot(mp_obj_t obj, synthio_block_slot_t *slot, qstr arg_name) {
    if (!synthio_block_assign_slot_maybe(obj, slot)) {
        mp_raise_TypeError_varg(MP_ERROR_TEXT("%q must be of type %q, not %q"), arg_name, MP_QSTR_BlockInput, mp_obj_get_type(obj)->name);
    }
}

bool synthio_obj_is_block(mp_obj_t obj) {
    return mp_proto_get(MP_QSTR_synthio_block, obj);
}

// --- from shared-bindings/synthio/__init__.c ------------------------------

MAKE_ENUM_VALUE(synthio_note_state_type, note_state, ATTACK, SYNTHIO_ENVELOPE_STATE_ATTACK);
MAKE_ENUM_VALUE(synthio_note_state_type, note_state, DECAY, SYNTHIO_ENVELOPE_STATE_DECAY);
MAKE_ENUM_VALUE(synthio_note_state_type, note_state, SUSTAIN, SYNTHIO_ENVELOPE_STATE_SUSTAIN);
MAKE_ENUM_VALUE(synthio_note_state_type, note_state, RELEASE, SYNTHIO_ENVELOPE_STATE_RELEASE);

MAKE_ENUM_MAP(synthio_note_state) {
    MAKE_ENUM_MAP_ENTRY(note_state, ATTACK),
    MAKE_ENUM_MAP_ENTRY(note_state, DECAY),
    MAKE_ENUM_MAP_ENTRY(note_state, SUSTAIN),
    MAKE_ENUM_MAP_ENTRY(note_state, RELEASE),
};

static MP_DEFINE_CONST_DICT(synthio_note_state_locals_dict, synthio_note_state_locals_table);
MAKE_PRINTER(synthio, synthio_note_state);
MAKE_ENUM_TYPE(synthio, EnvelopeState, synthio_note_state);

#define default_attack_time (MICROPY_FLOAT_CONST(0.1))
#define default_decay_time (MICROPY_FLOAT_CONST(0.05))
#define default_release_time (MICROPY_FLOAT_CONST(0.2))
#define default_attack_level (MICROPY_FLOAT_CONST(1.))
#define default_sustain_level (MICROPY_FLOAT_CONST(0.8))

static const mp_arg_t envelope_properties[] = {
    { MP_QSTR_attack_time, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL } },
    { MP_QSTR_decay_time, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL } },
    { MP_QSTR_release_time, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL } },
    { MP_QSTR_attack_level, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL } },
    { MP_QSTR_sustain_level, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL } },
};

static mp_obj_t synthio_envelope_make_new(const mp_obj_type_t *type_in, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_val_t args[MP_ARRAY_SIZE(envelope_properties)];
    enum { ARG_attack_time, ARG_decay_time, ARG_release_time, ARG_attack_level, ARG_sustain_level };
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(envelope_properties), envelope_properties, args);

    if (args[ARG_attack_time].u_obj == MP_OBJ_NULL) {
        args[ARG_attack_time].u_obj = mp_obj_new_float(default_attack_time);
    }
    if (args[ARG_decay_time].u_obj == MP_OBJ_NULL) {
        args[ARG_decay_time].u_obj = mp_obj_new_float(default_decay_time);
    }
    if (args[ARG_release_time].u_obj == MP_OBJ_NULL) {
        args[ARG_release_time].u_obj = mp_obj_new_float(default_release_time);
    }
    if (args[ARG_attack_level].u_obj == MP_OBJ_NULL) {
        args[ARG_attack_level].u_obj = mp_obj_new_float(default_attack_level);
    }
    if (args[ARG_sustain_level].u_obj == MP_OBJ_NULL) {
        args[ARG_sustain_level].u_obj = mp_obj_new_float(default_sustain_level);
    }

    mp_arg_validate_obj_float_non_negative(args[ARG_attack_time].u_obj, 0., MP_QSTR_attack_time);
    mp_arg_validate_obj_float_non_negative(args[ARG_decay_time].u_obj, 0., MP_QSTR_decay_time);
    mp_arg_validate_obj_float_non_negative(args[ARG_release_time].u_obj, 0., MP_QSTR_release_time);

    mp_arg_validate_obj_float_range(args[ARG_attack_level].u_obj, 0, 1, MP_QSTR_attack_level);
    mp_arg_validate_obj_float_range(args[ARG_sustain_level].u_obj, 0, 1, MP_QSTR_sustain_level);

    MP_STATIC_ASSERT(sizeof(mp_arg_val_t) == sizeof(mp_obj_t));
    return namedtuple_make_new(type_in, MP_ARRAY_SIZE(args), 0, &args[0].u_obj);
};

const mp_obj_namedtuple_type_t synthio_envelope_type_obj = {
    NAMEDTUPLE_TYPE_BASE_AND_SLOTS_MAKE_NEW(MP_QSTR_Envelope, synthio_envelope_make_new),
    .n_fields = 5,
    .fields = {
        MP_QSTR_attack_time,
        MP_QSTR_decay_time,
        MP_QSTR_release_time,
        MP_QSTR_attack_level,
        MP_QSTR_sustain_level,
    },
};

// Deviation from upstream: reads through the generic stream protocol
// instead of raw FatFS calls (mirrors audiocore.WaveFile -- see
// docs/upstream-diff.md). Small local read/seek helpers instead of sharing
// audiocore/WaveFile.c's static ones (kept file-local on both sides,
// same as upstream keeps this logic file-local too).
static void midi_read_exactly(mp_obj_t file, void *buf, size_t size) {
    int errcode = 0;
    mp_uint_t n = mp_stream_read_exactly(file, buf, size, &errcode);
    if (errcode != 0) {
        mp_raise_OSError(errcode);
    }
    if (n != size) {
        mp_raise_OSError(MP_EIO);
    }
}

static mp_obj_t synthio_from_file(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_file, ARG_sample_rate, ARG_waveform, ARG_envelope };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_file, MP_ARG_OBJ | MP_ARG_REQUIRED, {} },
        { MP_QSTR_sample_rate, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 11025} },
        { MP_QSTR_waveform, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = mp_const_none } },
        { MP_QSTR_envelope, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_obj_t file = args[ARG_file].u_obj;
    if (mp_obj_is_str(file)) {
        file = mp_call_function_2(MP_OBJ_FROM_PTR(&mp_builtin_open_obj), file, MP_ROM_QSTR(MP_QSTR_rb));
    }
    mp_get_stream_raise(file, MP_STREAM_OP_READ);

    uint8_t chunk_header[14];
    midi_read_exactly(file, chunk_header, sizeof(chunk_header));
    if (memcmp(chunk_header, "MThd\0\0\0\6\0\0\0\1", 12)) {
        mp_arg_error_invalid(MP_QSTR_file);
        // TODO: for a multi-track MIDI (type 1), return an AudioMixer
    }

    uint16_t tempo;
    if (chunk_header[12] & 0x80) {
        tempo = -(int8_t)chunk_header[12] * chunk_header[13];
    } else {
        tempo = 2 * ((chunk_header[12] << 8) | chunk_header[13]);
    }

    midi_read_exactly(file, chunk_header, 8);
    if (memcmp(chunk_header, "MTrk", 4)) {
        mp_arg_error_invalid(MP_QSTR_file);
    }
    uint32_t track_size = (chunk_header[4] << 24) |
        (chunk_header[5] << 16) | (chunk_header[6] << 8) | chunk_header[7];
    uint8_t *buffer = m_malloc(track_size);
    midi_read_exactly(file, buffer, track_size);

    synthio_miditrack_obj_t *result = mp_obj_malloc(synthio_miditrack_obj_t, &synthio_miditrack_type);
    common_hal_synthio_miditrack_construct(result, buffer, track_size,
        tempo, args[ARG_sample_rate].u_int, args[ARG_waveform].u_obj,
        mp_const_none,
        args[ARG_envelope].u_obj
        );

    #if MICROPY_MALLOC_USES_ALLOCATED_SIZE
    m_free(buffer, track_size);
    #else
    m_free(buffer);
    #endif

    return MP_OBJ_FROM_PTR(result);
}
MP_DEFINE_CONST_FUN_OBJ_KW(synthio_from_file_obj, 1, synthio_from_file);

static mp_obj_t midi_to_hz(mp_obj_t arg) {
    mp_float_t note = mp_arg_validate_type_float(arg, MP_QSTR_note);
    return mp_obj_new_float(common_hal_synthio_midi_to_hz_float(note));
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_midi_to_hz_obj, midi_to_hz);

static mp_obj_t voct_to_hz(mp_obj_t arg) {
    mp_float_t note = mp_arg_validate_obj_float_range(arg, -11, 11, MP_QSTR_ctrl);
    return mp_obj_new_float(common_hal_synthio_voct_to_hz_float(note));
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_voct_to_hz_obj, voct_to_hz);

static mp_obj_t synthio_lfo_tick(size_t n, const mp_obj_t *args) {
    shared_bindings_synthio_lfo_tick(48000, SYNTHIO_MAX_DUR);
    mp_obj_t result[n];
    for (size_t i = 0; i < n; i++) {
        synthio_block_slot_t slot;
        synthio_block_assign_slot(args[i], &slot, MP_QSTR_arg);
        mp_float_t value = synthio_block_slot_get(&slot);
        result[i] = mp_obj_new_float(value);
    }
    return mp_obj_new_tuple(n, result);
}
MP_DEFINE_CONST_FUN_OBJ_VAR(synthio_lfo_tick_obj, 1, synthio_lfo_tick);

static const mp_rom_map_elem_t synthio_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_synthio) },
    { MP_ROM_QSTR(MP_QSTR_Biquad), MP_ROM_PTR(&synthio_biquad_type_obj) },
    { MP_ROM_QSTR(MP_QSTR_FilterMode), MP_ROM_PTR(&synthio_filter_mode_type) },
    { MP_ROM_QSTR(MP_QSTR_Math), MP_ROM_PTR(&synthio_math_type) },
    { MP_ROM_QSTR(MP_QSTR_MathOperation), MP_ROM_PTR(&synthio_math_operation_type) },
    { MP_ROM_QSTR(MP_QSTR_MidiTrack), MP_ROM_PTR(&synthio_miditrack_type) },
    { MP_ROM_QSTR(MP_QSTR_Note), MP_ROM_PTR(&synthio_note_type) },
    { MP_ROM_QSTR(MP_QSTR_EnvelopeState), MP_ROM_PTR(&synthio_note_state_type) },
    { MP_ROM_QSTR(MP_QSTR_LFO), MP_ROM_PTR(&synthio_lfo_type) },
    { MP_ROM_QSTR(MP_QSTR_Synthesizer), MP_ROM_PTR(&synthio_synthesizer_type) },
    { MP_ROM_QSTR(MP_QSTR_from_file), MP_ROM_PTR(&synthio_from_file_obj) },
    { MP_ROM_QSTR(MP_QSTR_Envelope), MP_ROM_PTR(&synthio_envelope_type_obj) },
    { MP_ROM_QSTR(MP_QSTR_midi_to_hz), MP_ROM_PTR(&synthio_midi_to_hz_obj) },
    { MP_ROM_QSTR(MP_QSTR_voct_to_hz), MP_ROM_PTR(&synthio_voct_to_hz_obj) },
    { MP_ROM_QSTR(MP_QSTR_waveform_max_length), MP_ROM_INT(SYNTHIO_WAVEFORM_SIZE) },
    { MP_ROM_QSTR(MP_QSTR_lfo_tick), MP_ROM_PTR(&synthio_lfo_tick_obj) },
};

static MP_DEFINE_CONST_DICT(synthio_module_globals, synthio_module_globals_table);

const mp_obj_module_t synthio_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&synthio_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_synthio, synthio_module);
