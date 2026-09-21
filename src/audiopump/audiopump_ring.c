// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// audiopump.Ring -- a lock-free SPSC ring that IS an audiosample.
//
// Python writes PCM in; the pump's thread pulls blocks out through the same
// protocol every other node speaks. That is the whole idea: the pump does
// not grow a second kind of source with its own rules, it grows one more
// node. A pushed stream can be the tail by itself, or sit behind a Mixer
// next to a synth with an Overdrive on the end of it.
//
// --- what makes it safe ---------------------------------------------------
//
// One producer (the interpreter), one consumer (the pump). `w` and `r` are
// 32-bit byte counters that wrap: the producer owns `w` and only ever reads
// `r`, the consumer owns `r` and only ever reads `w`, and `w - r` in
// unsigned 32-bit arithmetic is the level even across the wrap, because the
// capacity is a few kilobytes and never close to 2^31.
//
// 32-BIT ON PURPOSE. The pump's own output ring uses `volatile uint64_t`
// counters, which is a latent tear on every 32-bit target we ship: the P4 is
// RV32 and the S3 is Xtensa LX7, and a 64-bit store there is two 32-bit
// stores with a window between them. A 32-bit aligned store is atomic
// everywhere, so the protocol is 32-bit and the 64-bit numbers in `stats()`
// are statistics, each written by one thread and meant to be read after the
// pump has stopped.
//
// Acquire/release rather than bare `volatile`, because `volatile` orders the
// compiler and not the machine. The producer's release-store of `w` is what
// publishes the bytes it just memcpy'd; the consumer's acquire-load of `w`
// is what makes them visible.
//
// `write()` NEVER takes the pump lock. A memcpy the length of a block is not
// a swap and has no business stopping the audio for its duration -- that is
// the difference between a ring and a parked graph. `clear()` and `deinit()`
// do take it, because both write the consumer's own state.
//
// --- what it does when it cannot keep up ----------------------------------
//
// Underrun is a whole block of silence and a count, and the data that WAS in
// the ring stays there. Not a partial block topped up with zeros: the frames
// in the ring are whole frames of somebody's music, and playing half a
// block's worth early only moves the hole. Holding lets the ring refill
// toward its depth instead of living permanently at zero.
//
// Overrun is a short write and a count. `write()` takes what fits, in whole
// frames, and hands the remainder back to the caller as a byte count -- the
// same contract `audiodev.PCMOutput.try_write()` already has, which is why a
// `PCMOutput` can sit on top of this later with a `_write` that is one line.
//
// Neither is a raise and neither blocks. A pull may not raise (a longjmp off
// the pump thread dies in `gc_alloc` building the exception), and a write
// that blocked would put the audio's timing on the interpreter's schedule.

#include <stdint.h>
#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "audiocore/__init__.h"
#include "shared/audiodsp_pump_lock.h"

#include "audiopump/audiopump_ring.h"

#define AUDIOPUMP_RING_FNV_OFFSET (0xcbf29ce484222325ULL)
#define AUDIOPUMP_RING_FNV_PRIME  (0x100000001b3ULL)

// One place, so the fallback is visible rather than silently different.
#if defined(__GNUC__) || defined(__clang__)
#define AUDIOPUMP_RING_LOAD_ACQ(p)      __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define AUDIOPUMP_RING_STORE_REL(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#else
#define AUDIOPUMP_RING_LOAD_ACQ(p)      (*(volatile uint32_t *)(p))
#define AUDIOPUMP_RING_STORE_REL(p, v)  (*(volatile uint32_t *)(p) = (v))
#endif

typedef struct {
    audiosample_base_t base;

    uint8_t *ring;          // cap bytes, a whole number of blocks
    uint32_t cap;           // capacity in bytes
    uint32_t block;         // bytes handed out per pull
    uint32_t frame;         // bytes per frame; cap and block are multiples

    // The protocol. 32-bit, wrapping, one owner each.
    uint32_t w;             // producer writes, consumer reads
    uint32_t r;             // consumer writes, producer reads

    // Where in the buffer those indices point. Tracked, NOT recomputed as
    // `w % cap`: the indices wrap at 2^32 and a capacity that does not divide
    // 2^32 makes the position JUMP when they do -- the write lands somewhere
    // else in the buffer and every byte after it is wrong. It happens once per
    // 4 GB, about six hours of 48 kHz stereo, which is precisely the working
    // day of a streaming player and nothing a short test will ever reach.
    // `w - r` stays exact across the wrap because it is unsigned arithmetic;
    // only the positions have to be carried.
    uint32_t wpos;          // producer only
    uint32_t rpos;          // consumer only

    // Handed to the graph. Two of them so a node downstream may still be
    // holding the previous block when the next pull comes -- the same reason
    // Input ping-pongs, and the reason single_buffer is false.
    uint8_t *out[2];
    uint8_t which;

    // Statistics. Each is written by exactly one thread. 64-bit ones are
    // read from Python after the pump has stopped; the 32-bit ones are live.
    uint32_t underruns;     // consumer: pulls that found less than a block
    uint32_t overruns;      // producer: writes that could not take it all
    uint32_t pulls;         // consumer: blocks handed out, silence included
    uint64_t wrote;         // producer: bytes accepted
    uint64_t read;          // consumer: bytes handed out as real audio
    uint64_t starved;       // consumer: bytes handed out as silence
    uint64_t in_digest;     // producer: FNV-1a 64 over every byte accepted
    uint64_t out_digest;    // consumer: over every REAL byte handed out
} audiopump_ring_obj_t;

