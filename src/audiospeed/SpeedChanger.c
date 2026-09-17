// Ported from CircuitPython's shared-bindings+shared-module/audiospeed/
// SpeedChanger.{h,c} (upstream repo: https://github.com/adafruit/circuitpython,
// MIT). Deviation: m_malloc_without_collect -> m_malloc (no mainline
// equivalent, same fix as every other tier -- see docs/upstream-diff.md).
// Type registration adds `attr, cp_compat_attr` for the rate property (see
// docs/upstream-diff.md, "Property invocation needs an explicit attr slot").
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Tod Kurt
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include "audiospeed/SpeedChanger.h"

#include <string.h>

#include "cp_compat/argcheck.h"
#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/objproperty.h"

#include "py/runtime.h"

#define OUTPUT_BUFFER_FRAMES 128

// Convert a Python float to 16.16 fixed-point rate
static uint32_t rate_to_fp(mp_obj_t rate_obj) {
    // Upstream passes 0.001 here, but mp_arg_validate_obj_float_range's min/max
    // are mp_int_t (even in CircuitPython's own declaration), so it truncates
    // to 0 there too -- passing 0 directly is behavior-identical and avoids
    // this port's -Wfloat-conversion -Werror flagging the truncating literal.
    mp_float_t rate = mp_arg_validate_obj_float_range(rate_obj, 0, 1000, MP_QSTR_rate);
    // Round to the nearest Q16 step. Upstream truncates, so a float landing a
    // hair under its neighbour loses a whole LSB instead of arriving at it:
    // 1/1.0000000000000004 becomes 65535/65536 rather than unity. A deliberate
    // departure -- see docs/upstream-diff.md (audioif#92).
    return (uint32_t)(rate * (1 << SPEED_SHIFT) + (mp_float_t)0.5);
}

// Convert 16.16 fixed-point rate to Python float
static mp_obj_t fp_to_rate(uint32_t rate_fp) {
    return mp_obj_new_float((mp_float_t)rate_fp / (1 << 16));
}

void common_hal_audiospeed_speedchanger_construct(audiospeed_speedchanger_obj_t *self,
    mp_obj_t source, uint32_t rate_fp) {
    audiosample_base_t *src_base = audiosample_check(source);

    self->source = source;
    self->rate_fp = rate_fp;
    self->phase = 0;
    self->src_buffer = NULL;
    self->src_buffer_length = 0;
    self->src_sample_count = 0;
    self->source_done = false;
    self->source_exhausted = false;

    self->base.sample_rate = src_base->sample_rate;
    self->base.channel_count = src_base->channel_count;
    self->base.bits_per_sample = src_base->bits_per_sample;
    self->base.samples_signed = src_base->samples_signed;
    self->base.single_buffer = true;

    uint8_t bytes_per_frame = (src_base->bits_per_sample / 8) * src_base->channel_count;
    self->output_buffer_length = OUTPUT_BUFFER_FRAMES * bytes_per_frame;
    self->base.max_buffer_length = self->output_buffer_length;

    self->output_buffer = m_malloc(self->output_buffer_length);
}

void common_hal_audiospeed_speedchanger_deinit(audiospeed_speedchanger_obj_t *self) {
    self->output_buffer = NULL;
    self->source = MP_OBJ_NULL;
    audiosample_mark_deinit(&self->base);
}

void common_hal_audiospeed_speedchanger_set_rate(audiospeed_speedchanger_obj_t *self, uint32_t rate_fp) {
    self->rate_fp = rate_fp;
}

uint32_t common_hal_audiospeed_speedchanger_get_rate(audiospeed_speedchanger_obj_t *self) {
    return self->rate_fp;
}

// Fetch the next buffer from the source. Returns false if no data available.
static bool fetch_source_buffer(audiospeed_speedchanger_obj_t *self) {
    if (self->source_exhausted) {
        return false;
    }
    uint8_t *buf = NULL;
    uint32_t len = 0;
    audioio_get_buffer_result_t result = audiosample_get_buffer(self->source, false, 0, &buf, &len);
    if (result == GET_BUFFER_ERROR) {
        self->source_exhausted = true;
        return false;
    }
    if (len == 0) {
        self->source_exhausted = true;
        return false;
    }
    self->src_buffer = buf;
    self->src_buffer_length = len;
    uint8_t bytes_per_frame = (self->base.bits_per_sample / 8) * self->base.channel_count;
    self->src_sample_count = len / bytes_per_frame;
    self->source_done = (result == GET_BUFFER_DONE);
    self->phase = 0;
    return true;
}

void audiospeed_speedchanger_reset_buffer(audiospeed_speedchanger_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    if (single_channel_output && channel == 1) {
        return;
    }
    audiosample_reset_buffer(self->source, false, 0);
    self->phase = 0;
    self->src_buffer = NULL;
    self->src_buffer_length = 0;
    self->src_sample_count = 0;
    self->source_done = false;
    self->source_exhausted = false;
}

