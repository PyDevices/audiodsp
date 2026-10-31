// audiosampler.SamplerVoice bindings for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "shared-bindings/audiosampler/SamplerVoice.h"
#include "shared-bindings/audiocore/__init__.h"

#include "py/objproperty.h"
#include "py/runtime.h"
#include "shared/runtime/context_manager_helpers.h"

//| class SamplerVoice:
//|     """One of a `Sampler`'s voices. Made by the sampler, never directly."""
//|

mp_obj_t audiosampler_samplervoice_new(audiosampler_sampler_obj_t *parent,
    uint32_t index) {
    audiosampler_samplervoice_obj_t *self = mp_obj_malloc(
        audiosampler_samplervoice_obj_t, &audiosampler_samplervoice_type);
    self->parent = parent;
    self->index = index;
    self->sample = mp_const_none;
    self->tail_sample = mp_const_none;
    self->level = mp_obj_new_float(1.0);
    self->pan = mp_obj_new_float(0.0);
    self->pitch = mp_obj_new_float(1.0);
    self->cutoff = mp_const_none;
    self->vibrato_depth = mp_obj_new_float(0.0);
    self->mod = NULL;
    return MP_OBJ_FROM_PTR(self);
}

// Gives the voice its filter, envelope and LFO state the first time it needs
// any of them, so a sampler whose notes never ask carries none.
static void samplervoice_need_mod(audiosampler_samplervoice_obj_t *self) {
    if (self->mod != NULL) {
        return;
    }
    audiodsp_sampler_mod_t *pair = m_malloc(2 * sizeof(audiodsp_sampler_mod_t));
    audiodsp_sampler_attach_mod(&self->parent->sampler, self->index, pair);
    self->mod = pair;
}

static float float_arg(mp_obj_t value, float fallback) {
    return value == mp_const_none ? fallback : (float)mp_obj_get_float(value);
}

