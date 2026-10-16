// audiometer: band levels for a spectrum meter, plus peak and RMS. audiodsp's
// own (media modules roadmap, Phase 6); the engine is shared/audiodsp_meter.c.
//
//   m = audiometer.Meter(bands=32, low_hz=35, high_hz=20000)
//   m.feed(pcm, 48000, channel_count=2)       # from Python, anywhere
//   m.attach(tap, 48000)                      # an audiopump.Tap, read in C
//   seq, levels, peak, rms = m.levels()       # one byte a band, half-dB steps
//
// The two ways in differ in where the work runs. feed() runs it in the
// caller. A tap is drained in C each time levels() is called, so the work
// lands on the reader's thread and the pump pays nothing. A tap meters any
// pump stream, whether it came from a file, I2S or USB.
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "audiopump/audiopump_tap.h"
#include "cp_compat/audiodsp_build.h"
#include "shared/audiodsp_hot.h"
#include "shared/audiodsp_meter.h"
#include "shared/audiodsp_port.h"

// --- Meter ------------------------------------------------------------------

typedef struct {
    mp_obj_base_t base;
    audiodsp_meter_t *m;
    mp_obj_t tap;               // MP_OBJ_NULL when no tap is attached
    uint32_t tap_cursor, tap_rate, tap_channels;
    uint32_t lapped;
} audiometer_meter_obj_t;

static audiometer_meter_obj_t *audiometer_get(mp_obj_t self_in) {
    audiometer_meter_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->m == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("meter is deinitialized"));
    }
    return self;
}

static void audiometer_configure_args(audiometer_meter_obj_t *self, mp_int_t bands, mp_obj_t lo, mp_obj_t hi) {
    float lo_hz = (float)mp_obj_get_float(lo), hi_hz = (float)mp_obj_get_float(hi);
    if (bands < 0 || bands > AUDIODSP_METER_MAX_BANDS || lo_hz <= 0 || hi_hz <= lo_hz) {
        mp_raise_ValueError(MP_ERROR_TEXT("bands 0..96, and 0 < low_hz < high_hz"));
    }
    audiodsp_meter_configure(self->m, (int)bands, lo_hz, hi_hz);
}

static void audiometer_detach_all(audiometer_meter_obj_t *self) {
    self->tap = MP_OBJ_NULL;
}

static mp_obj_t audiometer_meter_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw,
    const mp_obj_t *all_args) {
    enum { ARG_bands, ARG_low_hz, ARG_high_hz };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_bands, MP_ARG_INT, { .u_int = 32 } },
        { MP_QSTR_low_hz, MP_ARG_OBJ | MP_ARG_KW_ONLY, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_high_hz, MP_ARG_OBJ | MP_ARG_KW_ONLY, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, a);
    audiometer_meter_obj_t *self = mp_obj_malloc_with_finaliser(audiometer_meter_obj_t, type);
    self->m = NULL;
    self->tap = MP_OBJ_NULL;
    self->lapped = 0;
    self->m = audiodsp_meter_new();
    if (self->m == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("audiometer: no memory for the meter"));
    }
    audiometer_configure_args(self, a[ARG_bands].u_int,
        a[ARG_low_hz].u_obj == mp_const_none ? mp_obj_new_float(MICROPY_FLOAT_CONST(35.0)) : a[ARG_low_hz].u_obj,
        a[ARG_high_hz].u_obj == mp_const_none ? mp_obj_new_float(MICROPY_FLOAT_CONST(20000.0)) : a[ARG_high_hz].u_obj);
    return MP_OBJ_FROM_PTR(self);
}

// configure(bands, *, low_hz=35, high_hz=20000); bands=0 turns it off
static mp_obj_t audiometer_meter_configure(size_t n_args, const mp_obj_t *pos, mp_map_t *kw) {
    enum { ARG_self, ARG_bands, ARG_low_hz, ARG_high_hz };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_bands, MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_low_hz, MP_ARG_OBJ | MP_ARG_KW_ONLY, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_high_hz, MP_ARG_OBJ | MP_ARG_KW_ONLY, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    audiometer_meter_obj_t *self = audiometer_get(a[ARG_self].u_obj);
    audiometer_configure_args(self, a[ARG_bands].u_int,
        a[ARG_low_hz].u_obj == mp_const_none ? mp_obj_new_float(MICROPY_FLOAT_CONST(35.0)) : a[ARG_low_hz].u_obj,
        a[ARG_high_hz].u_obj == mp_const_none ? mp_obj_new_float(MICROPY_FLOAT_CONST(20000.0)) : a[ARG_high_hz].u_obj);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audiometer_meter_configure_obj, 2, audiometer_meter_configure);

