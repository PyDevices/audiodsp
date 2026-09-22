// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// audiopump.Events -- a timestamped queue the pump applies at block
// boundaries, so sequenced music keeps time on the audio clock instead of on
// the interpreter's.
//
// Live playing already works: `synth.press()` from Python lands under the
// pump lock and is heard within a block. What has no home is the OTHER half
// of playing music -- a drum machine, a soundtrack player, a MIDI file --
// where the note's moment was decided in advance and the interpreter is only
// the courier. Those keep time in Python today, so a collection, a screen
// redraw or a flash write lands as a late note.
//
// --- the split -----------------------------------------------------------
//
// SCHEDULING is a control-path call on the interpreter thread. It may raise,
// it may allocate, it may refuse: every type check, every range check, every
// float box and every protocol lookup happens there, once.
//
// APPLYING runs on the pump's thread inside the lock the pump already holds
// for the block. It is a handful of stores. Nothing it calls can allocate and
// nothing it calls can raise -- not "does not in practice", but "there is no
// call below it that could". That is why the closed set is closed: an
// operation earns its place by having an apply half that is stores, and the
// ones that cannot are named in the notes rather than smuggled in.
//
// --- the clock -----------------------------------------------------------
//
// One wrapping 32-bit frame counter, published by the pump with a release
// store and read by `audiopump.now()` with an acquire load. 32-bit ON PURPOSE,
// for the same reason the push ring is: a 64-bit store on RV32 or Xtensa is
// two 32-bit stores with a window between them, and a torn clock is a note in
// the wrong bar. It wraps every 2^32 frames -- 24.9 hours at 48 kHz -- and
// every comparison here is `(int32_t)(a - b) < 0`, which is correct as long
// as nothing is scheduled more than 2^31 frames (12.4 hours) ahead.
//
// --- what the collector sees ---------------------------------------------
//
// The event array is one `m_new` block, so the Notes and samples inside it
// are traced like any other heap object, and the queue itself is held in the
// pump's root array while it is attached. A Note pressed into a queue and
// then dropped by Python therefore survives until it is applied -- and stops
// being held the moment it is, because an applied slot is cleared rather than
// left behind by the memmove.

#include <stdint.h>
#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "audiocore/__init__.h"
#include "audiomixer/Mixer.h"
#include "audiomixer/MixerVoice.h"
#include "audiomodal/Bank.h"
#include "shared/audiodsp_pump_lock.h"
#include "synthio/Note.h"
#include "synthio/Synthesizer.h"
#include "synthio/__init__.h"

#include "audiopump/audiopump_events.h"

typedef struct {
    uint32_t frame;      // wrapping, absolute, in frames since spawn()
    uint32_t token;      // what cancel() names
    mp_obj_t target;     // Synthesizer, MixerVoice or Bank
    mp_obj_t arg;        // Note / small-int note / sample / boxed level /
                         // a strike's flattened mode table
    uint8_t op;
    uint8_t loop;
} audiopump_event_t;

typedef struct {
    mp_obj_base_t base;
    audiopump_event_t *ev;   // `capacity` entries, kept sorted by frame
    uint16_t capacity;
    uint16_t count;
    uint32_t next_token;
    // Statistics. `applied`, `late` and `refused` are the pump's; the rest
    // are the interpreter's. Each word has one writer.
    uint32_t scheduled;
    uint32_t applied;
    uint32_t late;
    uint32_t cancelled;
    uint32_t dropped;        // refused at the door: the queue was full
    uint32_t refused;        // refused at apply: the target went away
} audiopump_events_obj_t;