//|     def play(
//|         self,
//|         buffer: ReadableBuffer,
//|         *,
//|         rate: Optional[float] = None,
//|         pitch: float = 1.0,
//|         loop_start: int = 0,
//|         loop_end: Optional[int] = None,
//|         loop_mode: int = LOOP_NONE,
//|         level: float = 1.0,
//|         pan: float = 0.0,
//|         delay: float = 0.0,
//|         attack: float = 0.0,
//|         hold: float = 0.0,
//|         decay: float = 0.0,
//|         sustain: float = 1.0,
//|         release: float = 0.0,
//|         cutoff: Optional[float] = None,
//|         resonance: float = 0.0,
//|         mod_delay: float = 0.0,
//|         mod_attack: float = 0.0,
//|         mod_hold: float = 0.0,
//|         mod_decay: float = 0.0,
//|         mod_sustain: float = 1.0,
//|         mod_release: float = 0.0,
//|         mod_to_pitch: float = 0.0,
//|         mod_to_cutoff: float = 0.0,
//|         vibrato_delay: float = 0.0,
//|         vibrato_rate: float = 0.0,
//|         vibrato_depth: float = 0.0,
//|         lfo_delay: float = 0.0,
//|         lfo_rate: float = 0.0,
//|         lfo_to_pitch: float = 0.0,
//|         lfo_to_cutoff: float = 0.0,
//|         lfo_to_volume: float = 0.0,
//|     ) -> None:
//|         """Start ``buffer`` (mono signed 16-bit) on this voice, replacing
//|         whatever it was playing, which fades out over 5 ms under the new
//|         note. ``rate`` is the buffer's own sample rate,
//|         the sampler's when left out, so the step per output frame is
//|         ``pitch * rate / sample_rate``. ``loop_end`` left out means the end
//|         of the buffer. The envelope stages are in seconds and ``sustain``
//|         is a linear level; ``delay`` postpones the start of the sound.
//|         Raises ValueError for a loop that does not fit the buffer or a
//|         ``loop_mode`` that is not 0, 1 or 3.
//|
//|         ``cutoff`` (Hz) and ``resonance`` (dB at the cutoff above DC) set a
//|         two-pole low-pass; 19.9 kHz or more with no resonance and nothing
//|         moving it is no filter. The ``mod_`` envelope moves pitch and
//|         cutoff by ``mod_to_pitch`` and ``mod_to_cutoff`` cents at full
//|         scale. The vibrato LFO moves pitch by ``vibrato_depth`` cents; the
//|         modulation LFO moves pitch and cutoff by ``lfo_to_pitch`` and
//|         ``lfo_to_cutoff`` cents and the level by ``lfo_to_volume`` dB.
//|         Rates are in Hz and delays in seconds; both LFOs are triangles
//|         from zero, rising."""
//|         ...
static mp_obj_t audiosampler_samplervoice_play(size_t n_args,
    const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_buffer, ARG_rate, ARG_pitch, ARG_loop_start, ARG_loop_end,
           ARG_loop_mode, ARG_level, ARG_pan, ARG_delay, ARG_attack, ARG_hold,
           ARG_decay, ARG_sustain, ARG_release,
           ARG_cutoff, ARG_resonance, ARG_mod_delay, ARG_mod_attack, ARG_mod_hold, ARG_mod_decay, ARG_mod_sustain, ARG_mod_release, ARG_mod_to_pitch, ARG_mod_to_cutoff, ARG_vibrato_delay, ARG_vibrato_rate, ARG_vibrato_depth, ARG_lfo_delay, ARG_lfo_rate, ARG_lfo_to_pitch, ARG_lfo_to_cutoff, ARG_lfo_to_volume };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_buffer, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_rate, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_pitch, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_loop_start, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_loop_end, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_loop_mode, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_level, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_pan, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_delay, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_attack, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_hold, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_decay, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_sustain, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_release, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_cutoff, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_resonance, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_mod_delay, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_mod_attack, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_mod_hold, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_mod_decay, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_mod_sustain, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_mod_release, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_mod_to_pitch, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_mod_to_cutoff, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_vibrato_delay, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_vibrato_rate, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_vibrato_depth, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_lfo_delay, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_lfo_rate, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_lfo_to_pitch, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_lfo_to_cutoff, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_lfo_to_volume, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
    };
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    audiosample_check_for_deinit(&self->parent->base);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t info;
    mp_get_buffer_raise(args[ARG_buffer].u_obj, &info, MP_BUFFER_READ);
    if (info.len < 2 || (info.len % 2) != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "buffer must hold at least one whole 16-bit sample"));
    }
    if (args[ARG_loop_start].u_int < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("loop_start must not be negative"));
    }
    mp_int_t loop_end = 0;
    if (args[ARG_loop_end].u_obj != mp_const_none) {
        loop_end = mp_obj_get_int(args[ARG_loop_end].u_obj);
        if (loop_end < 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("loop_end must not be negative"));
        }
    }

    audiodsp_sampler_note_t note;
    audiodsp_sampler_note_init(&note);
    note.rate = float_arg(args[ARG_rate].u_obj, 0.0f);
    note.pitch = float_arg(args[ARG_pitch].u_obj, 1.0f);
    note.loop_start = (uint32_t)args[ARG_loop_start].u_int;
    note.loop_end = (uint32_t)loop_end;
    note.loop_mode = (uint32_t)args[ARG_loop_mode].u_int;
    note.level = float_arg(args[ARG_level].u_obj, 1.0f);
    note.pan = float_arg(args[ARG_pan].u_obj, 0.0f);
    note.delay = float_arg(args[ARG_delay].u_obj, 0.0f);
    note.attack = float_arg(args[ARG_attack].u_obj, 0.0f);
    note.hold = float_arg(args[ARG_hold].u_obj, 0.0f);
    note.decay = float_arg(args[ARG_decay].u_obj, 0.0f);
    note.sustain = float_arg(args[ARG_sustain].u_obj, 1.0f);
    note.release = float_arg(args[ARG_release].u_obj, 0.0f);
    note.cutoff = float_arg(args[ARG_cutoff].u_obj, 0.0f);
    note.resonance = float_arg(args[ARG_resonance].u_obj, 0.0f);
    note.mod_delay = float_arg(args[ARG_mod_delay].u_obj, 0.0f);
    note.mod_attack = float_arg(args[ARG_mod_attack].u_obj, 0.0f);
    note.mod_hold = float_arg(args[ARG_mod_hold].u_obj, 0.0f);
    note.mod_decay = float_arg(args[ARG_mod_decay].u_obj, 0.0f);
    note.mod_sustain = float_arg(args[ARG_mod_sustain].u_obj, 1.0f);
    note.mod_release = float_arg(args[ARG_mod_release].u_obj, 0.0f);
    note.mod_to_pitch = float_arg(args[ARG_mod_to_pitch].u_obj, 0.0f);
    note.mod_to_cutoff = float_arg(args[ARG_mod_to_cutoff].u_obj, 0.0f);
    note.vibrato_delay = float_arg(args[ARG_vibrato_delay].u_obj, 0.0f);
    note.vibrato_rate = float_arg(args[ARG_vibrato_rate].u_obj, 0.0f);
    note.vibrato_depth = float_arg(args[ARG_vibrato_depth].u_obj, 0.0f);
    note.lfo_delay = float_arg(args[ARG_lfo_delay].u_obj, 0.0f);
    note.lfo_rate = float_arg(args[ARG_lfo_rate].u_obj, 0.0f);
    note.lfo_to_pitch = float_arg(args[ARG_lfo_to_pitch].u_obj, 0.0f);
    note.lfo_to_cutoff = float_arg(args[ARG_lfo_to_cutoff].u_obj, 0.0f);
    note.lfo_to_volume = float_arg(args[ARG_lfo_to_volume].u_obj, 0.0f);
    if (audiodsp_sampler_note_modulated(&note)) {
        samplervoice_need_mod(self);
    }

    bool handed = audiodsp_sampler_audible(&self->parent->sampler, self->index);
    if (!audiodsp_sampler_play(&self->parent->sampler, self->index,
        (const int16_t *)(const void *)info.buf, (uint32_t)(info.len / 2),
        &note)) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "loop_mode must be 0, 1 or 3 and the loop must fit the buffer"));
    }
    // The note this replaced fades out on the tail, still reading its buffer.
    if (handed) {
        self->tail_sample = self->sample;
    }
    self->sample = args[ARG_buffer].u_obj;
    self->level = args[ARG_level].u_obj == mp_const_none
        ? mp_obj_new_float(1.0) : args[ARG_level].u_obj;
    self->pan = args[ARG_pan].u_obj == mp_const_none
        ? mp_obj_new_float(0.0) : args[ARG_pan].u_obj;
    self->pitch = args[ARG_pitch].u_obj == mp_const_none
        ? mp_obj_new_float(1.0) : args[ARG_pitch].u_obj;
    self->cutoff = args[ARG_cutoff].u_obj;
    self->vibrato_depth = args[ARG_vibrato_depth].u_obj == mp_const_none
        ? mp_obj_new_float(0.0) : args[ARG_vibrato_depth].u_obj;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(audiosampler_samplervoice_play_obj, 1,
    audiosampler_samplervoice_play);