// feed(buffer, sample_rate, channel_count=2): interleaved s16 frames
static mp_obj_t audiometer_meter_feed(size_t n_args, const mp_obj_t *args) {
    audiometer_meter_obj_t *self = audiometer_get(args[0]);
    if (self->tap != MP_OBJ_NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("meter is attached to a source; detach() first"));
    }
    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[1], &buf, MP_BUFFER_READ);
    mp_int_t rate = mp_obj_get_int(args[2]);
    mp_int_t ch = n_args > 3 ? mp_obj_get_int(args[3]) : 2;
    if (rate < 1000 || rate > 384000 || ch < 1 || ch > 8 || buf.len % (2 * ch)) {
        mp_raise_ValueError(MP_ERROR_TEXT("whole s16 frames, 1..8 channels, 1 kHz..384 kHz"));
    }
    audiodsp_meter_feed_s16(self->m, (const int16_t *)buf.buf, buf.len / (2 * ch), ch, rate);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiometer_meter_feed_obj, 3, 4, audiometer_meter_feed);

// attach(tap, sample_rate)
static mp_obj_t audiometer_meter_attach(size_t n_args, const mp_obj_t *args) {
    audiometer_meter_obj_t *self = audiometer_get(args[0]);
    audiometer_detach_all(self);
    mp_obj_t src = args[1];
    if (!mp_obj_is_type(src, &audiopump_tap_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("attach() takes an audiopump.Tap"));
    }
    if (n_args < 3) {
        mp_raise_TypeError(MP_ERROR_TEXT("a tap needs its sample_rate"));
    }
    mp_int_t rate = mp_obj_get_int(args[2]);
    if (rate < 1000 || rate > 384000) {
        mp_raise_ValueError(MP_ERROR_TEXT("sample_rate 1 kHz..384 kHz"));
    }
    self->tap = src;
    self->tap_rate = (uint32_t)rate;
    self->tap_channels = audiopump_tap_frame_bytes(src) / 2;
    self->tap_cursor = audiopump_tap_position(src);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiometer_meter_attach_obj, 2, 3, audiometer_meter_attach);

