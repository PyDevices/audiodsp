// audiomath module table. New code (not a port) -- see audiomixer/module.c
// for why this port uses a single registration file per module.
//
// The name is the room: anything that is arithmetic on streams rather than an
// effect built out of them belongs here. Multiply is arithmetic on two
// streams; SubOctave is arithmetic on one, driven by its own zero crossings.
//
// SPDX-License-Identifier: MIT

#include "audiomath/Multiply.h"
#include "audiomath/SubOctave.h"

#include "cp_compat/audiodsp_build.h"
#include "shared/audiodsp_cast.h"
#include "shared/audiodsp_remix.h"

#include "py/binary.h"
#include "py/obj.h"
#include "py/runtime.h"

static mp_obj_t audiomath_remix_s16(size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t src;
    mp_get_buffer_raise(args[0], &src, MP_BUFFER_READ);
    mp_int_t src_ch = mp_obj_get_int(args[1]);
    mp_int_t dst_ch = mp_obj_get_int(args[2]);
    if (src_ch < 1 || src_ch > 2 || dst_ch < 1 || dst_ch > 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("channel_count must be 1 or 2"));
    }
    size_t src_frame = 2u * (size_t)src_ch;
    if (src_frame == 0 || src.len % src_frame) {
        mp_raise_ValueError(
            MP_ERROR_TEXT("source must be a whole number of frames"));
    }
    size_t frames = src.len / src_frame;
    size_t dst_len = frames * 2u * (size_t)dst_ch;
    mp_obj_t dest;
    mp_buffer_info_t dst;
    if (n_args == 4 && args[3] != mp_const_none) {
        dest = args[3];
        mp_get_buffer_raise(dest, &dst, MP_BUFFER_WRITE);
        if (dst.len < dst_len) {
            mp_raise_ValueError(MP_ERROR_TEXT("dest is too small"));
        }
    } else {
        // Not mp_obj_new_bytearray(dst_len, NULL): that constructor memcpy's
        // from the pointer it is given, so NULL faults the moment dst_len is
        // nonzero. Allocate first, then hand the buffer over by reference.
        uint8_t *buf = m_new(uint8_t, dst_len);
        dest = mp_obj_new_bytearray_by_ref(dst_len, buf);
        mp_get_buffer_raise(dest, &dst, MP_BUFFER_WRITE);
    }
    audiodsp_remix_s16((int16_t *)dst.buf, (const int16_t *)src.buf,
        (uint32_t)frames, (uint32_t)src_ch, (uint32_t)dst_ch);
    return dest;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiomath_remix_s16_obj, 3, 4,
    audiomath_remix_s16);

// cast_into(dst, src, scale=32767.0): a float block into an int16 buffer the
// caller owns, scaled, rounded half away from zero and saturated
// (shared/audiodsp_cast.h). Nothing is allocated, so a host that keeps its
// blocks in float can feed a RawSample every block without touching the heap.
static mp_obj_t audiomath_cast_into(size_t n_args, const mp_obj_t *pos_args,
    mp_map_t *kw_args) {
    enum { ARG_dst, ARG_src, ARG_scale };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_dst, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_src, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_scale, MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args),
        allowed_args, args);
    mp_buffer_info_t dst;
    mp_get_buffer_raise(args[ARG_dst].u_obj, &dst, MP_BUFFER_WRITE);
    if (dst.typecode != 'h' && dst.typecode != 'b' && dst.typecode != 'B'
        && dst.typecode != BYTEARRAY_TYPECODE) {
        mp_raise_TypeError(MP_ERROR_TEXT("dst must be int16"));
    }
    mp_buffer_info_t src;
    mp_get_buffer_raise(args[ARG_src].u_obj, &src, MP_BUFFER_READ);
    size_t width;
    if (src.typecode == 'f') {
        width = sizeof(float);
    } else if (src.typecode == 'd') {
        width = sizeof(double);
    } else {
        mp_raise_TypeError(MP_ERROR_TEXT("src must be float32 or float64"));
    }
    size_t count = src.len / width;
    if (dst.len != count * sizeof(int16_t)) {
        mp_raise_ValueError(
            MP_ERROR_TEXT("dst and src must have the same length"));
    }
    mp_float_t scale = args[ARG_scale].u_obj == MP_OBJ_NULL
        ? (mp_float_t)32767.0
        : mp_obj_get_float(args[ARG_scale].u_obj);
    if (width == sizeof(float)) {
        audiodsp_cast_s16_f32((int16_t *)dst.buf, (const float *)src.buf,
            count, (float)scale);
    } else {
        audiodsp_cast_s16_f64((int16_t *)dst.buf, (const double *)src.buf,
            count, (double)scale);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audiomath_cast_into_obj, 2,
    audiomath_cast_into);

static const mp_rom_map_elem_t audiomath_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiomath) },
    AUDIODSP_BUILD_GLOBALS,
    { MP_ROM_QSTR(MP_QSTR_Multiply), MP_ROM_PTR(&audiomath_multiply_type) },
    { MP_ROM_QSTR(MP_QSTR_SubOctave),
      MP_ROM_PTR(&audiomath_suboctave_type) },
    { MP_ROM_QSTR(MP_QSTR_remix_s16), MP_ROM_PTR(&audiomath_remix_s16_obj) },
    { MP_ROM_QSTR(MP_QSTR_cast_into), MP_ROM_PTR(&audiomath_cast_into_obj) },
};
static MP_DEFINE_CONST_DICT(audiomath_module_globals,
    audiomath_module_globals_table);

const mp_obj_module_t audiomath_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audiomath_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audiomath, audiomath_module);