//|     def release(self) -> None:
//|         """Let the key go: enter the release stage. In `LOOP_UNTIL_RELEASE`
//|         the voice also stops looping and plays on to the end."""
//|         ...
static mp_obj_t audiosampler_samplervoice_release(mp_obj_t self_in) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    audiodsp_sampler_release(&self->parent->sampler, self->index);
    if (!audiodsp_sampler_playing(&self->parent->sampler, self->index)) {
        self->sample = mp_const_none;
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_samplervoice_release_obj,
    audiosampler_samplervoice_release);

//|     def stop(self) -> None:
//|         """Stop the voice. It is free at once, and the note fades to
//|         silence over 5 ms rather than being cut."""
//|         ...
static mp_obj_t audiosampler_samplervoice_stop(mp_obj_t self_in) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    if (audiodsp_sampler_audible(&self->parent->sampler, self->index)) {
        self->tail_sample = self->sample;
    }
    audiodsp_sampler_stop(&self->parent->sampler, self->index);
    self->sample = mp_const_none;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_samplervoice_stop_obj,
    audiosampler_samplervoice_stop);

//|     playing: bool
//|     """True while this voice sounds. Read-only."""
static mp_obj_t audiosampler_samplervoice_obj_get_playing(mp_obj_t self_in) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    return mp_obj_new_bool(
        audiodsp_sampler_playing(&self->parent->sampler, self->index));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_samplervoice_get_playing_obj,
    audiosampler_samplervoice_obj_get_playing);
