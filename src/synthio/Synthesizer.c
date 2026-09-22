// Ported from CircuitPython's shared-bindings/synthio/Synthesizer.c and
// shared-module/synthio/Synthesizer.c (upstream repo:
// https://github.com/adafruit/circuitpython, MIT), merged into one file.
// Docstrings dropped. `MP_OBJ_NEW_TUPLE(...)` (CP's variadic tuple-literal
// macro, py/objtuple.h) inlined at its one call site instead of ported --
// avoids relying on a GCC statement-expression extension for a single use.
// See docs/upstream-diff.md: every type using MP_PROPERTY_GETTER/GETSET
// needs `attr, cp_compat_attr` wired in.
//
// SPDX-FileCopyrightText: Copyright (c) 2021 Artyom Skrobov
// SPDX-FileCopyrightText: Copyright (c) 2023 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "cp_compat/argcheck.h"
#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/enum.h"
#include "cp_compat/objproperty.h"
#include "cp_compat/util.h"
#include "synthio/LFO.h"
#include "synthio/Note.h"
#include "synthio/Synthesizer.h"
#include "synthio/__init__.h"

#include "py/runtime.h"
#include "py/objlist.h"
#include "shared/audiodsp_pump_lock.h"

// --- from shared-module/synthio/Synthesizer.c -----------------------------

void common_hal_synthio_synthesizer_construct(synthio_synthesizer_obj_t *self,
    uint32_t sample_rate, int channel_count, mp_obj_t waveform_obj,
    mp_obj_t envelope_obj) {

    synthio_synth_init(&self->synth, sample_rate, channel_count, waveform_obj, envelope_obj);
    self->blocks = mp_obj_new_list(0, NULL);
}

void common_hal_synthio_synthesizer_deinit(synthio_synthesizer_obj_t *self) {
    // The whole body. mark_deinit is not the damage; the pointer
    // nulling AFTER it is -- the funnel's guard has already let a
    // pull in by then, and the pull writes into a buffer that has
    // just become NULL. Detach under the lock, free afterwards.
    audiodsp_pump_lock_acquire();
    synthio_synth_deinit(&self->synth);
    audiodsp_pump_lock_release();
}

void synthio_synthesizer_reset_buffer(synthio_synthesizer_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    synthio_synth_reset_buffer(&self->synth, single_channel_output, channel);
}

audioio_get_buffer_result_t synthio_synthesizer_get_buffer(synthio_synthesizer_obj_t *self,
    bool single_channel_output, uint8_t channel, uint8_t **buffer, uint32_t *buffer_length) {
    if (audiosample_deinited(&self->synth.base)) {
        *buffer_length = 0;
        return GET_BUFFER_ERROR;
    }
    self->synth.span.dur = SYNTHIO_MAX_DUR;


    synthio_synth_synthesize(&self->synth, buffer, buffer_length, single_channel_output ? channel : 0);

    // Free-running LFOs. Walked as a LIST, not with mp_getiter/mp_iternext.
    //
    // This is a pull, and mp_iternext re-enters the interpreter: it calls
    // mp_cstack_check(), which reads this thread's registered stack limits --
    // and a C pump thread has none, so it reads garbage and segfaults before
    // anything else can go wrong. Found by the storm on Tremolo, whose LFO
    // is a Synthesizer behind a Multiply:
    //
    //     mp_cstack_usage            py/cstack.c:46    <-- SIGSEGV
    //     mp_iternext                py/runtime.c:1392
    //     synthio_synthesizer_get_buffer
    //
    // `blocks` is created as a list here (make_new above) and exposed
    // read-only, so it is always a list and the walk below is exactly the
    // same traversal with no runtime in it. The type check is the honest
    // guard rather than an assumption: anything else is skipped rather than
    // iterated, because a custom iterable's __next__ is Python code and
    // Python code cannot run on this thread at all.
    if (mp_obj_is_type(self->blocks, &mp_type_list)) {
        size_t len = 0;
        mp_obj_t *items = NULL;
        mp_obj_list_get(self->blocks, &len, &items);
        for (size_t i = 0; i < len; i++) {
            if (!synthio_obj_is_block(items[i])) {
                continue;
            }
            synthio_block_slot_t slot = { items[i] };
            (void)synthio_block_slot_get(&slot);
        }
    }
    return GET_BUFFER_MORE_DATA;
}

void common_hal_synthio_synthesizer_release_all(synthio_synthesizer_obj_t *self) {
    for (size_t i = 0; i < CIRCUITPY_SYNTHIO_MAX_CHANNELS; i++) {
        if (self->synth.span.note_obj[i] != SYNTHIO_SILENCE) {
            synthio_span_change_note(&self->synth, self->synth.span.note_obj[i], SYNTHIO_SILENCE);
        }
    }
}

