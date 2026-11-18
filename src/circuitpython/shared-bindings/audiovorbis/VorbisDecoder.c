// audiovorbis.VorbisDecoder for CircuitPython: the Python surface. The same
// table as audiodsp's src/audiovorbis/VorbisDecoder.c (test_binding_parity.py
// holds the two to it), and as CircuitPython's own
// shared-bindings/audiomp3/MP3Decoder.c (https://github.com/adafruit/circuitpython,
// MIT), which it is modelled on.
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2019 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <string.h>

#include "py/builtin.h"
#include "py/objproperty.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "shared/runtime/context_manager_helpers.h"

#include "shared-bindings/audiocore/__init__.h"
#include "shared-bindings/audiovorbis/VorbisDecoder.h"

//| class VorbisDecoder:
//|     """Load an Ogg Vorbis file for playback, shaped like `audiomp3.MP3Decoder`."""
//|

static mp_obj_t vorbis_check_stream(mp_obj_t stream) {
    if (mp_obj_is_str(stream)) {
        stream = mp_call_function_2(MP_OBJ_FROM_PTR(&mp_builtin_open_obj), stream, MP_ROM_QSTR(MP_QSTR_rb));
    }
    const mp_stream_p_t *stream_p = mp_get_stream_raise(stream, MP_STREAM_OP_READ);
    if (stream_p->is_text) {
        mp_raise_TypeError(MP_ERROR_TEXT("file must be a file opened in byte mode"));
    }
    return stream;
}

static mp_obj_t audiovorbis_vorbisdecoder_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 2, false);
    mp_obj_t stream = vorbis_check_stream(args[0]);

    uint8_t *buffer = NULL;
    size_t buffer_size = 0;
    if (n_args >= 2) {
        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise(args[1], &bufinfo, MP_BUFFER_WRITE);
        buffer = bufinfo.buf;
        buffer_size = bufinfo.len;
    }

    audiovorbis_vorbisdecoder_obj_t *self = mp_obj_malloc_with_finaliser(audiovorbis_vorbisdecoder_obj_t, &audiovorbis_vorbisdecoder_type);
    // mp_obj_malloc_with_finaliser doesn't zero what follows the base.
    memset((uint8_t *)self + sizeof(mp_obj_base_t), 0, sizeof(*self) - sizeof(mp_obj_base_t));
    common_hal_audiovorbis_vorbisdecoder_construct(self, stream, buffer, buffer_size);

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audiovorbis_vorbisdecoder_deinit(mp_obj_t self_in) {
    audiovorbis_vorbisdecoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiovorbis_vorbisdecoder_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiovorbis_vorbisdecoder_deinit_obj, audiovorbis_vorbisdecoder_deinit);

static void check_for_deinit(audiovorbis_vorbisdecoder_obj_t *self) {
    audiosample_check_for_deinit(&self->base);
}

static mp_obj_t audiovorbis_vorbisdecoder_obj_get_file(mp_obj_t self_in) {
    audiovorbis_vorbisdecoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return self->stream;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiovorbis_vorbisdecoder_get_file_obj, audiovorbis_vorbisdecoder_obj_get_file);

static mp_obj_t audiovorbis_vorbisdecoder_obj_set_file(mp_obj_t self_in, mp_obj_t stream) {
    audiovorbis_vorbisdecoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    const mp_stream_p_t *stream_p = mp_get_stream_raise(stream, MP_STREAM_OP_READ);
    if (stream_p->is_text) {
        mp_raise_TypeError(MP_ERROR_TEXT("file must be a file opened in byte mode"));
    }
    common_hal_audiovorbis_vorbisdecoder_set_file(self, stream);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiovorbis_vorbisdecoder_set_file_obj, audiovorbis_vorbisdecoder_obj_set_file);

static mp_obj_t audiovorbis_vorbisdecoder_obj_open(mp_obj_t self_in, mp_obj_t path) {
    audiovorbis_vorbisdecoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    mp_obj_t file = mp_call_function_2(MP_OBJ_FROM_PTR(&mp_builtin_open_obj), path, MP_ROM_QSTR(MP_QSTR_rb));
    common_hal_audiovorbis_vorbisdecoder_set_file(self, file);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiovorbis_vorbisdecoder_open_obj, audiovorbis_vorbisdecoder_obj_open);

MP_PROPERTY_GETSET(audiovorbis_vorbisdecoder_file_obj,
    (mp_obj_t)&audiovorbis_vorbisdecoder_get_file_obj,
    (mp_obj_t)&audiovorbis_vorbisdecoder_set_file_obj);

static mp_obj_t audiovorbis_vorbisdecoder_obj_get_rms_level(mp_obj_t self_in) {
    audiovorbis_vorbisdecoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_float(common_hal_audiovorbis_vorbisdecoder_get_rms_level(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiovorbis_vorbisdecoder_get_rms_level_obj, audiovorbis_vorbisdecoder_obj_get_rms_level);

MP_PROPERTY_GETTER(audiovorbis_vorbisdecoder_rms_level_obj,
    (mp_obj_t)&audiovorbis_vorbisdecoder_get_rms_level_obj);

static mp_obj_t audiovorbis_vorbisdecoder_obj_get_samples_decoded(mp_obj_t self_in) {
    audiovorbis_vorbisdecoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_int_from_uint(common_hal_audiovorbis_vorbisdecoder_get_samples_decoded(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiovorbis_vorbisdecoder_get_samples_decoded_obj, audiovorbis_vorbisdecoder_obj_get_samples_decoded);

MP_PROPERTY_GETTER(audiovorbis_vorbisdecoder_samples_decoded_obj,
    (mp_obj_t)&audiovorbis_vorbisdecoder_get_samples_decoded_obj);

static const mp_rom_map_elem_t audiovorbis_vorbisdecoder_locals_dict_table[] = {
    // Methods
    { MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&audiovorbis_vorbisdecoder_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiovorbis_vorbisdecoder_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&audiovorbis_vorbisdecoder_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },

    // Properties
    { MP_ROM_QSTR(MP_QSTR_file), MP_ROM_PTR(&audiovorbis_vorbisdecoder_file_obj) },
    { MP_ROM_QSTR(MP_QSTR_rms_level), MP_ROM_PTR(&audiovorbis_vorbisdecoder_rms_level_obj) },
    { MP_ROM_QSTR(MP_QSTR_samples_decoded), MP_ROM_PTR(&audiovorbis_vorbisdecoder_samples_decoded_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiovorbis_vorbisdecoder_locals_dict, audiovorbis_vorbisdecoder_locals_dict_table);

static const audiosample_p_t audiovorbis_vorbisdecoder_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audiovorbis_vorbisdecoder_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audiovorbis_vorbisdecoder_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiovorbis_vorbisdecoder_type,
    MP_QSTR_VorbisDecoder,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiovorbis_vorbisdecoder_make_new,
    locals_dict, &audiovorbis_vorbisdecoder_locals_dict,
    protocol, &audiovorbis_vorbisdecoder_proto
    );
