// audiopump.Ring bindings for CircuitPython.
//
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices

#include <stdint.h>

#include "shared-bindings/audiopump/Ring.h"
#include "shared-bindings/audiocore/__init__.h"

#include "py/runtime.h"

//| class Ring:
//|     """A stream your code writes PCM into, played by CircuitPython's own
//|     audio outputs.
//|
//|     A Ring is an audio sample: hand it to `audiobusio.I2SOut.play()`,
//|     `audioio.AudioOut.play()` or an `audiomixer.MixerVoice`, and the output
//|     pulls blocks from it in C while your code keeps it filled. Signed
//|     16-bit, interleaved. A pull that finds less than a block plays a
//|     block of silence, keeps what it has, and counts an underrun.
//|
//|     The same class as MicroPython's ``audiopump.Ring``, so a producer
//|     written against one runs on the other::
//|
//|         import audiobusio, board
//|         from audiopump import Ring
//|
//|         ring = Ring(sample_rate=22050, channel_count=2, frames=256, capacity=8)
//|         ring.write(first_blocks)          # fill before play()
//|         out = audiobusio.I2SOut(board.I2S_BCLK, board.I2S_WCLK, board.I2S_DOUT)
//|         out.play(ring)
//|         while True:
//|             if ring.space() >= len(block):
//|                 ring.write(render(block))
//|
//|     ``play()`` pulls its first block at once, so write at least one block
//|     before it, or that first pull counts as an underrun."""
//|
//|     def __init__(
//|         self,
//|         *,
//|         sample_rate: int = 48000,
//|         channel_count: int = 2,
//|         frames: int = 256,
//|         capacity: int = 4,
//|     ) -> None:
//|         """Create a ring of ``capacity`` blocks of ``frames`` frames each.
//|
//|         ``frames`` is the block the output pulls; ``capacity`` is how many
//|         of them the ring holds, which is also its latency. ``channel_count``
//|         is 1 or 2, and ``capacity`` is at least 2."""
//|         ...
static mp_obj_t audiopump_ring_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_rate, ARG_channels, ARG_frames, ARG_capacity };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_sample_rate,   MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = 48000 } },
        { MP_QSTR_channel_count, MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = 2 } },
        { MP_QSTR_frames,        MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = 256 } },
        { MP_QSTR_capacity,      MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = 4 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed),
        allowed, args);

    const mp_int_t rate = args[ARG_rate].u_int;
    const mp_int_t channels = args[ARG_channels].u_int;
    const mp_int_t frames = args[ARG_frames].u_int;
    const mp_int_t capacity = args[ARG_capacity].u_int;
    if (rate < 1 || channels < 1 || channels > 2 || frames < 1 || capacity < 2) {
        mp_raise_ValueError(
            MP_ERROR_TEXT("bad channel_count, frames or capacity"));
    }
    // 2^31 is where `w - r` stops being the level; this only has to be far
    // below it.
    if ((uint64_t)frames * (uint64_t)channels * 2ULL * (uint64_t)capacity > (1ULL << 24)) {
        mp_raise_ValueError(MP_ERROR_TEXT("ring too large"));
    }

    audiopump_ring_obj_t *self = mp_obj_malloc(audiopump_ring_obj_t, &audiopump_ring_type);
    audiopump_ring_construct(self, (uint32_t)rate, (uint32_t)channels,
        (uint32_t)frames, (uint32_t)capacity);
    return MP_OBJ_FROM_PTR(self);
}

//|     def write(self, buf: circuitpython_typing.ReadableBuffer) -> int:
//|         """Copy in as many whole frames of ``buf`` as fit and return the
//|         number of bytes taken. Never blocks: a write that can't take it all
//|         is short, and counts an overrun."""
//|         ...
static mp_obj_t audiopump_ring_obj_write(mp_obj_t self_in, mp_obj_t buf_in) {
    audiopump_ring_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    mp_buffer_info_t info;
    mp_get_buffer_raise(buf_in, &info, MP_BUFFER_READ);
    return MP_OBJ_NEW_SMALL_INT(audiopump_ring_write(self, info.buf, (uint32_t)info.len));
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiopump_ring_write_obj, audiopump_ring_obj_write);

