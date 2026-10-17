// audiosampler.Sampler. See Sampler.h for provenance.
// SPDX-License-Identifier: MIT

#include "audiosampler/Sampler.h"
#include "audiosampler/SamplerVoice.h"

#include <string.h>

#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/objproperty.h"
#include "py/runtime.h"
#include "shared/audiodsp_pump_lock.h"

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
    // One output format, the one every other audiodsp node of ours renders.
    // The two arguments are here so a Sampler can stand where a Mixer stood,
    // and they say so if asked for anything else.
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

    self->voices = m_malloc((size_t)voice_count * sizeof(audiodsp_sampler_voice_t));
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

static mp_obj_t audiosampler_sampler_obj_get_voice(mp_obj_t self_in) {
    audiosampler_sampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return MP_OBJ_FROM_PTR(self->voice_tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_sampler_get_voice_obj,
    audiosampler_sampler_obj_get_voice);
MP_PROPERTY_GETTER(audiosampler_sampler_voice_obj,
    (mp_obj_t)&audiosampler_sampler_get_voice_obj);

static mp_obj_t audiosampler_sampler_obj_get_voice_count(mp_obj_t self_in) {
    audiosampler_sampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return MP_OBJ_NEW_SMALL_INT(self->sampler.voice_count);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_sampler_get_voice_count_obj,
    audiosampler_sampler_obj_get_voice_count);
MP_PROPERTY_GETTER(audiosampler_sampler_voice_count_obj,
    (mp_obj_t)&audiosampler_sampler_get_voice_count_obj);

// True while any voice sounds.
static mp_obj_t audiosampler_sampler_obj_get_playing(mp_obj_t self_in) {
    audiosampler_sampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    for (uint32_t v = 0; v < self->sampler.voice_count; v++) {
        if (audiodsp_sampler_playing(&self->sampler, v)) {
            return mp_const_true;
        }
    }
    return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_sampler_get_playing_obj,
    audiosampler_sampler_obj_get_playing);
MP_PROPERTY_GETTER(audiosampler_sampler_playing_obj,
    (mp_obj_t)&audiosampler_sampler_get_playing_obj);

// Every voice at once, no fade.
static mp_obj_t audiosampler_sampler_stop(mp_obj_t self_in) {
    audiosampler_sampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    audiodsp_pump_lock_acquire();
    for (uint32_t v = 0; v < self->sampler.voice_count; v++) {
        audiodsp_sampler_stop(&self->sampler, v);
        ((audiosampler_samplervoice_obj_t *)MP_OBJ_TO_PTR(self->voice[v]))
            ->sample = mp_const_none;
    }
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_sampler_stop_obj,
    audiosampler_sampler_stop);

static audioio_get_buffer_result_t audiosampler_sampler_get_buffer(
    mp_obj_t self_in, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    audiosampler_sampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_sampler_render_s16(&self->sampler, self->buffer);
    // A voice that ran out this block lets go of its buffer here, so a
    // one-shot drum sample is reclaimable without anyone calling stop().
    for (uint32_t v = 0; v < self->sampler.voice_count; v++) {
        if (!audiodsp_sampler_playing(&self->sampler, v)) {
            ((audiosampler_samplervoice_obj_t *)MP_OBJ_TO_PTR(self->voice[v]))
                ->sample = mp_const_none;
        }
    }
    *buffer = (uint8_t *)self->buffer;
    *buffer_length = self->base.max_buffer_length;
    return GET_BUFFER_MORE_DATA;
}

static void audiosampler_sampler_reset_buffer(mp_obj_t self_in,
    bool single_channel_output, uint8_t channel) {
    (void)self_in;
    (void)single_channel_output;
    (void)channel;
    // A host reset keeps every voice as it is, as audiomixer's does: a
    // sampler has no source to rewind, and a note that was sounding when the
    // output restarted should still be sounding afterwards.
}

// `deinit()` stops every voice and lets go of every buffer, and marks the
// node deinitialised, which is what makes every guarded entry point raise
// afterwards (audiodsp#58, #60, #63).
static mp_obj_t audiosampler_sampler_deinit(mp_obj_t self_in) {
    audiosampler_sampler_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    for (uint32_t v = 0; v < self->sampler.voice_count; v++) {
        audiodsp_sampler_stop(&self->sampler, v);
        ((audiosampler_samplervoice_obj_t *)MP_OBJ_TO_PTR(self->voice[v]))
            ->sample = mp_const_none;
    }
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_sampler_deinit_obj,
    audiosampler_sampler_deinit);

static const mp_rom_map_elem_t audiosampler_sampler_locals_table[] = {
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
static MP_DEFINE_CONST_DICT(audiosampler_sampler_locals,
    audiosampler_sampler_locals_table);

// No `.sources`: a sampler reads buffers, not nodes, so it is a leaf of the
// graph like audiocore.RawSample.
static const audiosample_p_t audiosampler_sampler_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = audiosampler_sampler_reset_buffer,
    .get_buffer = audiosampler_sampler_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiosampler_sampler_type,
    MP_QSTR_Sampler,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiosampler_sampler_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiosampler_sampler_locals,
    protocol, &audiosampler_sampler_proto
    );
