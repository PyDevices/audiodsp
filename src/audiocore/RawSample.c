// Ported from CircuitPython's shared-bindings/audiocore/RawSample.c and
// shared-module/audiocore/RawSample.c (upstream repo:
// https://github.com/adafruit/circuitpython, MIT), merged into one file.
// Docstrings (`//|` lines) dropped -- this port has no docs.circuitpython.org
// build to feed.
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2024 Tim Chinowsky
// SPDX-FileCopyrightText: Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "audiocore/RawSample.h"
#include "audiocore/__init__.h"
#include "cp_compat/argcheck.h"
#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/objproperty.h"

#include "py/binary.h"
#include "py/runtime.h"
#include "shared/audiodsp_pump_lock.h"

// --- from shared-module/audiocore/RawSample.c ---------------------------

void common_hal_audioio_rawsample_construct(audioio_rawsample_obj_t *self,
    uint8_t *buffer,
    uint32_t len,
    uint8_t bytes_per_sample,
    bool samples_signed,
    uint8_t channel_count,
    uint32_t sample_rate,
    bool single_buffer) {

    audiodsp_rawsample_construct(&self->shared_state, &self->shared_info,
        buffer, len, bytes_per_sample, samples_signed, channel_count,
        sample_rate, single_buffer);
    self->base.bits_per_sample = self->shared_info.bits_per_sample;
    self->base.samples_signed = self->shared_info.samples_signed;
    self->base.max_buffer_length = self->shared_info.max_buffer_length;
    self->base.channel_count = self->shared_info.channel_count;
    self->base.sample_rate = self->shared_info.sample_rate;
    self->base.single_buffer = self->shared_info.single_buffer;
}

void common_hal_audioio_rawsample_deinit(audioio_rawsample_obj_t *self) {
    // The whole body. mark_deinit is not the damage; the pointer
    // nulling AFTER it is -- the funnel's guard has already let a
    // pull in by then, and the pull writes into a buffer that has
    // just become NULL. Detach under the lock, free afterwards.
    audiodsp_pump_lock_acquire();
    audiodsp_rawsample_deinit(&self->shared_state);
    audiosample_mark_deinit(&self->base);
    audiodsp_pump_lock_release();
}

void audioio_rawsample_reset_buffer(audioio_rawsample_obj_t *self,
    bool single_channel_output,
    uint8_t channel) {
    audiodsp_sample_source_t source = audiodsp_rawsample_source(&self->shared_state);
    (void)audiodsp_sample_reset(&source, single_channel_output, channel);
}

audioio_get_buffer_result_t audioio_rawsample_get_buffer(audioio_rawsample_obj_t *self,
    bool single_channel_output,
    uint8_t channel,
    uint8_t **buffer,
    uint32_t *buffer_length) {

    audiodsp_sample_source_t source = audiodsp_rawsample_source(&self->shared_state);
    const uint8_t *shared_buffer = NULL;
    audiodsp_buffer_result_t result = AUDIODSP_BUFFER_ERROR;
    audiodsp_status_t status = audiodsp_sample_get(&source, single_channel_output,
        channel, &shared_buffer, buffer_length, &result);
    if (status != AUDIODSP_STATUS_OK) {
        *buffer = NULL;
        *buffer_length = 0;
        return GET_BUFFER_ERROR;
    }
    *buffer = (uint8_t *)shared_buffer;
    return (audioio_get_buffer_result_t)result;
}

// --- from shared-bindings/audiocore/RawSample.c --------------------------

static mp_obj_t audioio_rawsample_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_buffer, ARG_channel_count, ARG_sample_rate, ARG_single_buffer };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_buffer, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL } },
        { MP_QSTR_channel_count, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1 } },
        { MP_QSTR_sample_rate, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 8000} },
        { MP_QSTR_single_buffer, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = true} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[ARG_buffer].u_obj, &bufinfo, MP_BUFFER_READ);
    uint8_t bytes_per_sample = 1;
    bool signed_samples = bufinfo.typecode == 'b' || bufinfo.typecode == 'h';
    if (bufinfo.typecode == 'h' || bufinfo.typecode == 'H') {
        bytes_per_sample = 2;
    } else if (bufinfo.typecode != 'b' && bufinfo.typecode != 'B' && bufinfo.typecode != BYTEARRAY_TYPECODE) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be a bytearray or array of type 'h', 'H', 'b', or 'B'"), MP_QSTR_buffer);
    }
    if (!args[ARG_single_buffer].u_bool && bufinfo.len % (bytes_per_sample * args[ARG_channel_count].u_int * 2) != 0) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("Length of %q must be an even multiple of channel_count * type_size"), MP_QSTR_buffer);
    }

    audioio_rawsample_obj_t *self = mp_obj_malloc(audioio_rawsample_obj_t, &audioio_rawsample_type);
    common_hal_audioio_rawsample_construct(self,
        ((uint8_t *)bufinfo.buf),
        bufinfo.len,
        bytes_per_sample,
        signed_samples,
        args[ARG_channel_count].u_int,
        args[ARG_sample_rate].u_int,
        args[ARG_single_buffer].u_bool);

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audioio_rawsample_deinit(mp_obj_t self_in) {
    audioio_rawsample_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audioio_rawsample_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioio_rawsample_deinit_obj, audioio_rawsample_deinit);

static const mp_rom_map_elem_t audioio_rawsample_locals_dict_table[] = {
    // Methods
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audioio_rawsample_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },

    // Properties
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audioio_rawsample_locals_dict, audioio_rawsample_locals_dict_table);

static const audiosample_p_t audioio_rawsample_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audioio_rawsample_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audioio_rawsample_get_buffer,
};

// Deviation from upstream: adds `attr, cp_compat_attr` -- see
// cp_compat/objproperty.c for why a ported type's MP_PROPERTY_GETTER/GETSET
// locals_dict entries need this to actually be invoked on this port.
MP_DEFINE_CONST_OBJ_TYPE(
    audioio_rawsample_type,
    MP_QSTR_RawSample,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audioio_rawsample_make_new,
    attr, cp_compat_attr,
    locals_dict, &audioio_rawsample_locals_dict,
    protocol, &audioio_rawsample_proto
    );
