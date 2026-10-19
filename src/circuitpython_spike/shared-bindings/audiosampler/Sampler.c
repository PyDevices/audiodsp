// audiosampler.Sampler bindings for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <string.h>

#include "shared-bindings/audiosampler/Sampler.h"
#include "shared-bindings/audiosampler/SamplerVoice.h"
#include "shared-bindings/audiocore/__init__.h"

#include "py/objproperty.h"
#include "shared/runtime/context_manager_helpers.h"
#include "py/runtime.h"

//| class Sampler:
//|     """Play recorded notes: N voices, each reading a buffer at its own pitch.
//|
//|     Renders signed 16-bit audio and hands out a block at a time. It is an
//|     audio source like `audiomixer.Mixer`: play it into an output, a mixer
//|     voice or an effect. Notes are started on its voices, ``sampler.voice[i]``.
//|
//|         sampler = audiosampler.Sampler(voice_count=8, sample_rate=48000)
//|         audio_out.play(sampler)
//|         sampler.voice[0].play(piano_c4, rate=44100, pitch=2 ** (5 / 12),
//|                               loop_start=11000, loop_end=19000,
//|                               loop_mode=audiosampler.LOOP_UNTIL_RELEASE,
//|                               attack=0.005, release=0.3)
//|         sampler.voice[0].release()
//|     """
//|
//|     def __init__(
//|         self,
//|         *,
//|         voice_count: int = 8,
//|         buffer_size: int = 1024,
//|         channel_count: int = 2,
//|         bits_per_sample: int = 16,
//|         samples_signed: bool = True,
//|         sample_rate: int = 48000,
//|     ) -> None:
//|         """Create a sampler. ``voice_count`` (1 to 64) and ``channel_count``
//|         (1 or 2) size the allocation and cannot change afterwards.
//|         ``buffer_size`` is the output block in bytes, 256 frames by
//|         default. ``bits_per_sample`` and ``samples_signed`` are accepted so
//|         a Sampler can stand where a Mixer stood, and must be 16 and True."""
//|         ...

static mp_obj_t audiosampler_sampler_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_voice_count, ARG_buffer_size, ARG_channel_count,
           ARG_bits_per_sample, ARG_samples_signed, ARG_sample_rate };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_voice_count, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 8} },
        { MP_QSTR_buffer_size, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_channel_count, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 2} },
        { MP_QSTR_bits_per_sample, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 16} },
        { MP_QSTR_samples_signed, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = true} },
        { MP_QSTR_sample_rate, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 48000} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t voice_count = args[ARG_voice_count].u_int;
    if (voice_count < 1 || voice_count > (mp_int_t)AUDIODSP_SAMPLER_MAX_VOICES) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("voice_count must be 1 to %d"),
            (int)AUDIODSP_SAMPLER_MAX_VOICES);
    }
    mp_int_t channel_count = args[ARG_channel_count].u_int;
    if (channel_count < 1 || channel_count > 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("channel_count must be 1 or 2"));
    }
    mp_int_t sample_rate = args[ARG_sample_rate].u_int;
    if (sample_rate < 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("sample_rate must be at least 1"));
    }
    if (args[ARG_bits_per_sample].u_int != 16 || !args[ARG_samples_signed].u_bool) {
        mp_raise_ValueError(MP_ERROR_TEXT("Sampler renders signed 16-bit only"));
    }
    const mp_int_t width = 2 * channel_count;
    mp_int_t frames = AUDIODSP_SAMPLER_FRAMES;
    if (args[ARG_buffer_size].u_int != 0) {
        frames = args[ARG_buffer_size].u_int / width;
        if (frames < 1 || frames > (mp_int_t)AUDIODSP_SAMPLER_MAX_FRAMES) {
            mp_raise_msg_varg(&mp_type_ValueError,
                MP_ERROR_TEXT("buffer_size must be %d to %d bytes"),
                (int)width, (int)(AUDIODSP_SAMPLER_MAX_FRAMES * width));
        }
    }

    audiosampler_sampler_obj_t *self = mp_obj_malloc(audiosampler_sampler_obj_t,
        type);
    self->base.sample_rate = (uint32_t)sample_rate;
    self->base.max_buffer_length = (uint32_t)(frames * width);
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)channel_count;
    self->base.samples_signed = 1;
    self->base.single_buffer = false;

    self->voices = m_malloc((size_t)AUDIODSP_SAMPLER_SLOTS(voice_count) *
        sizeof(audiodsp_sampler_voice_t));
    self->mix = m_malloc((size_t)(frames * channel_count) * sizeof(float));
    self->buffer = m_malloc((size_t)(frames * width));
    audiodsp_sampler_init(&self->sampler, (uint32_t)sample_rate,
        (uint32_t)channel_count, (uint32_t)frames, (uint32_t)voice_count,
        self->voices, self->mix);

    self->voice = m_malloc((size_t)voice_count * sizeof(mp_obj_t));
    for (mp_int_t v = 0; v < voice_count; v++) {
        self->voice[v] = audiosampler_samplervoice_new(self, (uint32_t)v);
    }
    self->voice_tuple = mp_obj_new_tuple((size_t)voice_count, self->voice);
    return MP_OBJ_FROM_PTR(self);
}

