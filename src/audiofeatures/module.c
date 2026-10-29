// audiofeatures: speech features for a wake word model. audiodsp's own; the
// engine is shared/audiodsp_features.c.
//
//   fe = audiofeatures.Frontend()
//   fe.feed(pcm, 48000, channel_count=2)      # from Python, anywhere
//   fe.attach(tap, 48000)                     # an audiopump.Tap, read in C
//   n = fe.readinto(buf)                      # whole 10 ms slices of 40 int8
//   n = fe.readinto(buf, voice)               # and a byte each: 1 for speech
//   fe.speaking()                             # the newest slice holds speech
//
// As with audiometer.Meter, the two ways in differ in where the work runs.
// feed() runs it in the caller. A tap is drained in C each time readinto()
// is called, so the work lands on the reader's thread and the pump pays
// nothing.
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "audiopump/audiopump_tap.h"
#include "cp_compat/audiodsp_build.h"
#include "shared/audiodsp_features.h"

#define AUDIOFEATURES_DEFAULT_SLICES 100

typedef struct {
    mp_obj_base_t base;
    audiodsp_features_t *f;
    mp_obj_t tap;               // MP_OBJ_NULL when no tap is attached
    uint32_t tap_cursor, tap_rate, tap_channels;
    uint32_t lapped;
} audiofeatures_frontend_obj_t;

static audiofeatures_frontend_obj_t *audiofeatures_get(mp_obj_t self_in) {
    audiofeatures_frontend_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->f == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("frontend is deinitialized"));
    }
    return self;
}

// Frontend(*, slices=100)
static mp_obj_t audiofeatures_frontend_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw,
    const mp_obj_t *all_args) {
    enum { ARG_slices };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_slices, MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = AUDIOFEATURES_DEFAULT_SLICES } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, a);
    mp_int_t slices = a[ARG_slices].u_int;
    if (slices < 8 || slices > 100000) {
        mp_raise_ValueError(MP_ERROR_TEXT("slices 8..100000"));
    }
    audiofeatures_frontend_obj_t *self = mp_obj_malloc_with_finaliser(audiofeatures_frontend_obj_t, type);
    self->f = NULL;
    self->tap = MP_OBJ_NULL;
    self->lapped = 0;
    self->f = audiodsp_features_new((uint32_t)slices);
    if (self->f == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("audiofeatures: no memory for the frontend"));
    }
    return MP_OBJ_FROM_PTR(self);
}

static void audiofeatures_check_rate(mp_int_t rate) {
    if (rate < AUDIODSP_FEATURES_MIN_RATE || rate > AUDIODSP_FEATURES_MAX_RATE) {
        mp_raise_ValueError(MP_ERROR_TEXT("sample_rate 16 kHz..384 kHz"));
    }
}

static void audiofeatures_run(audiofeatures_frontend_obj_t *self, const int16_t *pcm, size_t frames,
    uint32_t channels, uint32_t rate) {
    if (audiodsp_features_feed_s16(self->f, pcm, frames, channels, rate) != 0) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("audiofeatures: no memory for the resampler"));
    }
}

// feed(buffer, sample_rate, channel_count=1) -> slices waiting
static mp_obj_t audiofeatures_frontend_feed(size_t n_args, const mp_obj_t *args) {
    audiofeatures_frontend_obj_t *self = audiofeatures_get(args[0]);
    if (self->tap != MP_OBJ_NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("frontend is attached to a source; detach() first"));
    }
    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[1], &buf, MP_BUFFER_READ);
    mp_int_t rate = mp_obj_get_int(args[2]);
    mp_int_t ch = n_args > 3 ? mp_obj_get_int(args[3]) : 1;
    audiofeatures_check_rate(rate);
    if (ch < 1 || ch > 8 || buf.len % (2 * ch)) {
        mp_raise_ValueError(MP_ERROR_TEXT("whole s16 frames, 1..8 channels"));
    }
    audiofeatures_run(self, (const int16_t *)buf.buf, buf.len / (2 * ch), (uint32_t)ch, (uint32_t)rate);
    return MP_OBJ_NEW_SMALL_INT(audiodsp_features_waiting(self->f));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiofeatures_frontend_feed_obj, 3, 4, audiofeatures_frontend_feed);