static mp_obj_t audiometer_meter_detach(mp_obj_t self_in) {
    audiometer_detach_all(audiometer_get(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiometer_meter_detach_obj, audiometer_meter_detach);

// Everything the tap has written since the last call, through the meter.
static void audiometer_drain(audiometer_meter_obj_t *self) {
    if (self->tap == MP_OBJ_NULL) {
        return;
    }
    uint8_t chunk[1024];
    for (;;) {
        bool lapped;
        uint32_t got = audiopump_tap_read_since(self->tap, &self->tap_cursor, chunk, sizeof(chunk), &lapped);
        if (lapped) {
            self->lapped++;
        }
        if (got == 0) {
            break;
        }
        audiodsp_meter_feed_s16(self->m, (const int16_t *)(void *)chunk,
            got / (2 * self->tap_channels), self->tap_channels, self->tap_rate);
    }
}

// levels(buf=None) -> (seq, levels, peak, rms)
static mp_obj_t audiometer_meter_levels(size_t n_args, const mp_obj_t *args) {
    audiometer_meter_obj_t *self = audiometer_get(args[0]);
    audiometer_drain(self);
    uint8_t tmp[AUDIODSP_METER_MAX_BANDS];
    uint8_t pk = 0, rms = 0;
    uint32_t n = 0;
    uint32_t seq = audiodsp_meter_read(self->m, tmp, sizeof(tmp), &pk, &rms, &n);
    mp_obj_t lv;
    if (n_args > 1 && args[1] != mp_const_none) {
        mp_buffer_info_t buf;
        mp_get_buffer_raise(args[1], &buf, MP_BUFFER_WRITE);
        memcpy(buf.buf, tmp, n < buf.len ? n : buf.len);
        lv = args[1];
    } else {
        lv = mp_obj_new_bytes(tmp, n);
    }
    mp_obj_t items[4] = {
        mp_obj_new_int_from_uint(seq),
        lv,
        MP_OBJ_NEW_SMALL_INT(pk),
        MP_OBJ_NEW_SMALL_INT(rms),
    };
    return mp_obj_new_tuple(4, items);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiometer_meter_levels_obj, 1, 2, audiometer_meter_levels);

static mp_obj_t audiometer_meter_stats(mp_obj_t self_in) {
    audiometer_meter_obj_t *self = audiometer_get(self_in);
    audiodsp_meter_stats_t st;
    audiodsp_meter_stats(self->m, &st);
    mp_obj_t d = mp_obj_new_dict(9);
    #define PUT(k, v) mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(k), (v))
    PUT(MP_QSTR_enabled, mp_obj_new_bool(st.enabled));
    PUT(MP_QSTR_bands, MP_OBJ_NEW_SMALL_INT(st.bands));
    PUT(MP_QSTR_analyses, mp_obj_new_int_from_uint(st.analyses));
    PUT(MP_QSTR_feed_us, mp_obj_new_int_from_ull(st.feed_us));
    PUT(MP_QSTR_analysis_us, mp_obj_new_int_from_ull(st.analysis_us));
    PUT(MP_QSTR_max_analysis_us, mp_obj_new_int_from_uint(st.max_analysis_us));
    PUT(MP_QSTR_elapsed_us, mp_obj_new_int_from_ull(st.elapsed_us));
    PUT(MP_QSTR_lapped, mp_obj_new_int_from_uint(self->lapped));
    PUT(MP_QSTR_source, self->tap != MP_OBJ_NULL ? MP_OBJ_NEW_QSTR(MP_QSTR_tap) : mp_const_none);
    #undef PUT
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiometer_meter_stats_obj, audiometer_meter_stats);

static mp_obj_t audiometer_meter_deinit(mp_obj_t self_in) {
    audiometer_meter_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->m != NULL) {
        audiometer_detach_all(self);
        audiodsp_meter_free(self->m);
        self->m = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiometer_meter_deinit_obj, audiometer_meter_deinit);

static mp_obj_t audiometer_meter_exit(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return audiometer_meter_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiometer_meter_exit_obj, 4, 4, audiometer_meter_exit);

static mp_obj_t audiometer_meter_enter(mp_obj_t self_in) {
    return self_in;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiometer_meter_enter_obj, audiometer_meter_enter);

static const mp_rom_map_elem_t audiometer_meter_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_configure), MP_ROM_PTR(&audiometer_meter_configure_obj) },
    { MP_ROM_QSTR(MP_QSTR_feed), MP_ROM_PTR(&audiometer_meter_feed_obj) },
    { MP_ROM_QSTR(MP_QSTR_attach), MP_ROM_PTR(&audiometer_meter_attach_obj) },
    { MP_ROM_QSTR(MP_QSTR_detach), MP_ROM_PTR(&audiometer_meter_detach_obj) },
    { MP_ROM_QSTR(MP_QSTR_levels), MP_ROM_PTR(&audiometer_meter_levels_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&audiometer_meter_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiometer_meter_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&audiometer_meter_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&audiometer_meter_enter_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&audiometer_meter_exit_obj) },
};
static MP_DEFINE_CONST_DICT(audiometer_meter_locals, audiometer_meter_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    audiometer_meter_type,
    MP_QSTR_Meter,
    MP_TYPE_FLAG_NONE,
    make_new, audiometer_meter_make_new,
    locals_dict, &audiometer_meter_locals
    );

// db_byte(power): the byte a power reads as (1.0 is a full-scale sine)
static mp_obj_t audiometer_db_byte(mp_obj_t power) {
    return MP_OBJ_NEW_SMALL_INT(audiodsp_meter_db_byte((float)mp_obj_get_float(power)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiometer_db_byte_obj, audiometer_db_byte);

static const mp_rom_map_elem_t audiometer_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiometer) },
    AUDIODSP_BUILD_GLOBALS,
    { MP_ROM_QSTR(MP_QSTR_Meter), MP_ROM_PTR(&audiometer_meter_type) },
    { MP_ROM_QSTR(MP_QSTR_MAX_BANDS), MP_ROM_INT(AUDIODSP_METER_MAX_BANDS) },
    { MP_ROM_QSTR(MP_QSTR_db_byte), MP_ROM_PTR(&audiometer_db_byte_obj) },
};
static MP_DEFINE_CONST_DICT(audiometer_module_globals, audiometer_module_globals_table);

const mp_obj_module_t audiometer_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audiometer_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audiometer, audiometer_module);