// --- the consumer side: this runs on the pump thread ----------------------
//
// Nothing in here touches the MicroPython runtime. No allocation, no raise,
// no lock of its own: the pump already holds audiodsp's lock for the whole
// block pull and the funnel re-takes it recursively on the way in.

static void audiopump_ring_reset_buffer(audiopump_ring_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    // Deliberately does NOT drop what is queued. `MixerVoice.play()` calls
    // this, and a ring somebody pre-filled before starting the pump would
    // lose its head start.
    self->which = 0;
}

static audioio_get_buffer_result_t audiopump_ring_get_buffer(
    audiopump_ring_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    // Interleaved only, like Input: the pump pulls (false, 0) and the
    // single-channel spacing walk is the caller's job in every node that
    // supports it.
    (void)single_channel_output;
    (void)channel;

    uint8_t *dst = self->out[self->which];
    self->which ^= 1;

    const uint32_t block = self->block;
    const uint32_t r = self->r;
    const uint32_t level = AUDIOPUMP_RING_LOAD_ACQ(&self->w) - r;

    if (level < block) {
        memset(dst, 0, block);
        self->underruns++;
        self->starved += block;
    } else {
        const uint32_t at = self->rpos;
        uint32_t first = self->cap - at;
        if (first > block) {
            first = block;
        }
        memcpy(dst, self->ring + at, first);
        if (block > first) {
            memcpy(dst + first, self->ring, block - first);
        }
        uint64_t digest = self->out_digest;
        for (uint32_t i = 0; i < block; i++) {
            digest ^= dst[i];
            digest *= AUDIOPUMP_RING_FNV_PRIME;
        }
        self->out_digest = digest;
        self->read += block;
        self->rpos = (at + block) % self->cap;
        // Release: the bytes are copied out before the space is given back,
        // so a producer that sees the room cannot land on top of them.
        AUDIOPUMP_RING_STORE_REL(&self->r, r + block);
    }

    self->pulls++;
    *buffer = dst;
    *buffer_length = block;
    return GET_BUFFER_MORE_DATA;
}

// --- the producer side: this runs on the interpreter thread ---------------

static mp_obj_t audiopump_ring_write(mp_obj_t self_in, mp_obj_t buf_in) {
    audiopump_ring_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);

    mp_buffer_info_t info;
    mp_get_buffer_raise(buf_in, &info, MP_BUFFER_READ);

    const uint32_t w = self->w;
    const uint32_t room = self->cap - (w - AUDIOPUMP_RING_LOAD_ACQ(&self->r));
    uint32_t n = (uint32_t)info.len;
    if (n > room) {
        n = room;
        self->overruns++;
    }
    n -= n % self->frame;
    if (n == 0) {
        return MP_OBJ_NEW_SMALL_INT(0);
    }

    const uint8_t *src = info.buf;
    const uint32_t at = self->wpos;
    uint32_t first = self->cap - at;
    if (first > n) {
        first = n;
    }
    memcpy(self->ring + at, src, first);
    if (n > first) {
        memcpy(self->ring, src + first, n - first);
    }

    uint64_t digest = self->in_digest;
    for (uint32_t i = 0; i < n; i++) {
        digest ^= src[i];
        digest *= AUDIOPUMP_RING_FNV_PRIME;
    }
    self->in_digest = digest;
    self->wrote += n;
    self->wpos = (at + n) % self->cap;

    // Release, and last: everything above must be visible to the pump before
    // the bytes are claimed to be there.
    AUDIOPUMP_RING_STORE_REL(&self->w, w + n);
    return MP_OBJ_NEW_SMALL_INT(n);
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiopump_ring_write_obj,
    audiopump_ring_write);

static mp_obj_t audiopump_ring_space(mp_obj_t self_in) {
    audiopump_ring_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    const uint32_t room =
        self->cap - (self->w - AUDIOPUMP_RING_LOAD_ACQ(&self->r));
    return MP_OBJ_NEW_SMALL_INT(room - (room % self->frame));
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_ring_space_obj,
    audiopump_ring_space);

static mp_obj_t audiopump_ring_level(mp_obj_t self_in) {
    audiopump_ring_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return MP_OBJ_NEW_SMALL_INT(self->w - AUDIOPUMP_RING_LOAD_ACQ(&self->r));
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_ring_level_obj,
    audiopump_ring_level);

