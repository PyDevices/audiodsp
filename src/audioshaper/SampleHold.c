// audioshaper.SampleHold. See SampleHold.h for provenance.
// SPDX-License-Identifier: MIT

#include "audioshaper/SampleHold.h"

#include <string.h>

#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/objproperty.h"
#include "py/runtime.h"
#include "shared/audioif_pump_lock.h"

// The ratio is one setting, not two, so both halves arrive together
// everywhere -- the constructor and `set()` share this. A node left holding
// half of a new ratio would be exactly the kind of transient state the exact
// accumulator exists to rule out.
static void samplehold_check_ratio(mp_int_t num, mp_int_t den) {
    if (num < 1 || den < 1 || den > num ||
        num > (mp_int_t)AUDIOIF_SAMPLEHOLD_MAX_RATIO) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "num and den must be whole, den <= num (a hold cannot invent "
            "frames)"));
    }
}

static mp_obj_t audioshaper_samplehold_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_source, ARG_num, ARG_den };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_source, MP_ARG_REQUIRED | MP_ARG_OBJ, {} },
        { MP_QSTR_num, MP_ARG_INT, { .u_int = 1 } },
        { MP_QSTR_den, MP_ARG_INT, { .u_int = 1 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed),
        allowed, args);
    samplehold_check_ratio(args[ARG_num].u_int, args[ARG_den].u_int);

    mp_obj_t source = args[ARG_source].u_obj;
    audiosample_base_t *src = audiosample_check(source);
    uint32_t frame_bytes =
        (uint32_t)(src->bits_per_sample / 8u) * src->channel_count;
    if (frame_bytes < 1u ||
        frame_bytes > AUDIOIF_SAMPLEHOLD_MAX_FRAME_BYTES) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "source frames must be 1 or 2 channels of 8- or 16-bit audio"));
    }

    audioshaper_samplehold_obj_t *self =
        mp_obj_malloc(audioshaper_samplehold_obj_t, type);
    // The format is the source's own: this node resamples nothing, so every
    // field downstream reads is the one it would have read from the source.
    self->base.sample_rate = src->sample_rate;
    self->base.channel_count = src->channel_count;
    self->base.bits_per_sample = src->bits_per_sample;
    self->base.samples_signed = src->samples_signed;
    self->base.single_buffer = false;
    self->base.max_buffer_length = AUDIOIF_SAMPLEHOLD_FRAMES * frame_bytes;
    self->source = source;
    self->frame_bytes = (uint8_t)frame_bytes;
    self->pending = NULL;
    self->pending_frames = 0;
    self->source_done = false;
    self->source_exhausted = false;

    audioif_samplehold_config_init(&self->config,
        (uint32_t)args[ARG_num].u_int, (uint32_t)args[ARG_den].u_int);
    audioif_samplehold_state_init(&self->state, &self->config);
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audioshaper_samplehold_play(mp_obj_t self_in,
    mp_obj_t sample) {
    audioshaper_samplehold_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_base_t *src = audiosample_check(sample);
    // The format is fixed at construction -- everything downstream has read it
    // already -- so a source in a different one is refused rather than
    // reinterpreted.
    if (src->bits_per_sample != self->base.bits_per_sample ||
        src->channel_count != self->base.channel_count) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "source format does not match the one this node was built with"));
    }
    audioif_pump_lock_acquire();
    self->source = sample;
    self->pending = NULL;
    self->pending_frames = 0;
    self->source_done = false;
    self->source_exhausted = false;
    audioif_pump_lock_release();
    audioif_samplehold_reset(&self->state, &self->config);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audioshaper_samplehold_play_obj,
    audioshaper_samplehold_play);

static mp_obj_t audioshaper_samplehold_set(size_t n_args,
    const mp_obj_t *args, mp_map_t *kw_args) {
    enum { ARG_num, ARG_den };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_num, MP_ARG_REQUIRED | MP_ARG_INT, {} },
        { MP_QSTR_den, MP_ARG_REQUIRED | MP_ARG_INT, {} },
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args - 1, args + 1, kw_args, MP_ARRAY_SIZE(allowed),
        allowed, parsed);
    samplehold_check_ratio(parsed[ARG_num].u_int, parsed[ARG_den].u_int);
    audioshaper_samplehold_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (audioif_samplehold_config_set(&self->config,
        (uint32_t)parsed[ARG_num].u_int, (uint32_t)parsed[ARG_den].u_int)) {
        // A ratio that actually moved re-arms the accumulator, so the next
        // frame latches and the new staircase starts where the knob turned.
        // A ratio set to what it already was does nothing at all: a class
        // writes its settings on every block, and re-latching 187 times a
        // second would be a defect nobody asked for.
    audioif_pump_lock_acquire();
        audioif_samplehold_reset(&self->state, &self->config);
    audioif_pump_lock_release();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audioshaper_samplehold_set_obj, 1,
    audioshaper_samplehold_set);

static mp_obj_t audioshaper_samplehold_clear(mp_obj_t self_in) {
    audioshaper_samplehold_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audioif_pump_lock_acquire();
    audioif_samplehold_reset(&self->state, &self->config);
    audioif_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioshaper_samplehold_clear_obj,
    audioshaper_samplehold_clear);

static mp_obj_t audioshaper_samplehold_get_num(mp_obj_t self_in) {
    audioshaper_samplehold_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return MP_OBJ_NEW_SMALL_INT(self->config.num);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioshaper_samplehold_get_num_obj,
    audioshaper_samplehold_get_num);
static MP_PROPERTY_GETTER(audioshaper_samplehold_num_obj,
    (mp_obj_t)&audioshaper_samplehold_get_num_obj);