audioio_get_buffer_result_t audiospeed_speedchanger_get_buffer(audiospeed_speedchanger_obj_t *self,
    bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {

    if (self->src_buffer == NULL) {
        if (!fetch_source_buffer(self)) {
            *buffer = NULL;
            *buffer_length = 0;
            return GET_BUFFER_DONE;
        }
    }

    uint8_t bytes_per_sample = self->base.bits_per_sample / 8;
    uint8_t channels = self->base.channel_count;
    uint8_t bytes_per_frame = bytes_per_sample * channels;
    uint32_t out_frames = 0;
    uint32_t max_out_frames = self->output_buffer_length / bytes_per_frame;

    if (bytes_per_sample == 1) {
        uint8_t *out = self->output_buffer;
        while (out_frames < max_out_frames) {
            uint32_t src_index = self->phase >> SPEED_SHIFT;
            if (src_index >= self->src_sample_count) {
                if (self->source_done) {
                    self->source_exhausted = true;
                    break;
                }
                if (!fetch_source_buffer(self)) {
                    break;
                }
                src_index = 0;
            }
            uint8_t *src = self->src_buffer + src_index * bytes_per_frame;
            for (uint8_t c = 0; c < channels; c++) {
                *out++ = src[c];
            }
            out_frames++;
            self->phase += self->rate_fp;
        }
    } else {
        int16_t *out = (int16_t *)self->output_buffer;
        while (out_frames < max_out_frames) {
            uint32_t src_index = self->phase >> SPEED_SHIFT;
            if (src_index >= self->src_sample_count) {
                if (self->source_done) {
                    self->source_exhausted = true;
                    break;
                }
                if (!fetch_source_buffer(self)) {
                    break;
                }
                src_index = 0;
            }
            int16_t *src = (int16_t *)(self->src_buffer + src_index * bytes_per_frame);
            for (uint8_t c = 0; c < channels; c++) {
                *out++ = src[c];
            }
            out_frames++;
            self->phase += self->rate_fp;
        }
    }

    *buffer = self->output_buffer;
    *buffer_length = out_frames * bytes_per_frame;

    if (out_frames == 0) {
        return GET_BUFFER_DONE;
    }
    return self->source_exhausted ? GET_BUFFER_DONE : GET_BUFFER_MORE_DATA;
}

// --- Python bindings (from shared-bindings/audiospeed/SpeedChanger.c) ----

static mp_obj_t audiospeed_speedchanger_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_source, ARG_rate };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_source, MP_ARG_REQUIRED | MP_ARG_OBJ, {} },
        { MP_QSTR_rate, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_obj_t source = args[ARG_source].u_obj;
    audiosample_check(source);

    uint32_t rate_fp = 1 << 16; // default 1.0
    if (args[ARG_rate].u_obj != mp_const_none) {
        rate_fp = rate_to_fp(args[ARG_rate].u_obj);
    }

    audiospeed_speedchanger_obj_t *self = mp_obj_malloc(audiospeed_speedchanger_obj_t, &audiospeed_speedchanger_type);
    common_hal_audiospeed_speedchanger_construct(self, source, rate_fp);
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audiospeed_speedchanger_deinit(mp_obj_t self_in) {
    audiospeed_speedchanger_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiospeed_speedchanger_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiospeed_speedchanger_deinit_obj, audiospeed_speedchanger_deinit);

static mp_obj_t audiospeed_speedchanger_obj_get_rate(mp_obj_t self_in) {
    audiospeed_speedchanger_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return fp_to_rate(common_hal_audiospeed_speedchanger_get_rate(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiospeed_speedchanger_get_rate_obj, audiospeed_speedchanger_obj_get_rate);

static mp_obj_t audiospeed_speedchanger_obj_set_rate(mp_obj_t self_in, mp_obj_t rate_obj) {
    audiospeed_speedchanger_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    common_hal_audiospeed_speedchanger_set_rate(self, rate_to_fp(rate_obj));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiospeed_speedchanger_set_rate_obj, audiospeed_speedchanger_obj_set_rate);

MP_PROPERTY_GETSET(audiospeed_speedchanger_rate_obj,
    (mp_obj_t)&audiospeed_speedchanger_get_rate_obj,
    (mp_obj_t)&audiospeed_speedchanger_set_rate_obj);

static const mp_rom_map_elem_t audiospeed_speedchanger_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiospeed_speedchanger_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_rate), MP_ROM_PTR(&audiospeed_speedchanger_rate_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiospeed_speedchanger_locals_dict, audiospeed_speedchanger_locals_dict_table);

static const audiosample_p_t audiospeed_speedchanger_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audiospeed_speedchanger_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audiospeed_speedchanger_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiospeed_speedchanger_type,
    MP_QSTR_SpeedChanger,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiospeed_speedchanger_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiospeed_speedchanger_locals_dict,
    protocol, &audiospeed_speedchanger_proto
    );
