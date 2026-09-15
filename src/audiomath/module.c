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

#include "cp_compat/audioif_build.h"
#include "shared/audioif_remix.h"

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
        dest = mp_obj_new_bytearray(dst_len, NULL);
        mp_get_buffer_raise(dest, &dst, MP_BUFFER_WRITE);
    }
    audioif_remix_s16((int16_t *)dst.buf, (const int16_t *)src.buf,
        (uint32_t)frames, (uint32_t)src_ch, (uint32_t)dst_ch);
    return dest;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiomath_remix_s16_obj, 3, 4,
    audiomath_remix_s16);

static const mp_rom_map_elem_t audiomath_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiomath) },
    AUDIOIF_BUILD_GLOBALS,
    { MP_ROM_QSTR(MP_QSTR_Multiply), MP_ROM_PTR(&audiomath_multiply_type) },
    { MP_ROM_QSTR(MP_QSTR_SubOctave),
      MP_ROM_PTR(&audiomath_suboctave_type) },
    { MP_ROM_QSTR(MP_QSTR_remix_s16), MP_ROM_PTR(&audiomath_remix_s16_obj) },
};
static MP_DEFINE_CONST_DICT(audiomath_module_globals,
    audiomath_module_globals_table);

const mp_obj_module_t audiomath_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audiomath_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audiomath, audiomath_module);
