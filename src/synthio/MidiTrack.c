// Ported from CircuitPython's shared-bindings/synthio/MidiTrack.c and
// shared-module/synthio/MidiTrack.c (upstream repo:
// https://github.com/adafruit/circuitpython, MIT), merged into one file.
// Docstrings dropped. See docs/upstream-diff.md: every type using
// MP_PROPERTY_GETTER/GETSET needs `attr, cp_compat_attr` wired in. This
// file's own byte-buffer-based construction (`MidiTrack(buffer, tempo)`)
// needs no file-I/O adaptation -- only `synthio.from_file()`
// (synthio/__init__.c) reads an actual MIDI file, and that's where the
// FatFS-vs-generic-stream deviation lives.
//
// SPDX-FileCopyrightText: Copyright (c) 2021 Artyom Skrobov
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "cp_compat/argcheck.h"
#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/objproperty.h"
#include "cp_compat/util.h"
#include "synthio/MidiTrack.h"
#include "synthio/__init__.h"

#include "py/runtime.h"
#include "shared/audiodsp_pump_lock.h"

// --- from shared-module/synthio/MidiTrack.c -------------------------------

static void record_midi_stream_error(synthio_miditrack_obj_t *self) {
    self->error_location = self->pos;
    self->pos = self->track.len;
}

static mp_obj_t parse_note(synthio_miditrack_obj_t *self) {
    uint8_t *buffer = self->track.buf;
    size_t len = self->track.len;
    if (self->pos + 1 >= len) {
        record_midi_stream_error(self);
    }
    uint8_t note = buffer[(self->pos)++];
    if (note > 127 || buffer[(self->pos)++] > 127) {
        record_midi_stream_error(self);
    }
    return MP_OBJ_NEW_SMALL_INT(note);
}

static int decode_duration(synthio_miditrack_obj_t *self) {
    uint8_t *buffer = self->track.buf;
    size_t len = self->track.len;
    uint8_t c;
    uint32_t delta = 0;
    do {
        c = buffer[self->pos++];
        delta <<= 7;
        delta |= c & 0x7f;
    } while ((c & 0x80) && (self->pos < len));

    // errors cannot be raised from the background task, so simply end the track.
    if (c & 0x80) {
        self->pos = self->track.len;
        record_midi_stream_error(self);
    }
    return delta * self->synth.base.sample_rate / self->tempo;
}

// invariant: pointing at a MIDI message
static void decode_until_pause(synthio_miditrack_obj_t *self) {
    uint8_t *buffer = self->track.buf;
    size_t len = self->track.len;
    do {
        switch (buffer[self->pos++] >> 4) {
            case 8: { // Note Off
                mp_obj_t note = parse_note(self);
                synthio_span_change_note(&self->synth, note, SYNTHIO_SILENCE);
                break;
            }
            case 9: { // Note On
                mp_obj_t note = parse_note(self);
                synthio_span_change_note(&self->synth, SYNTHIO_SILENCE, note);
                break;
            }
            case 10:
            case 11:
            case 14: // two data bytes to ignore
                parse_note(self);
                break;
            case 12:
            case 13: // one data byte to ignore
                if (self->pos >= len || buffer[self->pos++] > 127) {
                    record_midi_stream_error(self);
                }
                break;
            case 15: // the full syntax is too complicated, just assume it's "End of Track" event
                self->pos = len;
                break;
            default: // invalid event
                record_midi_stream_error(self);
        }
        if (self->pos < len) {
            self->synth.span.dur = decode_duration(self);
        }
    } while (self->pos < len && self->synth.span.dur == 0);
}

static void start_parse(synthio_miditrack_obj_t *self) {
    self->pos = 0;
    self->error_location = -1;
    self->synth.span.dur = decode_duration(self);
    if (self->synth.span.dur == 0) {
        // the usual case: the file starts with some MIDI event, not a delay
        decode_until_pause(self);
    }
}

void common_hal_synthio_miditrack_construct(synthio_miditrack_obj_t *self,
    const uint8_t *buffer, uint32_t len, uint32_t tempo, uint32_t sample_rate,
    mp_obj_t waveform_obj, mp_obj_t filter_obj, mp_obj_t envelope_obj) {

    self->tempo = tempo;
    self->track.buf = (void *)buffer;
    self->track.len = len;

    synthio_synth_init(&self->synth, sample_rate, 1, waveform_obj, envelope_obj);

    start_parse(self);
}

void common_hal_synthio_miditrack_deinit(synthio_miditrack_obj_t *self) {
    // The whole body. mark_deinit is not the damage; the pointer
    // nulling AFTER it is -- the funnel's guard has already let a
    // pull in by then, and the pull writes into a buffer that has
    // just become NULL. Detach under the lock, free afterwards.
    audiodsp_pump_lock_acquire();
    synthio_synth_deinit(&self->synth);
    audiodsp_pump_lock_release();
}

mp_int_t common_hal_synthio_miditrack_get_error_location(synthio_miditrack_obj_t *self) {
    return self->error_location;
}

mp_int_t common_hal_synthio_miditrack_get_tempo(synthio_miditrack_obj_t *self) {
    return self->tempo;
}

void common_hal_synthio_miditrack_set_tempo(synthio_miditrack_obj_t *self, mp_int_t value) {
    mp_int_t val = mp_arg_validate_int_min(value, 1, MP_QSTR_tempo);
    self->synth.span.dur = (uint32_t)self->synth.span.dur * self->tempo / val;
    self->tempo = val;
}