MP_PROPERTY_GETTER(audiosampler_samplervoice_playing_obj,
    (mp_obj_t)&audiosampler_samplervoice_get_playing_obj);

//|     level: float
//|     """The voice's level, 0 to 1. A change lands as a ramp across the next
//|     block."""
static mp_obj_t audiosampler_samplervoice_obj_get_level(mp_obj_t self_in) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    return self->level;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_samplervoice_get_level_obj,
    audiosampler_samplervoice_obj_get_level);
static mp_obj_t audiosampler_samplervoice_obj_set_level(mp_obj_t self_in,
    mp_obj_t value) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    audiodsp_sampler_set_level(&self->parent->sampler, self->index,
        (float)mp_obj_get_float(value));
    self->level = value;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiosampler_samplervoice_set_level_obj,
    audiosampler_samplervoice_obj_set_level);
MP_PROPERTY_GETSET(audiosampler_samplervoice_level_obj,
    (mp_obj_t)&audiosampler_samplervoice_get_level_obj,
    (mp_obj_t)&audiosampler_samplervoice_set_level_obj);

//|     pan: float
//|     """Where the voice sits, -1 (left) to 1 (right), as a linear balance.
//|     Ignored on a mono sampler. A change lands as a ramp."""
static mp_obj_t audiosampler_samplervoice_obj_get_pan(mp_obj_t self_in) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    return self->pan;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_samplervoice_get_pan_obj,
    audiosampler_samplervoice_obj_get_pan);
static mp_obj_t audiosampler_samplervoice_obj_set_pan(mp_obj_t self_in,
    mp_obj_t value) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    audiodsp_sampler_set_pan(&self->parent->sampler, self->index,
        (float)mp_obj_get_float(value));
    self->pan = value;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiosampler_samplervoice_set_pan_obj,
    audiosampler_samplervoice_obj_set_pan);
MP_PROPERTY_GETSET(audiosampler_samplervoice_pan_obj,
    (mp_obj_t)&audiosampler_samplervoice_get_pan_obj,
    (mp_obj_t)&audiosampler_samplervoice_set_pan_obj);

//|     pitch: float
//|     """The playback ratio, 1.0 being the buffer's own pitch. Live, so a
//|     pitch bend is a series of writes here; each lands at the next block."""
//|
static mp_obj_t audiosampler_samplervoice_obj_get_pitch(mp_obj_t self_in) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    return self->pitch;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_samplervoice_get_pitch_obj,
    audiosampler_samplervoice_obj_get_pitch);
static mp_obj_t audiosampler_samplervoice_obj_set_pitch(mp_obj_t self_in,
    mp_obj_t value) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    audiodsp_sampler_set_pitch(&self->parent->sampler, self->index,
        (float)mp_obj_get_float(value));
    self->pitch = value;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiosampler_samplervoice_set_pitch_obj,
    audiosampler_samplervoice_obj_set_pitch);
MP_PROPERTY_GETSET(audiosampler_samplervoice_pitch_obj,
    (mp_obj_t)&audiosampler_samplervoice_get_pitch_obj,
    (mp_obj_t)&audiosampler_samplervoice_set_pitch_obj);

//|     cutoff: Optional[float]
//|     """The low-pass cutoff in Hz, None for no filter. Live: a change lands
//|     within 32 frames, and a filter switched on starts from rest."""
//|
static mp_obj_t audiosampler_samplervoice_obj_get_cutoff(mp_obj_t self_in) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    return self->cutoff;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_samplervoice_get_cutoff_obj,
    audiosampler_samplervoice_obj_get_cutoff);