// attach(tap, sample_rate)
static mp_obj_t audiofeatures_frontend_attach(mp_obj_t self_in, mp_obj_t src, mp_obj_t rate_in) {
    audiofeatures_frontend_obj_t *self = audiofeatures_get(self_in);
    self->tap = MP_OBJ_NULL;
    if (!mp_obj_is_type(src, &audiopump_tap_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("attach() takes an audiopump.Tap"));
    }
    mp_int_t rate = mp_obj_get_int(rate_in);
    audiofeatures_check_rate(rate);
    self->tap = src;
    self->tap_rate = (uint32_t)rate;
    self->tap_channels = audiopump_tap_frame_bytes(src) / 2;
    self->tap_cursor = audiopump_tap_position(src);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(audiofeatures_frontend_attach_obj, audiofeatures_frontend_attach);

static mp_obj_t audiofeatures_frontend_detach(mp_obj_t self_in) {
    audiofeatures_get(self_in)->tap = MP_OBJ_NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofeatures_frontend_detach_obj, audiofeatures_frontend_detach);

// Reads the tap until `want` slices wait or it has nothing new. A chunk is
// at most 1024 bytes, which at 16 kHz mono is 32 ms: a few slices at most, so
// the loop stops while the queue still has room for them and never drops one.
static void audiofeatures_drain(audiofeatures_frontend_obj_t *self, uint32_t want) {
    if (self->tap == MP_OBJ_NULL) {
        return;
    }
    uint8_t chunk[1024];
    while (audiodsp_features_waiting(self->f) < want && audiodsp_features_room(self->f) >= 5) {
        bool lapped;
        uint32_t got = audiopump_tap_read_since(self->tap, &self->tap_cursor, chunk, sizeof(chunk), &lapped);
        if (lapped) {
            self->lapped++;
        }
        if (got == 0) {
            break;
        }
        audiofeatures_run(self, (const int16_t *)(void *)chunk, got / (2 * self->tap_channels),
            self->tap_channels, self->tap_rate);
    }
}

static mp_obj_t audiofeatures_read(size_t n_args, const mp_obj_t *args, bool raw) {
    audiofeatures_frontend_obj_t *self = audiofeatures_get(args[0]);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[1], &buf, MP_BUFFER_WRITE);
    size_t width = raw ? 2 * AUDIODSP_FEATURES_SIZE : AUDIODSP_FEATURES_SIZE;
    size_t max = buf.len / width;
    uint8_t *voice = NULL;
    if (n_args > 2 && args[2] != mp_const_none) {
        mp_buffer_info_t vb;
        mp_get_buffer_raise(args[2], &vb, MP_BUFFER_WRITE);
        voice = (uint8_t *)vb.buf;
        if (vb.len < max) {
            max = vb.len;
        }
    }
    audiofeatures_drain(self, max > UINT32_MAX ? UINT32_MAX : (uint32_t)max);
    size_t n = raw ? audiodsp_features_read_u16(self->f, (uint16_t *)buf.buf, max, voice)
        : audiodsp_features_read_s8(self->f, (int8_t *)buf.buf, max, voice);
    return MP_OBJ_NEW_SMALL_INT(n);
}

// readinto(buf, voice=None) -> slices: 40 int8 each, ready for a model, and
// a byte each into voice, 1 when the slice holds speech
static mp_obj_t audiofeatures_frontend_readinto(size_t n_args, const mp_obj_t *args) {
    return audiofeatures_read(n_args, args, false);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiofeatures_frontend_readinto_obj, 2, 3, audiofeatures_frontend_readinto);

// readinto_raw(buf, voice=None) -> slices: 40 uint16 each, the frontend's own values
static mp_obj_t audiofeatures_frontend_readinto_raw(size_t n_args, const mp_obj_t *args) {
    return audiofeatures_read(n_args, args, true);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiofeatures_frontend_readinto_raw_obj, 2, 3, audiofeatures_frontend_readinto_raw);

// speaking() -> whether the newest slice made holds speech
static mp_obj_t audiofeatures_frontend_speaking(mp_obj_t self_in) {
    return mp_obj_new_bool(audiodsp_features_speaking(audiofeatures_get(self_in)->f));
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofeatures_frontend_speaking_obj, audiofeatures_frontend_speaking);

