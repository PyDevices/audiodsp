// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// audiopump.Tap -- Python reads what went out, without being in the path.
//
// A meter, a scope, a level LED and a "did that actually make a sound" check
// all want the same thing: the last few milliseconds of the pump's output. A
// tap is a LOSSY ring the pump copies its block into after the tail has
// produced it. Lossy is the point: nothing the reader does can stall the
// audio, and a reader that falls behind loses old audio rather than new.
//
// The cost to the pull is one memcpy of the block -- 1 KB at 256 frames
// stereo, against a 5.3 ms budget -- and a NULL check when no tap exists.
//
// --- reading something that is being written -----------------------------
//
// The reader wants the most recent N bytes. It reads the write counter, works
// out where they are, copies them, and reads the counter again: if the writer
// has advanced far enough to have overwritten any of what was copied, the
// copy is torn and it tries once more. That is a seqlock over a byte range,
// and it is why `readinto` reports how much it got rather than assuming.
//
// The capacity is rounded UP TO A POWER OF TWO in bytes, so `w % cap` stays
// correct across the wrap of the 32-bit counter. A ring whose capacity does
// not divide 2^32 jumps its read position when the counter wraps -- at 48 kHz
// stereo that is once every 6.2 hours, which is exactly the kind of bug that
// is found by a customer and not by a test.

#include <stdint.h>
#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "audiopump/audiopump_tap.h"

#if defined(__GNUC__) || defined(__clang__)
#define AUDIOPUMP_TAP_LOAD_ACQ(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define AUDIOPUMP_TAP_STORE_REL(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#else
#define AUDIOPUMP_TAP_LOAD_ACQ(p)     (*(volatile uint32_t *)(p))
#define AUDIOPUMP_TAP_STORE_REL(p, v) (*(volatile uint32_t *)(p) = (v))
#endif

typedef struct {
    mp_obj_base_t base;
    uint8_t *buf;
    uint32_t cap;        // a power of two, in bytes
    uint32_t frame;      // bytes per frame, so a read lands on a boundary
    uint32_t w;          // wrapping byte counter; writer stores, reader loads
    uint32_t blocks;     // writer
    uint32_t reads;      // reader
    uint32_t torn;       // reader: the writer lapped it twice running
} audiopump_tap_obj_t;

void audiopump_tap_write(mp_obj_t tap, const uint8_t *buffer,
    uint32_t length) {
    audiopump_tap_obj_t *self = MP_OBJ_TO_PTR(tap);
    if (length == 0) {
        return;
    }
    if (length > self->cap) {
        // Only the tail of an over-long block can be the "most recent", so
        // keep that rather than wrap over ourselves.
        buffer += length - self->cap;
        length = self->cap;
    }
    const uint32_t w = self->w;          // the writer owns it; a plain read
    const uint32_t at = w & (self->cap - 1);
    uint32_t first = self->cap - at;
    if (first > length) {
        first = length;
    }
    memcpy(self->buf + at, buffer, first);
    if (length > first) {
        memcpy(self->buf, buffer + first, length - first);
    }
    self->blocks++;
    // Release, and last: the bytes are in place before they are claimed.
    AUDIOPUMP_TAP_STORE_REL(&self->w, w + length);
}

// The most recent whole frames that fit in `buf`. Returns bytes written.
static mp_obj_t audiopump_tap_readinto(mp_obj_t self_in, mp_obj_t buf_in) {
    audiopump_tap_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_buffer_info_t out;
    mp_get_buffer_raise(buf_in, &out, MP_BUFFER_WRITE);

    uint32_t want = (uint32_t)out.len;
    if (want > self->cap) {
        want = self->cap;
    }
    want -= want % self->frame;
    if (want == 0) {
        return MP_OBJ_NEW_SMALL_INT(0);
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        const uint32_t w = AUDIOPUMP_TAP_LOAD_ACQ(&self->w);
        if (w < want) {
            // Not enough has ever gone out. Give what there is, from 0.
            want = w - (w % self->frame);
            if (want == 0) {
                return MP_OBJ_NEW_SMALL_INT(0);
            }
        }
        const uint32_t start = w - want;
        const uint32_t at = start & (self->cap - 1);
        uint32_t first = self->cap - at;
        if (first > want) {
            first = want;
        }
        memcpy(out.buf, self->buf + at, first);
        if (want > first) {
            memcpy((uint8_t *)out.buf + first, self->buf, want - first);
        }
        // Did the writer get far enough ahead to have overwritten any of it?
        const uint32_t after = AUDIOPUMP_TAP_LOAD_ACQ(&self->w);
        if (after - start <= self->cap) {
            self->reads++;
            return mp_obj_new_int_from_uint(want);
        }
    }
    self->torn++;
    return MP_OBJ_NEW_SMALL_INT(0);
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiopump_tap_readinto_obj,
    audiopump_tap_readinto);

// (bytes_written, blocks, reads, torn, capacity, frame_size)
static mp_obj_t audiopump_tap_stats(mp_obj_t self_in) {
    audiopump_tap_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t items[6] = {
        mp_obj_new_int_from_uint(AUDIOPUMP_TAP_LOAD_ACQ(&self->w)),
        mp_obj_new_int_from_uint(self->blocks),
        mp_obj_new_int_from_uint(self->reads),
        mp_obj_new_int_from_uint(self->torn),
        mp_obj_new_int_from_uint(self->cap),
        mp_obj_new_int_from_uint(self->frame),
    };
    return mp_obj_new_tuple(6, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_tap_stats_obj, audiopump_tap_stats);

static mp_obj_t audiopump_tap_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_frames, ARG_channels };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_frames,        MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = 2048 } },
        { MP_QSTR_channel_count, MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = 2 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed),
        allowed, args);

    const uint32_t channels = (uint32_t)args[ARG_channels].u_int;
    const uint32_t frames = (uint32_t)args[ARG_frames].u_int;
    if (channels < 1 || channels > 2 || frames < 16) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad channel_count or frames"));
    }
    const uint32_t frame = channels * 2;
    uint32_t cap = 1;
    while (cap < frames * frame) {
        cap <<= 1;
    }
    if (cap > (1u << 22)) {
        mp_raise_ValueError(MP_ERROR_TEXT("tap too large"));
    }

    audiopump_tap_obj_t *self = mp_obj_malloc(audiopump_tap_obj_t,
        (const mp_obj_type_t *)&audiopump_tap_type);
    self->buf = m_new(uint8_t, cap);
    memset(self->buf, 0, cap);
    self->cap = cap;
    self->frame = frame;
    self->w = 0;
    self->blocks = 0;
    self->reads = 0;
    self->torn = 0;
    return MP_OBJ_FROM_PTR(self);
}

static const mp_rom_map_elem_t audiopump_tap_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&audiopump_tap_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&audiopump_tap_stats_obj) },
};
static MP_DEFINE_CONST_DICT(audiopump_tap_locals, audiopump_tap_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    audiopump_tap_type,
    MP_QSTR_Tap,
    MP_TYPE_FLAG_NONE,
    make_new, audiopump_tap_make_new,
    locals_dict, &audiopump_tap_locals
    );