// (wrote, read, starved, underruns, overruns, pulls, in_digest, out_digest,
//  capacity, block_size)
//
// The 64-bit words are two 32-bit stores on RV32 and Xtensa, so they are
// honest only once the pump has stopped. The live questions -- how full is
// it, did it starve -- are answered by level(), space() and the two 32-bit
// counters.
static mp_obj_t audiopump_ring_stats(mp_obj_t self_in) {
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
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_ring_stats_obj,
    audiopump_ring_stats);

// Drops what is queued. This is the one call that writes the CONSUMER's
// index from the producer's thread, so it is the one that has to hold the
// lock -- inside it the pump is either before a pull or after one, never in
// the middle of copying a block out.
static mp_obj_t audiopump_ring_clear(mp_obj_t self_in) {
    audiopump_ring_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    audiodsp_pump_lock_acquire();
    self->r = self->w;
    self->rpos = self->wpos;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_ring_clear_obj,
    audiopump_ring_clear);

// Marks, under the lock, and keeps every pointer. The buffers are the
// collector's and there is nothing to free; what has to be atomic against a
// pull is the mark itself, so the funnel's guard and the pump's own tail
// check both see a whole deinit rather than half of one.
static mp_obj_t audiopump_ring_deinit(mp_obj_t self_in) {
    audiopump_ring_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_ring_deinit_obj,
    audiopump_ring_deinit);

static mp_obj_t audiopump_ring_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_rate, ARG_channels, ARG_frames, ARG_capacity };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_sample_rate,   MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = 48000 } },
        { MP_QSTR_channel_count, MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = 2 } },
        // The block the pump pulls, in frames -- the same meaning `frames`
        // has on Input and on every other source.
        { MP_QSTR_frames,        MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = 256 } },
        // How many of those blocks deep the ring is. Depth IS the latency
        // (the board half measured that), so it is a separate number from
        // the block size and the default is the shallowest that leaves the
        // producer a block of slack.
        { MP_QSTR_capacity,      MP_ARG_INT | MP_ARG_KW_ONLY, { .u_int = 4 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed),
        allowed, args);

    const uint32_t channels = (uint32_t)args[ARG_channels].u_int;
    const uint32_t frames = (uint32_t)args[ARG_frames].u_int;
    const uint32_t capacity = (uint32_t)args[ARG_capacity].u_int;
    // No 16-frame floor, unlike Input: Input's block is a DMA read and a
    // tiny one is a mistake, a ring's block is a memcpy and a tiny one is a
    // test case.
    if (channels < 1 || channels > 2 || frames < 1 || capacity < 2) {
        mp_raise_ValueError(
            MP_ERROR_TEXT("bad channel_count, frames or capacity"));
    }
    // 2^31 is where `w - r` stops being the level. The real limit is the
    // heap; this only has to be far below the arithmetic's.
    if ((uint64_t)frames * channels * 2ULL * capacity > (1ULL << 24)) {
        mp_raise_ValueError(MP_ERROR_TEXT("ring too large"));
    }

    audiopump_ring_obj_t *self = mp_obj_malloc(audiopump_ring_obj_t,
        (const mp_obj_type_t *)&audiopump_ring_type);
    self->frame = channels * 2;
    self->block = frames * self->frame;
    self->cap = self->block * capacity;
    self->ring = m_new(uint8_t, self->cap);
    self->out[0] = m_new(uint8_t, self->block);
    self->out[1] = m_new(uint8_t, self->block);
    memset(self->ring, 0, self->cap);
    memset(self->out[0], 0, self->block);
    memset(self->out[1], 0, self->block);
    self->which = 0;
    self->w = 0;
    self->r = 0;
    self->wpos = 0;
    self->rpos = 0;
    self->underruns = 0;
    self->overruns = 0;
    self->pulls = 0;
    self->wrote = 0;
    self->read = 0;
    self->starved = 0;
    self->in_digest = AUDIOPUMP_RING_FNV_OFFSET;
    self->out_digest = AUDIOPUMP_RING_FNV_OFFSET;

    self->base.sample_rate = (uint32_t)args[ARG_rate].u_int;
    self->base.max_buffer_length = self->block;
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)channels;
    self->base.samples_signed = true;
    self->base.single_buffer = false;
    return MP_OBJ_FROM_PTR(self);
}

static const mp_rom_map_elem_t audiopump_ring_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&audiopump_ring_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_space), MP_ROM_PTR(&audiopump_ring_space_obj) },
    { MP_ROM_QSTR(MP_QSTR_level), MP_ROM_PTR(&audiopump_ring_level_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&audiopump_ring_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&audiopump_ring_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiopump_ring_deinit_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiopump_ring_locals,
    audiopump_ring_locals_table);

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
    attr, cp_compat_attr,
    locals_dict, &audiopump_ring_locals,
    protocol, &audiopump_ring_proto
    );
