// audiomath module table for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "py/binary.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "shared/audiodsp_cast.h"

#include "shared-bindings/audiomath/__init__.h"
#include "shared-bindings/audiomath/Multiply.h"
#include "shared-bindings/audiomath/SubOctave.h"

//| """Arithmetic on audio streams
//|
//| The `audiomath` module multiplies one audio stream by another, which is
//| ring modulation and, with a modulator that does not cross zero, amplitude
//| modulation. It is not part of CircuitPython upstream; it comes from
//| PyDevices' audiodsp.
//|
//| `synthio` can ring a note against an oscillator, but that reaches only
//| synthesized notes - not a microphone, a sample, or the output of another
//| effect - and an LFO-driven parameter updates once per block, far below the
//| hundreds of hertz a ring modulator wants.
//|
//| It also divides one stream down in frequency, with a comparator into a
//| chain of flip-flops - the analog octave divider, as in a Boss OC-2. That
//| is arithmetic on the signal against its own zero crossings, which is why
//| it lives here rather than beside the granular pitch shifter it does not
//| resemble.
//|
//| `cast_into` is not a node: it turns a float block into the int16 buffer a
//| `audiocore.RawSample` plays, without allocating.
//|
//| """

//| def cast_into(
//|     dst: WriteableBuffer, src: ReadableBuffer, scale: float = 32767.0
//| ) -> None:
//|     """Write ``src * scale`` into ``dst`` as int16, rounded half away from
//|     zero and clamped to -32768..32767; NaN becomes 0.
//|
//|     :param WriteableBuffer dst: int16 samples (or their bytes), as many as
//|         ``src`` has
//|     :param ReadableBuffer src: float32 or float64 samples
//|     :param float scale: what 1.0 becomes before rounding"""
//|     ...
//|
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
    { MP_ROM_QSTR(MP_QSTR_Multiply), MP_ROM_PTR(&audiomath_multiply_type) },
    { MP_ROM_QSTR(MP_QSTR_SubOctave),
      MP_ROM_PTR(&audiomath_suboctave_type) },
    { MP_ROM_QSTR(MP_QSTR_cast_into), MP_ROM_PTR(&audiomath_cast_into_obj) },
};

static MP_DEFINE_CONST_DICT(audiomath_module_globals,
    audiomath_module_globals_table);

const mp_obj_module_t audiomath_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audiomath_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_audiomath, audiomath_module);