void synthio_miditrack_reset_buffer(synthio_miditrack_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    synthio_synth_reset_buffer(&self->synth, single_channel_output, channel);
    start_parse(self);
}

audioio_get_buffer_result_t synthio_miditrack_get_buffer(synthio_miditrack_obj_t *self,
    bool single_channel_output, uint8_t channel, uint8_t **buffer, uint32_t *buffer_length) {
    if (audiosample_deinited(&self->synth.base)) {
        *buffer_length = 0;
        return GET_BUFFER_ERROR;
    }

    synthio_synth_synthesize(&self->synth, buffer, buffer_length, single_channel_output ? 0 : channel);
    if (self->synth.span.dur == 0) {
        if (self->pos == self->track.len) {
            return GET_BUFFER_DONE;
        } else {
            decode_until_pause(self);
        }
    }
    return GET_BUFFER_MORE_DATA;
}

// --- from shared-bindings/synthio/MidiTrack.c -----------------------------

static mp_obj_t synthio_miditrack_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_buffer, ARG_tempo, ARG_sample_rate, ARG_waveform, ARG_envelope };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_buffer, MP_ARG_OBJ | MP_ARG_REQUIRED, {} },
        { MP_QSTR_tempo, MP_ARG_INT | MP_ARG_REQUIRED, {} },
        { MP_QSTR_sample_rate, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 11025} },
        { MP_QSTR_waveform, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = mp_const_none } },
        { MP_QSTR_envelope, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[ARG_buffer].u_obj, &bufinfo, MP_BUFFER_READ);

    synthio_miditrack_obj_t *self = mp_obj_malloc(synthio_miditrack_obj_t, &synthio_miditrack_type);
    common_hal_synthio_miditrack_construct(self,
        (uint8_t *)bufinfo.buf, bufinfo.len,
        args[ARG_tempo].u_int,
        args[ARG_sample_rate].u_int,
        args[ARG_waveform].u_obj,
        mp_const_none,
        args[ARG_envelope].u_obj
        );

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t synthio_miditrack_deinit(mp_obj_t self_in) {
    synthio_miditrack_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_miditrack_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(synthio_miditrack_deinit_obj, synthio_miditrack_deinit);

static void check_for_deinit(synthio_miditrack_obj_t *self) {
    // One word read under the lock, and the RAISE OUTSIDE IT. The lock's
    // contract is that nothing which can longjmp runs while it is held
    // (shared/audiodsp_pump_lock.h): a raise from in here never reaches the
    // release, so the mutex is left owned by a thread that has gone back to
    // the interpreter, and the pump blocks on it for ever. This is the guard
    // on every Python-facing method of this class, so it is the most reached
    // statement in the file.
    audiodsp_pump_lock_acquire();
    const bool released = audiosample_deinited(&self->synth.base);
    audiodsp_pump_lock_release();
    if (released) {
        audiosample_check_for_deinit(&self->synth.base);
    }
}

static mp_obj_t synthio_miditrack_obj_get_tempo(mp_obj_t self_in) {
    synthio_miditrack_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_int(common_hal_synthio_miditrack_get_tempo(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_miditrack_get_tempo_obj, synthio_miditrack_obj_get_tempo);

static mp_obj_t synthio_miditrack_obj_set_tempo(mp_obj_t self_in, mp_obj_t arg) {
    synthio_miditrack_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    common_hal_synthio_miditrack_set_tempo(self, mp_obj_get_int(arg));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_miditrack_set_tempo_obj, synthio_miditrack_obj_set_tempo);
MP_PROPERTY_GETSET(synthio_miditrack_tempo_obj,
    (mp_obj_t)&synthio_miditrack_get_tempo_obj,
    (mp_obj_t)&synthio_miditrack_set_tempo_obj);

static mp_obj_t synthio_miditrack_obj_get_error_location(mp_obj_t self_in) {
    synthio_miditrack_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    mp_int_t location = common_hal_synthio_miditrack_get_error_location(self);
    if (location >= 0) {
        return MP_OBJ_NEW_SMALL_INT(location);
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_miditrack_get_error_location_obj, synthio_miditrack_obj_get_error_location);

MP_PROPERTY_GETTER(synthio_miditrack_error_location_obj,
    (mp_obj_t)&synthio_miditrack_get_error_location_obj);

static const mp_rom_map_elem_t synthio_miditrack_locals_dict_table[] = {
    // Methods
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&synthio_miditrack_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },

    // Properties
    { MP_ROM_QSTR(MP_QSTR_error_location), MP_ROM_PTR(&synthio_miditrack_error_location_obj) },
    { MP_ROM_QSTR(MP_QSTR_tempo), MP_ROM_PTR(&synthio_miditrack_tempo_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(synthio_miditrack_locals_dict, synthio_miditrack_locals_dict_table);

static const audiosample_p_t synthio_miditrack_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)synthio_miditrack_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)synthio_miditrack_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    synthio_miditrack_type,
    MP_QSTR_MidiTrack,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, synthio_miditrack_make_new,
    attr, cp_compat_attr,
    locals_dict, &synthio_miditrack_locals_dict,
    protocol, &synthio_miditrack_proto
    );