// Wrapping order. Everything in the queue is inside one 2^31-frame window of
// the clock, so this is a total order over what is actually in it.
static inline bool audiopump_before(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

// --- applying, on the pump thread ----------------------------------------
//
// Read these four with the contract in mind: no allocation, no raise, no
// Python call, no iteration over a Python object.

static void audiopump_apply_press(audiopump_event_t *e, bool release,
    uint32_t *refused) {
    synthio_synthesizer_obj_t *synth = MP_OBJ_TO_PTR(e->target);
    if (audiosample_deinited(&synth->synth.base)) {
        // Deinited between scheduling and now. Refuse the event rather than
        // let the fault register stop the pump: a sequencer whose synth was
        // torn down should go quiet, not take the audio with it.
        (*refused)++;
        return;
    }
    if (!release && !mp_obj_is_small_int(e->arg)) {
        // `synthio_note_start` minus its `synthio_note_recalculate` half.
        // The recalculate is the part that reads a namedtuple with
        // mp_obj_get_float, so it ran at schedule time, where raising is
        // free; anything that changes the note's envelope afterwards rebuilds
        // the definition itself (Note.c, set_envelope). What is left is four
        // filter-state memsets.
        synthio_note_obj_t *note = MP_OBJ_TO_PTR(e->arg);
        for (size_t i = 0; i < SYNTHIO_NOTE_MAX_FILTER_STAGES; i++) {
            synthio_biquad_filter_reset(&note->filter_state[i]);
        }
    }
    // Pure C over the span arrays; it takes the pump lock itself, which the
    // recursive mutex allows from in here.
    if (release) {
        (void)synthio_span_change_note(&synth->synth, e->arg, SYNTHIO_SILENCE);
    } else {
        (void)synthio_span_change_note(&synth->synth, SYNTHIO_SILENCE, e->arg);
    }
}

static void audiopump_apply_release_all(audiopump_event_t *e,
    uint32_t *refused) {
    synthio_synthesizer_obj_t *synth = MP_OBJ_TO_PTR(e->target);
    if (audiosample_deinited(&synth->synth.base)) {
        (*refused)++;
        return;
    }
    common_hal_synthio_synthesizer_release_all(synth);
}

static void audiopump_apply_play(audiopump_event_t *e, uint32_t *refused) {
    audiomixer_mixervoice_obj_t *voice = MP_OBJ_TO_PTR(e->target);
    if (audiosample_deinited(MP_OBJ_TO_PTR(e->arg))
        || audiosample_deinited(&voice->parent->base)) {
        (*refused)++;
        return;
    }
    // common_hal_audiomixer_mixervoice_play's locked body, minus the two
    // things that are not stores: `audiosample_must_match`, which ran at
    // schedule time, and the ValueError, which becomes a count. The priming
    // pull inside reset() is a pull on the pump's own thread and re-takes the
    // lock the pump is already holding, which is the case the recursive mutex
    // exists for.
    voice->sample = e->arg;
    voice->loop = e->loop != 0;
    common_hal_audiomixer_mixervoice_reset(voice);
    if (voice->loop && voice->buffer_length == 0 && !voice->more_data) {
        // audiodsp#85: a looping source that cannot fill one packed word never
        // ends. play() raises here; an event counts it and leaves the voice
        // stopped rather than half-started.
        voice->sample = NULL;
        voice->loop = false;
        (*refused)++;
    }
}

// A strike: the whole mode table, written into a bank that is already
// running. Three floats a mode, flattened and range-checked when the event
// was scheduled, so this is a loop of stores over a float buffer -- no
// allocation, no raise, no type lookup. Rows the table does not reach are
// silenced, which is `set_modes()`'s rule: a shorter table is a smaller drum
// and not a chord of two.
//
// `audiodsp_modal_config_finish` is not called here because `get_buffer`
// already calls it at the top of every block, so the coefficients derived
// from these numbers are recomputed before the first sample that uses them.
static void audiopump_apply_strike(audiopump_event_t *e) {
    audiomodal_bank_obj_t *bank = MP_OBJ_TO_PTR(e->target);
    mp_buffer_info_t table;
    if (!mp_get_buffer(e->arg, &table, MP_BUFFER_READ)) {
        return;
    }
    const float *rows = (const float *)table.buf;
    const uint32_t count =
        (uint32_t)(table.len / sizeof(float)) / 3u;
    uint32_t index = 0;
    for (; index < count && index < bank->config.mode_count; ++index) {
        audiodsp_modal_set_mode(&bank->config, index, rows[index * 3u],
            rows[index * 3u + 1u], rows[index * 3u + 2u]);
    }
    for (; index < bank->config.mode_count; ++index) {
        audiodsp_modal_set_mode(&bank->config, index, 0.0f,
            AUDIODSP_MODAL_MIN_DECAY, 0.0f);
    }
}

uint32_t audiopump_events_apply(mp_obj_t queue, uint32_t now,
    uint32_t block_frames) {
    audiopump_events_obj_t *q = MP_OBJ_TO_PTR(queue);
    if (q->count == 0) {
        return 0;
    }
    // The block about to be pulled covers [now, end). An event behind `now`
    // is late and lands here too, which is the same comparison.
    const uint32_t end = now + block_frames;
    uint16_t n = 0;
    while (n < q->count && audiopump_before(q->ev[n].frame, end)) {
        audiopump_event_t *e = &q->ev[n];
        if (audiopump_before(e->frame, now)) {
            q->late++;
        }
        switch (e->op) {
            case AUDIOPUMP_OP_PRESS:
                audiopump_apply_press(e, false, &q->refused);
                break;
            case AUDIOPUMP_OP_RELEASE:
                audiopump_apply_press(e, true, &q->refused);
                break;
            case AUDIOPUMP_OP_RELEASE_ALL:
                audiopump_apply_release_all(e, &q->refused);
                break;
            case AUDIOPUMP_OP_PLAY:
                audiopump_apply_play(e, &q->refused);
                break;
            case AUDIOPUMP_OP_STOP:
                ((audiomixer_mixervoice_obj_t *)MP_OBJ_TO_PTR(e->target))
                ->sample = NULL;
                break;
            case AUDIOPUMP_OP_LEVEL:
                // One store. The float was boxed at schedule time, which is
                // the whole reason this can be an event at all --
                // synthio_block_assign_slot calls mp_obj_new_float.
                ((audiomixer_mixervoice_obj_t *)MP_OBJ_TO_PTR(e->target))
                ->level.obj = e->arg;
                break;
            case AUDIOPUMP_OP_STRIKE:
                audiopump_apply_strike(e);
                break;
            case AUDIOPUMP_OP_CHOKE:
                audiodsp_modal_reset(
                    &((audiomodal_bank_obj_t *)MP_OBJ_TO_PTR(e->target))
                    ->state);
                break;
            default:
                break;
        }
        n++;
    }
    if (n == 0) {
        return 0;
    }
    q->applied += n;
    q->count -= n;
    memmove(&q->ev[0], &q->ev[n], (size_t)q->count * sizeof(audiopump_event_t));
    // Clear what the memmove left behind. Without this the queue goes on
    // holding a Note the collector could otherwise have taken, and "scheduled
    // objects stay alive until applied" quietly becomes "forever".
    memset(&q->ev[q->count], 0, (size_t)n * sizeof(audiopump_event_t));
    return n;
}

// --- scheduling, on the interpreter thread --------------------------------

static void audiopump_events_validate(audiopump_events_obj_t *q, uint32_t op,
    mp_obj_t target, mp_obj_t arg, bool loop, audiopump_event_t *out) {
    (void)q;
    out->op = (uint8_t)op;
    out->target = target;
    out->arg = arg;
    out->loop = loop ? 1 : 0;

    switch (op) {
        case AUDIOPUMP_OP_PRESS:
        case AUDIOPUMP_OP_RELEASE:
        case AUDIOPUMP_OP_RELEASE_ALL: {
            if (!mp_obj_is_type(target, &synthio_synthesizer_type)) {
                mp_raise_TypeError(MP_ERROR_TEXT(
                    "press/release want a Synthesizer"));
            }
            synthio_synthesizer_obj_t *synth = MP_OBJ_TO_PTR(target);
            audiosample_check_for_deinit(&synth->synth.base);
            if (op == AUDIOPUMP_OP_RELEASE_ALL) {
                out->arg = mp_const_none;
                break;
            }
            if (mp_obj_is_small_int(arg)) {
                const mp_int_t n = mp_obj_get_int(arg);
                if (n < 0 || n > 127) {
                    mp_raise_ValueError(MP_ERROR_TEXT(
                        "a MIDI note is 0..127"));
                }
            } else if (mp_obj_is_type(arg, &synthio_note_type)) {
                // The half of press() that can raise, done here. After this
                // the note's envelope definition is current for this
                // synthesizer's rate, so the apply half is memsets.
                synthio_note_recalculate(MP_OBJ_TO_PTR(arg),
                    (int32_t)synth->synth.base.sample_rate);
            } else {
                mp_raise_TypeError(MP_ERROR_TEXT(
                    "a scheduled note is a Note or a MIDI number -- an "
                    "iterable of them is several events"));
            }
            break;
        }
        case AUDIOPUMP_OP_PLAY:
        case AUDIOPUMP_OP_STOP:
        case AUDIOPUMP_OP_LEVEL: {
            if (!mp_obj_is_type(target, &audiomixer_mixervoice_type)) {
                mp_raise_TypeError(MP_ERROR_TEXT(
                    "play/stop/level want a MixerVoice"));
            }
            audiomixer_mixervoice_obj_t *voice = MP_OBJ_TO_PTR(target);
            if (voice->parent == NULL) {
                mp_raise_ValueError(MP_ERROR_TEXT(
                    "the voice is not in a Mixer"));
            }
            if (op == AUDIOPUMP_OP_PLAY) {
                // The format check and the Resampler rate binding, both of
                // which raise. A sample that passes here is one the apply
                // half can install with two stores and a priming pull.
                audiosample_must_match(&voice->parent->base, arg, true);
            } else if (op == AUDIOPUMP_OP_STOP) {
                out->arg = mp_const_none;
            } else {
                // Box the level NOW. A float assigned into a block slot is
                // an allocation, and an allocation on the pump thread is the
                // thing this whole spike exists to avoid.
                synthio_block_slot_t slot;
                synthio_block_assign_slot(arg, &slot, MP_QSTR_level);
                out->arg = slot.obj;
            }
            break;
        }
        case AUDIOPUMP_OP_STRIKE:
        case AUDIOPUMP_OP_CHOKE: {
            if (!mp_obj_is_type(target, &audiomodal_bank_type)) {
                mp_raise_TypeError(MP_ERROR_TEXT(
                    "strike/choke want an audiomodal.Bank"));
            }
            audiomodal_bank_obj_t *bank = MP_OBJ_TO_PTR(target);
            audiosample_check_for_deinit(&bank->base);
            if (op == AUDIOPUMP_OP_CHOKE) {
                out->arg = mp_const_none;
                break;
            }
            // Everything that can refuse happens HERE. The apply half reads
            // a float buffer and stores, and a table that reached it in the
            // wrong shape would have to raise on the pump thread, which is
            // the one thing the queue exists to make impossible.
            mp_buffer_info_t table;
            if (!mp_get_buffer(arg, &table, MP_BUFFER_READ)
                || table.typecode != 'f') {
                mp_raise_TypeError(MP_ERROR_TEXT(
                    "a strike's table is an array('f') of "
                    "frequency, decay, gain per mode"));
            }
            const size_t floats = table.len / sizeof(float);
            if (floats == 0 || floats % 3u != 0) {
                mp_raise_ValueError(MP_ERROR_TEXT(
                    "a strike's table needs three floats a mode"));
            }
            if (floats / 3u > bank->config.mode_count) {
                mp_raise_msg_varg(&mp_type_ValueError,
                    MP_ERROR_TEXT("table has %d modes, bank holds %d"),
                    (int)(floats / 3u), (int)bank->config.mode_count);
            }
            // The buffer OBJECT is what the event holds, so the collector
            // keeps the floats alive until the strike lands. A caller that
            // rewrites the array before then changes the strike, which is
            // the same contract a scheduled Note already has.
            out->arg = arg;
            break;
        }
        default:
            mp_raise_ValueError(MP_ERROR_TEXT("unknown op"));
    }
}

static mp_obj_t audiopump_events_at(size_t n_args, const mp_obj_t *args) {
    audiopump_events_obj_t *q = MP_OBJ_TO_PTR(args[0]);
    // Masked rather than range-checked: `now() + n` in Python is an
    // arbitrary-precision int that walks past 2^32 the first time the clock
    // wraps, and the arithmetic below is modulo 2^32 anyway.
    const uint32_t frame = (uint32_t)(mp_obj_get_int_truncated(args[1])
        & 0xffffffffu);
    const uint32_t op = (uint32_t)mp_obj_get_int(args[2]);
    mp_obj_t target = args[3];
    mp_obj_t arg = n_args > 4 ? args[4] : mp_const_none;
    const bool loop = n_args > 5 ? mp_obj_is_true(args[5]) : false;

    audiopump_event_t e;
    memset(&e, 0, sizeof(e));
    audiopump_events_validate(q, op, target, arg, loop, &e);
    e.frame = frame;

    // Everything above could raise and none of it is inside the lock. What
    // follows is a compare, a memmove and a store.
    audiodsp_pump_lock_acquire();
    if (q->count >= q->capacity) {
        q->dropped++;
        audiodsp_pump_lock_release();
        // A refusal with a count, not a raise from nowhere: a sequencer that
        // filled the queue wants to know it dropped a note, not to lose its
        // whole step to an exception.
        return MP_OBJ_NEW_SMALL_INT(0);
    }
    e.token = ++q->next_token;
    if (e.token == 0) {
        e.token = ++q->next_token;   // 0 is "refused", never a live token
    }
    uint16_t i = q->count;
    while (i > 0 && audiopump_before(e.frame, q->ev[i - 1].frame)) {
        i--;
    }
    memmove(&q->ev[i + 1], &q->ev[i],
        (size_t)(q->count - i) * sizeof(audiopump_event_t));
    q->ev[i] = e;
    q->count++;
    q->scheduled++;
    const uint32_t token = e.token;
    audiodsp_pump_lock_release();
    return mp_obj_new_int_from_uint(token);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiopump_events_at_obj, 4, 6,
    audiopump_events_at);

static mp_obj_t audiopump_events_cancel(mp_obj_t self_in, mp_obj_t token_in) {
    audiopump_events_obj_t *q = MP_OBJ_TO_PTR(self_in);
    const uint32_t token = (uint32_t)mp_obj_get_int_truncated(token_in);
    bool found = false;
    audiodsp_pump_lock_acquire();
    for (uint16_t i = 0; i < q->count; i++) {
        if (q->ev[i].token == token) {
            q->count--;
            memmove(&q->ev[i], &q->ev[i + 1],
                (size_t)(q->count - i) * sizeof(audiopump_event_t));
            memset(&q->ev[q->count], 0, sizeof(audiopump_event_t));
            q->cancelled++;
            found = true;
            break;
        }
    }
    audiodsp_pump_lock_release();
    return mp_obj_new_bool(found);
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiopump_events_cancel_obj,
    audiopump_events_cancel);

static mp_obj_t audiopump_events_clear(mp_obj_t self_in) {
    audiopump_events_obj_t *q = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_acquire();
    const uint16_t was = q->count;
    q->count = 0;
    memset(q->ev, 0, (size_t)was * sizeof(audiopump_event_t));
    q->cancelled += was;
    audiodsp_pump_lock_release();
    return MP_OBJ_NEW_SMALL_INT(was);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_events_clear_obj,
    audiopump_events_clear);

// (scheduled, applied, late, cancelled, dropped, refused, depth, capacity)
static mp_obj_t audiopump_events_stats(mp_obj_t self_in) {
    audiopump_events_obj_t *q = MP_OBJ_TO_PTR(self_in);
    mp_obj_t items[8] = {
        mp_obj_new_int_from_uint(q->scheduled),
        mp_obj_new_int_from_uint(q->applied),
        mp_obj_new_int_from_uint(q->late),
        mp_obj_new_int_from_uint(q->cancelled),
        mp_obj_new_int_from_uint(q->dropped),
        mp_obj_new_int_from_uint(q->refused),
        mp_obj_new_int_from_uint(q->count),
        mp_obj_new_int_from_uint(q->capacity),
    };
    return mp_obj_new_tuple(8, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_events_stats_obj,
    audiopump_events_stats);

static mp_obj_t audiopump_events_pending(mp_obj_t self_in) {
    audiopump_events_obj_t *q = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(q->count);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_events_pending_obj,
    audiopump_events_pending);

static mp_obj_t audiopump_events_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_capacity };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_capacity, MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = 64 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed),
        allowed, args);

    const mp_int_t capacity = args[ARG_capacity].u_int;
    if (capacity < 1 || capacity > 4096) {
        mp_raise_ValueError(MP_ERROR_TEXT("capacity is 1..4096"));
    }

    audiopump_events_obj_t *q = mp_obj_malloc(audiopump_events_obj_t,
        (const mp_obj_type_t *)&audiopump_events_type);
    // One block, traced conservatively like every other, which is what makes
    // a scheduled Note reachable from the collector's point of view.
    q->ev = m_new(audiopump_event_t, capacity);
    memset(q->ev, 0, (size_t)capacity * sizeof(audiopump_event_t));
    q->capacity = (uint16_t)capacity;
    q->count = 0;
    q->next_token = 0;
    q->scheduled = 0;
    q->applied = 0;
    q->late = 0;
    q->cancelled = 0;
    q->dropped = 0;
    q->refused = 0;
    return MP_OBJ_FROM_PTR(q);
}

static const mp_rom_map_elem_t audiopump_events_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_at), MP_ROM_PTR(&audiopump_events_at_obj) },
    { MP_ROM_QSTR(MP_QSTR_cancel), MP_ROM_PTR(&audiopump_events_cancel_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&audiopump_events_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&audiopump_events_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_pending),
      MP_ROM_PTR(&audiopump_events_pending_obj) },
};
static MP_DEFINE_CONST_DICT(audiopump_events_locals,
    audiopump_events_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    audiopump_events_type,
    MP_QSTR_Events,
    MP_TYPE_FLAG_NONE,
    make_new, audiopump_events_make_new,
    locals_dict, &audiopump_events_locals
    );