static bool is_note(mp_obj_t note_in) {
    return mp_obj_is_small_int(note_in) || mp_obj_is_type(note_in, &synthio_note_type);
}

static mp_obj_t validate_note(mp_obj_t note_in) {
    if (mp_obj_is_small_int(note_in)) {
        mp_arg_validate_int_range(mp_obj_get_int(note_in), 0, 127, MP_QSTR_note);
    } else {
        const mp_obj_type_t *note_type = mp_obj_get_type(note_in);
        if (note_type != &synthio_note_type) {
            mp_raise_TypeError_varg(MP_ERROR_TEXT("%q must be of type %q or %q, not %q"), MP_QSTR_note, MP_QSTR_int, MP_QSTR_Note, note_type->name);
        }
    }
    return note_in;
}

void common_hal_synthio_synthesizer_release(synthio_synthesizer_obj_t *self, mp_obj_t to_release) {
    if (is_note(to_release)) {
        synthio_span_change_note(&self->synth, validate_note(to_release), SYNTHIO_SILENCE);
        return;
    }

    mp_obj_iter_buf_t iter_buf;
    mp_obj_t iterable = mp_getiter(to_release, &iter_buf);
    mp_obj_t item;
    while ((item = mp_iternext(iterable)) != MP_OBJ_STOP_ITERATION) {
        synthio_span_change_note(&self->synth, validate_note(item), SYNTHIO_SILENCE);
    }
}

void common_hal_synthio_synthesizer_press(synthio_synthesizer_obj_t *self, mp_obj_t to_press) {
    if (is_note(to_press)) {
        if (!mp_obj_is_small_int(to_press)) {
            synthio_note_obj_t *note = MP_OBJ_TO_PTR(to_press);
            synthio_note_start(note, self->synth.base.sample_rate);
        }
        synthio_span_change_note(&self->synth, SYNTHIO_SILENCE, validate_note(to_press));
        return;
    }

    mp_obj_iter_buf_t iter_buf;
    mp_obj_t iterable = mp_getiter(to_press, &iter_buf);
    mp_obj_t note_obj;
    while ((note_obj = mp_iternext(iterable)) != MP_OBJ_STOP_ITERATION) {
        note_obj = validate_note(note_obj);
        if (!mp_obj_is_small_int(note_obj)) {
            synthio_note_obj_t *note = MP_OBJ_TO_PTR(note_obj);
            synthio_note_start(note, self->synth.base.sample_rate);
        }
        synthio_span_change_note(&self->synth, SYNTHIO_SILENCE, note_obj);
    }
}

void common_hal_synthio_synthesizer_retrigger(synthio_synthesizer_obj_t *self, mp_obj_t to_retrigger) {
    if (mp_obj_is_type(to_retrigger, &synthio_lfo_type)) {
        synthio_lfo_obj_t *lfo = MP_OBJ_TO_PTR(mp_arg_validate_type(to_retrigger, &synthio_lfo_type, MP_QSTR_retrigger));
        common_hal_synthio_lfo_retrigger(lfo);
        return;
    }

    mp_obj_iter_buf_t iter_buf;
    mp_obj_t iterable = mp_getiter(to_retrigger, &iter_buf);
    mp_obj_t lfo_obj;
    while ((lfo_obj = mp_iternext(iterable)) != MP_OBJ_STOP_ITERATION) {
        synthio_lfo_obj_t *lfo = MP_OBJ_TO_PTR(mp_arg_validate_type(lfo_obj, &synthio_lfo_type, MP_QSTR_retrigger));
        common_hal_synthio_lfo_retrigger(lfo);
    }
}

mp_obj_t common_hal_synthio_synthesizer_get_pressed_notes(synthio_synthesizer_obj_t *self) {
    int count = 0;
    for (int chan = 0; chan < CIRCUITPY_SYNTHIO_MAX_CHANNELS; chan++) {
        if (self->synth.span.note_obj[chan] != SYNTHIO_SILENCE && SYNTHIO_NOTE_IS_PLAYING(&self->synth, chan)) {
            count += 1;
        }
    }
    mp_obj_tuple_t *result = MP_OBJ_TO_PTR(mp_obj_new_tuple(count, NULL));
    for (size_t chan = 0, j = 0; chan < CIRCUITPY_SYNTHIO_MAX_CHANNELS; chan++) {
        if (self->synth.span.note_obj[chan] != SYNTHIO_SILENCE && SYNTHIO_NOTE_IS_PLAYING(&self->synth, chan)) {
            result->items[j++] = self->synth.span.note_obj[chan];
        }
    }
    return MP_OBJ_FROM_PTR(result);
}

