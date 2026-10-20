// audiosampler.SampleBuffer. See SampleBuffer.h.
//
// A bytearray's bytes live in the collected heap, and every collection reads
// all of them looking for pointers: a 13 MB SoundFont arena made of
// bytearrays cost an ESP32-P4 1.2-1.6 s of stopped interpreter per
// gc.collect(), every 45 s or so while a piece played. These bytes come from
// the port's mem hooks instead (PSRAM where the driver says so), or malloc,
// and the collector only ever sees the small object that owns them.
//
// What that costs the caller: a memoryview of a SampleBuffer does not keep
// it alive, because the view's pointer is outside the heap and the collector
// cannot follow it. Whatever plays from the buffer must hold the buffer
// itself for as long as a voice reads it. deinit(), or the object being
// collected, frees the bytes at once.
//
// SPDX-License-Identifier: MIT

#include "audiosampler/SampleBuffer.h"

#include <stdlib.h>
#include <string.h>

#include "cp_compat/context_manager_helpers.h"
#include "py/runtime.h"
#include "shared/audiodsp_port.h"

static void *samplebuffer_alloc(size_t len, bool *hooked) {
    const audiodsp_port_ops_t *port = audiodsp_port();
    *hooked = port->mem_alloc != NULL && port->mem_free != NULL;
    if (*hooked) {
        return port->mem_alloc(len);
    }
    return malloc(len);
}

static void samplebuffer_release(audiosampler_samplebuffer_obj_t *self) {
    if (self->data != NULL) {
        if (self->hooked) {
            audiodsp_port()->mem_free(self->data);
        } else {
            free(self->data);
        }
    }
    self->data = NULL;
    self->len = 0;
}

static mp_obj_t audiosampler_samplebuffer_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    mp_int_t len = mp_obj_get_int(args[0]);
    if (len < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("size must not be negative"));
    }
    bool hooked = false;
    uint8_t *data = NULL;
    if (len > 0) {
        data = samplebuffer_alloc((size_t)len, &hooked);
        if (data == NULL) {
            mp_raise_msg_varg(&mp_type_MemoryError,
                MP_ERROR_TEXT("no %d bytes outside the heap"), (int)len);
        }
        memset(data, 0, (size_t)len);
    }
    // Made after the bytes, so a refused allocation leaves nothing to free;
    // and the object cannot fail once it holds them.
    audiosampler_samplebuffer_obj_t *self = mp_obj_malloc_with_finaliser(
        audiosampler_samplebuffer_obj_t, type);
    self->data = data;
    self->len = (size_t)len;
    self->hooked = hooked;
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audiosampler_samplebuffer_deinit(mp_obj_t self_in) {
    samplebuffer_release(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiosampler_samplebuffer_deinit_obj,
    audiosampler_samplebuffer_deinit);

static mp_obj_t audiosampler_samplebuffer_unary_op(mp_unary_op_t op,
    mp_obj_t self_in) {
    audiosampler_samplebuffer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    switch (op) {
        case MP_UNARY_OP_LEN:
            return MP_OBJ_NEW_SMALL_INT(self->len);
        case MP_UNARY_OP_BOOL:
            return mp_obj_new_bool(self->len != 0);
        default:
            return MP_OBJ_NULL;
    }
}

static mp_int_t audiosampler_samplebuffer_get_buffer(mp_obj_t self_in,
    mp_buffer_info_t *bufinfo, mp_uint_t flags) {
    (void)flags;
    audiosampler_samplebuffer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    // A deinitialised buffer is still a buffer, with nothing in it.
    bufinfo->buf = self->data;
    bufinfo->len = self->len;
    bufinfo->typecode = 'B';
    return 0;
}

static const mp_rom_map_elem_t audiosampler_samplebuffer_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit),
      MP_ROM_PTR(&audiosampler_samplebuffer_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__),
      MP_ROM_PTR(&audiosampler_samplebuffer_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
};
static MP_DEFINE_CONST_DICT(audiosampler_samplebuffer_locals,
    audiosampler_samplebuffer_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    audiosampler_samplebuffer_type,
    MP_QSTR_SampleBuffer,
    MP_TYPE_FLAG_NONE,
    make_new, audiosampler_samplebuffer_make_new,
    unary_op, audiosampler_samplebuffer_unary_op,
    buffer, audiosampler_samplebuffer_get_buffer,
    locals_dict, &audiosampler_samplebuffer_locals
    );