static mp_obj_t audiosampler_samplervoice_obj_set_cutoff(mp_obj_t self_in,
    mp_obj_t value) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    float cutoff = value == mp_const_none ? 0.0f
        : (float)mp_obj_get_float(value);
    if (cutoff > 0.0f) {
        samplervoice_need_mod(self);
    }
    audiodsp_sampler_set_cutoff(&self->parent->sampler, self->index, cutoff);
    self->cutoff = value;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiosampler_samplervoice_set_cutoff_obj,
    audiosampler_samplervoice_obj_set_cutoff);
MP_PROPERTY_GETSET(audiosampler_samplervoice_cutoff_obj,
    (mp_obj_t)&audiosampler_samplervoice_get_cutoff_obj,
    (mp_obj_t)&audiosampler_samplervoice_set_cutoff_obj);

//|     vibrato_depth: float
//|     """The vibrato LFO's depth in cents. Live, so a mod wheel is a series
//|     of writes here; each lands within 32 frames."""
//|
static mp_obj_t audiosampler_samplervoice_obj_get_vibrato_depth(
    mp_obj_t self_in) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    return self->vibrato_depth;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_samplervoice_get_vibrato_depth_obj,
    audiosampler_samplervoice_obj_get_vibrato_depth);
static mp_obj_t audiosampler_samplervoice_obj_set_vibrato_depth(
    mp_obj_t self_in, mp_obj_t value) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->parent->base);
    float cents = (float)mp_obj_get_float(value);
    if (cents != 0.0f) {
        samplervoice_need_mod(self);
    }
    audiodsp_sampler_set_vibrato_depth(&self->parent->sampler, self->index,
        cents);
    self->vibrato_depth = value;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiosampler_samplervoice_set_vibrato_depth_obj,
    audiosampler_samplervoice_obj_set_vibrato_depth);
MP_PROPERTY_GETSET(audiosampler_samplervoice_vibrato_depth_obj,
    (mp_obj_t)&audiosampler_samplervoice_get_vibrato_depth_obj,
    (mp_obj_t)&audiosampler_samplervoice_set_vibrato_depth_obj);

//|     def deinit(self) -> None:
//|         """Stop the voice and let go of its buffer. A voice has nothing of
//|         its own to release; this is `stop()`, with no fade, under the name
//|         every node answers to."""
//|         ...
//|
static mp_obj_t audiosampler_samplervoice_deinit(mp_obj_t self_in) {
    audiosampler_samplervoice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_sampler_cut(&self->parent->sampler, self->index);
    self->sample = mp_const_none;
    self->tail_sample = mp_const_none;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_samplervoice_deinit_obj,
    audiosampler_samplervoice_deinit);

static const mp_rom_map_elem_t audiosampler_samplervoice_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit),
      MP_ROM_PTR(&audiosampler_samplervoice_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiosampler_samplervoice_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_release),
      MP_ROM_PTR(&audiosampler_samplervoice_release_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audiosampler_samplervoice_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_playing),
      MP_ROM_PTR(&audiosampler_samplervoice_playing_obj) },
    { MP_ROM_QSTR(MP_QSTR_level), MP_ROM_PTR(&audiosampler_samplervoice_level_obj) },
    { MP_ROM_QSTR(MP_QSTR_pan), MP_ROM_PTR(&audiosampler_samplervoice_pan_obj) },
    { MP_ROM_QSTR(MP_QSTR_pitch), MP_ROM_PTR(&audiosampler_samplervoice_pitch_obj) },
    { MP_ROM_QSTR(MP_QSTR_cutoff), MP_ROM_PTR(&audiosampler_samplervoice_cutoff_obj) },
    { MP_ROM_QSTR(MP_QSTR_vibrato_depth),
      MP_ROM_PTR(&audiosampler_samplervoice_vibrato_depth_obj) },
};
static MP_DEFINE_CONST_DICT(audiosampler_samplervoice_locals_dict,
    audiosampler_samplervoice_locals_dict_table);

// No make_new: a voice belongs to the Sampler that made it.
MP_DEFINE_CONST_OBJ_TYPE(
    audiosampler_samplervoice_type,
    MP_QSTR_SamplerVoice,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    locals_dict, &audiosampler_samplervoice_locals_dict
    );