envelope_state_e common_hal_synthio_synthesizer_note_info(synthio_synthesizer_obj_t *self, mp_obj_t note, mp_float_t *vol_out) {
    for (int chan = 0; chan < CIRCUITPY_SYNTHIO_MAX_CHANNELS; chan++) {
        if (self->synth.span.note_obj[chan] == note) {
            *vol_out = self->synth.envelope_state[chan].level / MICROPY_FLOAT_CONST(32767.);
            return self->synth.envelope_state[chan].state;
        }
    }
    return (envelope_state_e) - 1;
}


mp_obj_t common_hal_synthio_synthesizer_get_blocks(synthio_synthesizer_obj_t *self) {
    return self->blocks;
}

// --- from shared-bindings/synthio/Synthesizer.c ---------------------------

static mp_obj_t synthio_synthesizer_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_sample_rate, ARG_channel_count, ARG_waveform, ARG_envelope };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sample_rate, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 11025} },
        { MP_QSTR_channel_count, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1} },
        { MP_QSTR_waveform, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = mp_const_none } },
        { MP_QSTR_envelope, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    synthio_synthesizer_obj_t *self = mp_obj_malloc(synthio_synthesizer_obj_t, &synthio_synthesizer_type);
    common_hal_synthio_synthesizer_construct(self,
        args[ARG_sample_rate].u_int,
        args[ARG_channel_count].u_int,
        args[ARG_waveform].u_obj,
        args[ARG_envelope].u_obj);

    return MP_OBJ_FROM_PTR(self);
}