static mp_obj_t audiofeatures_frontend_reset(mp_obj_t self_in) {
    audiofeatures_frontend_obj_t *self = audiofeatures_get(self_in);
    audiodsp_features_reset(self->f);
    self->lapped = 0;
    if (self->tap != MP_OBJ_NULL) {
        self->tap_cursor = audiopump_tap_position(self->tap);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofeatures_frontend_reset_obj, audiofeatures_frontend_reset);

static mp_obj_t audiofeatures_frontend_stats(mp_obj_t self_in) {
    audiofeatures_frontend_obj_t *self = audiofeatures_get(self_in);
    audiodsp_features_stats_t st;
    audiodsp_features_stats(self->f, &st);
    mp_obj_t d = mp_obj_new_dict(8);
    #define PUT(k, v) mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(k), (v))
    PUT(MP_QSTR_slices, mp_obj_new_int_from_uint(st.slices));
    PUT(MP_QSTR_dropped, mp_obj_new_int_from_uint(st.dropped));
    PUT(MP_QSTR_waiting, mp_obj_new_int_from_uint(st.waiting));
    PUT(MP_QSTR_capacity, mp_obj_new_int_from_uint(st.capacity));
    PUT(MP_QSTR_rate, mp_obj_new_int_from_uint(st.rate));
    PUT(MP_QSTR_resampling, mp_obj_new_bool(st.resampling));
    PUT(MP_QSTR_lapped, mp_obj_new_int_from_uint(self->lapped));
    PUT(MP_QSTR_source, self->tap != MP_OBJ_NULL ? MP_OBJ_NEW_QSTR(MP_QSTR_tap) : mp_const_none);
    #undef PUT
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofeatures_frontend_stats_obj, audiofeatures_frontend_stats);

static mp_obj_t audiofeatures_frontend_deinit(mp_obj_t self_in) {
    audiofeatures_frontend_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->f != NULL) {
        self->tap = MP_OBJ_NULL;
        audiodsp_features_free(self->f);
        self->f = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofeatures_frontend_deinit_obj, audiofeatures_frontend_deinit);

static mp_obj_t audiofeatures_frontend_exit(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return audiofeatures_frontend_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiofeatures_frontend_exit_obj, 4, 4, audiofeatures_frontend_exit);

static mp_obj_t audiofeatures_frontend_enter(mp_obj_t self_in) {
    return self_in;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofeatures_frontend_enter_obj, audiofeatures_frontend_enter);

static const mp_rom_map_elem_t audiofeatures_frontend_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_feed), MP_ROM_PTR(&audiofeatures_frontend_feed_obj) },
    { MP_ROM_QSTR(MP_QSTR_attach), MP_ROM_PTR(&audiofeatures_frontend_attach_obj) },
    { MP_ROM_QSTR(MP_QSTR_detach), MP_ROM_PTR(&audiofeatures_frontend_detach_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&audiofeatures_frontend_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto_raw), MP_ROM_PTR(&audiofeatures_frontend_readinto_raw_obj) },
    { MP_ROM_QSTR(MP_QSTR_speaking), MP_ROM_PTR(&audiofeatures_frontend_speaking_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&audiofeatures_frontend_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&audiofeatures_frontend_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiofeatures_frontend_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&audiofeatures_frontend_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&audiofeatures_frontend_enter_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&audiofeatures_frontend_exit_obj) },
};
static MP_DEFINE_CONST_DICT(audiofeatures_frontend_locals, audiofeatures_frontend_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    audiofeatures_frontend_type,
    MP_QSTR_Frontend,
    MP_TYPE_FLAG_NONE,
    make_new, audiofeatures_frontend_make_new,
    locals_dict, &audiofeatures_frontend_locals
    );

// quantize(value): the int8 a model takes for one raw value
static mp_obj_t audiofeatures_quantize(mp_obj_t value) {
    mp_int_t v = mp_obj_get_int(value);
    if (v < 0 || v > 65535) {
        mp_raise_ValueError(MP_ERROR_TEXT("value 0..65535"));
    }
    return MP_OBJ_NEW_SMALL_INT(audiodsp_features_quantize((uint16_t)v));
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiofeatures_quantize_obj, audiofeatures_quantize);

static const mp_rom_map_elem_t audiofeatures_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiofeatures) },
    AUDIODSP_BUILD_GLOBALS,
    { MP_ROM_QSTR(MP_QSTR_Frontend), MP_ROM_PTR(&audiofeatures_frontend_type) },
    { MP_ROM_QSTR(MP_QSTR_FEATURE_SIZE), MP_ROM_INT(AUDIODSP_FEATURES_SIZE) },
    { MP_ROM_QSTR(MP_QSTR_SAMPLE_RATE), MP_ROM_INT(AUDIODSP_FEATURES_RATE) },
    { MP_ROM_QSTR(MP_QSTR_STEP_MS), MP_ROM_INT(AUDIODSP_FEATURES_STEP_MS) },
    { MP_ROM_QSTR(MP_QSTR_quantize), MP_ROM_PTR(&audiofeatures_quantize_obj) },
};
static MP_DEFINE_CONST_DICT(audiofeatures_module_globals, audiofeatures_module_globals_table);

const mp_obj_module_t audiofeatures_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audiofeatures_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audiofeatures, audiofeatures_module);