//|     voice: Tuple[SamplerVoice, ...]
//|     """The voices, one `SamplerVoice` each. Read-only."""
static mp_obj_t audiosampler_sampler_obj_get_voice(mp_obj_t self_in) {
    audiosampler_sampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return MP_OBJ_FROM_PTR(self->voice_tuple);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_sampler_get_voice_obj,
    audiosampler_sampler_obj_get_voice);
MP_PROPERTY_GETTER(audiosampler_sampler_voice_obj,
    (mp_obj_t)&audiosampler_sampler_get_voice_obj);

//|     voice_count: int
//|     """How many voices this sampler was built with."""
static mp_obj_t audiosampler_sampler_obj_get_voice_count(mp_obj_t self_in) {
    audiosampler_sampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return MP_OBJ_NEW_SMALL_INT(self->sampler.voice_count);
}
MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_sampler_get_voice_count_obj,
    audiosampler_sampler_obj_get_voice_count);
MP_PROPERTY_GETTER(audiosampler_sampler_voice_count_obj,
    (mp_obj_t)&audiosampler_sampler_get_voice_count_obj);

//|     playing: bool
//|     """True while any voice sounds, a stopped note's fade included."""
static mp_obj_t audiosampler_sampler_obj_get_playing(mp_obj_t self_in) {
    audiosampler_sampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    for (uint32_t v = 0; v < self->sampler.voice_count; v++) {
        if (audiodsp_sampler_playing(&self->sampler, v) ||
            audiodsp_sampler_fading(&self->sampler, v)) {
            return mp_const_true;
        }
    }
    return mp_const_false;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_sampler_get_playing_obj,
    audiosampler_sampler_obj_get_playing);
MP_PROPERTY_GETTER(audiosampler_sampler_playing_obj,
    (mp_obj_t)&audiosampler_sampler_get_playing_obj);

//|     def stop(self) -> None:
//|         """Stop every voice at once, each with a voice's 5 ms fade."""
//|         ...
static mp_obj_t audiosampler_sampler_stop(mp_obj_t self_in) {
    audiosampler_sampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    for (uint32_t v = 0; v < self->sampler.voice_count; v++) {
        audiosampler_samplervoice_obj_t *voice =
            MP_OBJ_TO_PTR(self->voice[v]);
        if (audiodsp_sampler_audible(&self->sampler, v)) {
            voice->tail_sample = voice->sample;
        }
        audiodsp_sampler_stop(&self->sampler, v);
        voice->sample = mp_const_none;
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_sampler_stop_obj,
    audiosampler_sampler_stop);

//|     def deinit(self) -> None:
//|         """Stop every voice, let go of every buffer and release the node."""
//|         ...
static mp_obj_t audiosampler_sampler_deinit(mp_obj_t self_in) {
    audiosampler_sampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    for (uint32_t v = 0; v < self->sampler.voice_count; v++) {
        audiosampler_samplervoice_obj_t *voice =
            MP_OBJ_TO_PTR(self->voice[v]);
        audiodsp_sampler_cut(&self->sampler, v);
        voice->sample = mp_const_none;
        voice->tail_sample = mp_const_none;
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_sampler_deinit_obj,
    audiosampler_sampler_deinit);

//|     def __enter__(self) -> Sampler:
//|         """No-op used by Context Managers."""
//|         ...
//|
//|     def __exit__(self) -> None:
//|         """Automatically deinitializes the hardware when exiting a context."""
//|         ...
//|
static const mp_rom_map_elem_t audiosampler_sampler_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiosampler_sampler_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audiosampler_sampler_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_voice), MP_ROM_PTR(&audiosampler_sampler_voice_obj) },
    { MP_ROM_QSTR(MP_QSTR_voice_count),
      MP_ROM_PTR(&audiosampler_sampler_voice_count_obj) },
    { MP_ROM_QSTR(MP_QSTR_playing),
      MP_ROM_PTR(&audiosampler_sampler_playing_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiosampler_sampler_locals_dict,
    audiosampler_sampler_locals_dict_table);

static const audiosample_p_t audiosampler_sampler_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audiosampler_sampler_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audiosampler_sampler_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiosampler_sampler_type,
    MP_QSTR_Sampler,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiosampler_sampler_make_new,
    locals_dict, &audiosampler_sampler_locals_dict,
    protocol, &audiosampler_sampler_proto
    );