static void check_for_deinit(synthio_synthesizer_obj_t *self) {
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

static mp_obj_t synthio_synthesizer_press(mp_obj_t self_in, mp_obj_t press) {
    synthio_synthesizer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    common_hal_synthio_synthesizer_press(self, press);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(synthio_synthesizer_press_obj, synthio_synthesizer_press);

static mp_obj_t synthio_synthesizer_release(mp_obj_t self_in, mp_obj_t release) {
    synthio_synthesizer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    common_hal_synthio_synthesizer_release(self, release);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(synthio_synthesizer_release_obj, synthio_synthesizer_release);

static mp_obj_t synthio_synthesizer_change(mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_release, ARG_press, ARG_retrigger };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_release, MP_ARG_OBJ, {.u_obj = mp_const_empty_tuple } },
        { MP_QSTR_press, MP_ARG_OBJ, {.u_obj = mp_const_empty_tuple } },
        { MP_QSTR_retrigger, MP_ARG_OBJ, {.u_obj = mp_const_empty_tuple } },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    synthio_synthesizer_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    check_for_deinit(self);
    common_hal_synthio_synthesizer_release(self, args[ARG_release].u_obj);
    common_hal_synthio_synthesizer_press(self, args[ARG_press].u_obj);
    common_hal_synthio_synthesizer_retrigger(self, args[ARG_retrigger].u_obj);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(synthio_synthesizer_change_obj, 1, synthio_synthesizer_change);

static mp_obj_t synthio_synthesizer_release_all_then_press(mp_obj_t self_in, mp_obj_t press) {
    synthio_synthesizer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    common_hal_synthio_synthesizer_release_all(self);
    common_hal_synthio_synthesizer_press(self, press);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(synthio_synthesizer_release_all_then_press_obj, synthio_synthesizer_release_all_then_press);

static mp_obj_t synthio_synthesizer_release_all(mp_obj_t self_in) {
    synthio_synthesizer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    common_hal_synthio_synthesizer_release_all(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(synthio_synthesizer_release_all_obj, synthio_synthesizer_release_all);

static mp_obj_t synthio_synthesizer_deinit(mp_obj_t self_in) {
    synthio_synthesizer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_synthio_synthesizer_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(synthio_synthesizer_deinit_obj, synthio_synthesizer_deinit);

static mp_obj_t synthio_synthesizer_obj_get_envelope(mp_obj_t self_in) {
    synthio_synthesizer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return synthio_synth_envelope_get(&self->synth);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_synthesizer_get_envelope_obj, synthio_synthesizer_obj_get_envelope);

static mp_obj_t synthio_synthesizer_obj_set_envelope(mp_obj_t self_in, mp_obj_t envelope) {
    synthio_synthesizer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    synthio_synth_envelope_set(&self->synth, envelope);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_synthesizer_set_envelope_obj, synthio_synthesizer_obj_set_envelope);

MP_PROPERTY_GETSET(synthio_synthesizer_envelope_obj,
    (mp_obj_t)&synthio_synthesizer_get_envelope_obj,
    (mp_obj_t)&synthio_synthesizer_set_envelope_obj);

static mp_obj_t synthio_synthesizer_obj_get_pressed(mp_obj_t self_in) {
    synthio_synthesizer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return common_hal_synthio_synthesizer_get_pressed_notes(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_synthesizer_get_pressed_obj, synthio_synthesizer_obj_get_pressed);

MP_PROPERTY_GETTER(synthio_synthesizer_pressed_obj,
    (mp_obj_t)&synthio_synthesizer_get_pressed_obj);

//|     refused: int
//|     """How many presses have been dropped because every channel was held.
//|
//|     A refused press is the one failure nobody can hear go wrong: the note
//|     simply never sounds. This is the count, and nothing more -- there is
//|     no stealing policy here and ``max_polyphony`` has not moved. An
//|     instrument that wants to steal a channel can decide that for itself
//|     once it can see the number.
//|
//|     Counts a press that arrived any way at all: a live ``press()``, and
//|     a press applied from an ``audiopump.Events`` queue, which comes
//|     through the same place. Not to be confused with
//|     ``Events.stats()``'s own refused count, which is events that could
//|     not be applied at all -- a deinited target, a voice with no Mixer.
//|
//|     Monotonic from construction, and not reset by ``release_all()``."""
static mp_obj_t synthio_synthesizer_obj_get_refused(mp_obj_t self_in) {
    synthio_synthesizer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_int_from_uint(self->synth.refused);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_synthesizer_get_refused_obj,
    synthio_synthesizer_obj_get_refused);

MP_PROPERTY_GETTER(synthio_synthesizer_refused_obj,
    (mp_obj_t)&synthio_synthesizer_get_refused_obj);

static mp_obj_t synthio_synthesizer_obj_note_info(mp_obj_t self_in, mp_obj_t note) {
    synthio_synthesizer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    mp_float_t vol = MICROPY_FLOAT_CONST(0.0);
    envelope_state_e state = common_hal_synthio_synthesizer_note_info(self, note, &vol);
    mp_obj_t items[] = { cp_enum_find(&synthio_note_state_type, state), mp_obj_new_float(vol) };
    return mp_obj_new_tuple(2, items);
}
MP_DEFINE_CONST_FUN_OBJ_2(synthio_synthesizer_note_info_obj, synthio_synthesizer_obj_note_info);

static mp_obj_t synthio_synthesizer_obj_get_blocks(mp_obj_t self_in) {
    synthio_synthesizer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return common_hal_synthio_synthesizer_get_blocks(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(synthio_synthesizer_get_blocks_obj, synthio_synthesizer_obj_get_blocks);

MP_PROPERTY_GETTER(synthio_synthesizer_blocks_obj,
    (mp_obj_t)&synthio_synthesizer_get_blocks_obj);

static const mp_rom_map_elem_t synthio_synthesizer_locals_dict_table[] = {
    // Methods
    { MP_ROM_QSTR(MP_QSTR_press), MP_ROM_PTR(&synthio_synthesizer_press_obj) },
    { MP_ROM_QSTR(MP_QSTR_release), MP_ROM_PTR(&synthio_synthesizer_release_obj) },
    { MP_ROM_QSTR(MP_QSTR_release_all), MP_ROM_PTR(&synthio_synthesizer_release_all_obj) },
    { MP_ROM_QSTR(MP_QSTR_change), MP_ROM_PTR(&synthio_synthesizer_change_obj) },
    { MP_ROM_QSTR(MP_QSTR_release_then_press), MP_ROM_PTR(&synthio_synthesizer_change_obj) },
    { MP_ROM_QSTR(MP_QSTR_release_all_then_press), MP_ROM_PTR(&synthio_synthesizer_release_all_then_press_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&synthio_synthesizer_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },

    // Properties
    { MP_ROM_QSTR(MP_QSTR_envelope), MP_ROM_PTR(&synthio_synthesizer_envelope_obj) },
    { MP_ROM_QSTR(MP_QSTR_max_polyphony), MP_ROM_INT(CIRCUITPY_SYNTHIO_MAX_CHANNELS) },
    { MP_ROM_QSTR(MP_QSTR_pressed), MP_ROM_PTR(&synthio_synthesizer_pressed_obj) },
    { MP_ROM_QSTR(MP_QSTR_refused), MP_ROM_PTR(&synthio_synthesizer_refused_obj) },
    { MP_ROM_QSTR(MP_QSTR_note_info), MP_ROM_PTR(&synthio_synthesizer_note_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_blocks), MP_ROM_PTR(&synthio_synthesizer_blocks_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(synthio_synthesizer_locals_dict, synthio_synthesizer_locals_dict_table);

static const audiosample_p_t synthio_synthesizer_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)synthio_synthesizer_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)synthio_synthesizer_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    synthio_synthesizer_type,
    MP_QSTR_Synthesizer,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, synthio_synthesizer_make_new,
    attr, cp_compat_attr,
    locals_dict, &synthio_synthesizer_locals_dict,
    protocol, &synthio_synthesizer_proto
    );