//|     def space(self) -> int:
//|         """Bytes a `write()` would take now, in whole frames."""
//|         ...
static mp_obj_t audiopump_ring_obj_space(mp_obj_t self_in) {
    audiopump_ring_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return MP_OBJ_NEW_SMALL_INT(audiopump_ring_space(self));
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_ring_space_obj, audiopump_ring_obj_space);

//|     def level(self) -> int:
//|         """Bytes queued and not yet pulled."""
//|         ...
static mp_obj_t audiopump_ring_obj_level(mp_obj_t self_in) {
    audiopump_ring_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return MP_OBJ_NEW_SMALL_INT(audiopump_ring_level(self));
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_ring_level_obj, audiopump_ring_obj_level);

//|     def stats(self) -> tuple[int, int, int, int, int, int, int, int, int, int]:
//|         """``(wrote, read, starved, underruns, overruns, pulls, in_digest,
//|         out_digest, capacity, block_size)``.
//|
//|         Byte counts, then the underrun, overrun and pull counts, then an
//|         FNV-1a 64 digest of every byte written and of every real byte
//|         pulled (silence is left out), then the ring's capacity and block in
//|         bytes. After the ring is drained, ``wrote == read`` and the two
//|         digests match exactly when the output played what was written, in
//|         order."""
//|         ...
static mp_obj_t audiopump_ring_obj_stats(mp_obj_t self_in) {
    audiopump_ring_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t items[10] = {
        mp_obj_new_int_from_ull(self->wrote),
        mp_obj_new_int_from_ull(self->read),
        mp_obj_new_int_from_ull(self->starved),
        mp_obj_new_int_from_uint(self->underruns),
        mp_obj_new_int_from_uint(self->overruns),
        mp_obj_new_int_from_uint(self->pulls),
        mp_obj_new_int_from_ull(self->in_digest),
        mp_obj_new_int_from_ull(self->out_digest),
        mp_obj_new_int_from_uint(self->cap),
        mp_obj_new_int_from_uint(self->block),
    };
    return mp_obj_new_tuple(10, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_ring_stats_obj, audiopump_ring_obj_stats);

//|     def clear(self) -> None:
//|         """Drop what is queued. The output stops hearing it from its next
//|         pull, and `space()` counts the room at once."""
//|         ...
static mp_obj_t audiopump_ring_obj_clear(mp_obj_t self_in) {
    audiopump_ring_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    audiopump_ring_clear(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_ring_clear_obj, audiopump_ring_obj_clear);

//|     def deinit(self) -> None:
//|         """Mark the ring deinitialised. An output still playing it stops at
//|         its next pull."""
//|         ...
//|
static mp_obj_t audiopump_ring_obj_deinit(mp_obj_t self_in) {
    audiopump_ring_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_ring_deinit_obj, audiopump_ring_obj_deinit);

static const mp_rom_map_elem_t audiopump_ring_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&audiopump_ring_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_space), MP_ROM_PTR(&audiopump_ring_space_obj) },
    { MP_ROM_QSTR(MP_QSTR_level), MP_ROM_PTR(&audiopump_ring_level_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&audiopump_ring_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&audiopump_ring_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiopump_ring_deinit_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiopump_ring_locals_dict, audiopump_ring_locals_dict_table);

static const audiosample_p_t audiopump_ring_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audiopump_ring_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audiopump_ring_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiopump_ring_type,
    MP_QSTR_Ring,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiopump_ring_make_new,
    locals_dict, &audiopump_ring_locals_dict,
    protocol, &audiopump_ring_proto
    );