static mp_obj_t audioshaper_samplehold_get_den(mp_obj_t self_in) {
    audioshaper_samplehold_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return MP_OBJ_NEW_SMALL_INT(self->config.den);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioshaper_samplehold_get_den_obj,
    audioshaper_samplehold_get_den);
static MP_PROPERTY_GETTER(audioshaper_samplehold_den_obj,
    (mp_obj_t)&audioshaper_samplehold_get_den_obj);

static mp_obj_t audioshaper_samplehold_get_latency(mp_obj_t self_in) {
    // Zero, at every ratio, and it is a measurement rather than a hope: a
    // refresh latches the frame it is looking at and emits it in the same
    // frame, so 1/1 is the input byte for byte. What a hold displaces is an
    // event landing on a frame it drops, by up to ceil(num/den) - 1 frames --
    // the effect, not a delay of this node, and the class that turns this into
    // a rate knob is what reports that bound (audioif#97).
    audioshaper_samplehold_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return MP_OBJ_NEW_SMALL_INT(0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioshaper_samplehold_get_latency_obj,
    audioshaper_samplehold_get_latency);
static MP_PROPERTY_GETTER(audioshaper_samplehold_latency_obj,
    (mp_obj_t)&audioshaper_samplehold_get_latency_obj);

static audioio_get_buffer_result_t audioshaper_samplehold_get_buffer(
    mp_obj_t self_in, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    audioshaper_samplehold_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const uint32_t width = self->frame_bytes;
    uint32_t produced = 0;
    while (produced < AUDIOIF_SAMPLEHOLD_FRAMES) {
        if (self->pending_frames == 0) {
            if (self->source == MP_OBJ_NULL || self->source_exhausted ||
                self->source_done) {
                self->source_exhausted = true;
                break;
            }
            uint8_t *raw = NULL;
            uint32_t raw_bytes = 0;
            audioio_get_buffer_result_t result = audiosample_get_buffer(
                self->source, false, 0, &raw, &raw_bytes);
            if (result == GET_BUFFER_ERROR || raw == NULL ||
                raw_bytes < width) {
                self->source_exhausted = true;
                break;
            }
            self->pending = raw;
            self->pending_frames = raw_bytes / width;
            self->source_done = (result == GET_BUFFER_DONE);
        }
        uint32_t run = AUDIOIF_SAMPLEHOLD_FRAMES - produced;
        if (run > self->pending_frames) {
            run = self->pending_frames;
        }
        audioif_samplehold_process(&self->config, &self->state,
            &self->buffer[produced * width], self->pending, run, width);
        self->pending += run * width;
        self->pending_frames -= run;
        produced += run;
    }
    // One frame in, one frame out, and the source's own ending. A node that
    // manufactured silence here would move where a chain ends, and the pair of
    // `SpeedChanger`s this replaces did not.
    *buffer = self->buffer;
    *buffer_length = produced * width;
    if (produced == 0 || self->source_exhausted) {
        return GET_BUFFER_DONE;
    }
    return GET_BUFFER_MORE_DATA;
}

static void audioshaper_samplehold_reset_buffer(mp_obj_t self_in,
    bool single_channel_output, uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    audioshaper_samplehold_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->source != MP_OBJ_NULL && self->source != mp_const_none) {
        audiosample_reset_buffer(self->source, false, 0);
    }
    self->pending = NULL;
    self->pending_frames = 0;
    self->source_done = false;
    self->source_exhausted = false;
    audioif_samplehold_reset(&self->state, &self->config);
}

// `deinit()` releases what this binding holds and marks the node
// deinitialised, which is what makes every guarded entry point raise
// afterwards. Same shape as the Waveshaper beside it: the buffer goes with
// the object, and what is cleared is what the object holds a *reference* to --
// the source, so releasing the tail of a chain lets the GC reclaim the rest,
// and the borrowed pointer into the source's buffer, so nothing dangles.
static mp_obj_t audioshaper_samplehold_deinit(mp_obj_t self_in) {
    audioshaper_samplehold_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    audioif_pump_lock_acquire();
    self->source = mp_const_none;
    self->pending = NULL;
    self->pending_frames = 0;
    self->source_exhausted = true;
    audioif_pump_lock_release();
    audioif_samplehold_reset(&self->state, &self->config);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioshaper_samplehold_deinit_obj,
    audioshaper_samplehold_deinit);

static const mp_rom_map_elem_t audioshaper_samplehold_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit),
      MP_ROM_PTR(&audioshaper_samplehold_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play),
      MP_ROM_PTR(&audioshaper_samplehold_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_set),
      MP_ROM_PTR(&audioshaper_samplehold_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear),
      MP_ROM_PTR(&audioshaper_samplehold_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_num), MP_ROM_PTR(&audioshaper_samplehold_num_obj) },
    { MP_ROM_QSTR(MP_QSTR_den), MP_ROM_PTR(&audioshaper_samplehold_den_obj) },
    { MP_ROM_QSTR(MP_QSTR_latency),
      MP_ROM_PTR(&audioshaper_samplehold_latency_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audioshaper_samplehold_locals,
    audioshaper_samplehold_locals_table);

static const audiosample_p_t audioshaper_samplehold_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = audioshaper_samplehold_reset_buffer,
    .get_buffer = audioshaper_samplehold_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audioshaper_samplehold_type,
    MP_QSTR_SampleHold,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audioshaper_samplehold_make_new,
    attr, cp_compat_attr,
    locals_dict, &audioshaper_samplehold_locals,
    protocol, &audioshaper_samplehold_proto
    );
