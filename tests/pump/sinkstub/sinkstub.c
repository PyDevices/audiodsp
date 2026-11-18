// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// `_pumpsink`: a driver whose sink takes at most `step` bytes a write, the
// way a board whose pump runs in service mode takes what fits in its DMA ring
// and returns. A probe arms it with a buffer, opens it with
// audiopump.spawn(..., sink="stub"), and reads back exactly what the sink was
// given, in order. With `lie`, the sink says it took the whole block while
// keeping only `step` bytes of it: a tail lost the way an engine that drops
// short writes loses it, so a probe can show that it notices.
//
// c_sink() hands out the same capture as a published C sink
// (audiopump/pcm_c_sink.h), the way a USB speaker's stream is handed to
// audiopump.spawn(sink=<bytes>): `step` bytes a write, nothing at all for the
// first `refuse` writes or for ever, and closed once it has kept `close_at`
// bytes.
//
// No thread: thread_start is NULL, so the pump stays in service mode and the
// probe drives it with audiopump.service().
//
// It also fills the hot-memory pair, with the C library's calloc and free and
// a count of each, so the MP3 decoder's buffers come through here on this
// binary and a probe can see them all come back (pump_probe.py, `hotmem`).

#include <stdlib.h>
#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "shared/audiodsp_port.h"
#include "audiopump/pcm_c_sink.h"

MP_REGISTER_ROOT_POINTER(mp_obj_t pumpsink_buffer);

static uint8_t *pumpsink_buf;
static size_t pumpsink_cap;
static size_t pumpsink_len;
static uint32_t pumpsink_step;
static uint32_t pumpsink_writes;
static bool pumpsink_lie;
static bool pumpsink_open;

static bool pumpsink_sink_open(const char *path) {
    (void)path;
    if (pumpsink_buf == NULL) {
        return false;
    }
    pumpsink_open = true;
    return true;
}

static void pumpsink_sink_close(void) {
    pumpsink_open = false;
}

static bool pumpsink_sink_ready(void) {
    return pumpsink_open;
}

static uint32_t pumpsink_sink_write(const uint8_t *buffer, uint32_t length,
    uint32_t timeout_ms, bool *timed_out) {
    (void)timeout_ms;
    (void)timed_out;
    pumpsink_writes++;
    uint32_t n = length < pumpsink_step ? length : pumpsink_step;
    if (n > pumpsink_cap - pumpsink_len) {
        n = (uint32_t)(pumpsink_cap - pumpsink_len);
    }
    memcpy(pumpsink_buf + pumpsink_len, buffer, n);
    pumpsink_len += n;
    return pumpsink_lie ? length : n;
}

static uint32_t pumpsink_hot_allocs;
static uint32_t pumpsink_hot_frees;

static void *pumpsink_mem_alloc_hot(size_t size) {
    void *ptr = calloc(1, size);
    if (ptr != NULL) {
        __atomic_add_fetch(&pumpsink_hot_allocs, 1, __ATOMIC_RELAXED);
    }
    return ptr;
}

static void pumpsink_mem_free_hot(void *ptr) {
    if (ptr != NULL) {
        __atomic_add_fetch(&pumpsink_hot_frees, 1, __ATOMIC_RELAXED);
    }
    free(ptr);
}

static const audiodsp_port_ops_t pumpsink_ops = {
    .name = "sinkstub",
    .sink_open = pumpsink_sink_open,
    .sink_close = pumpsink_sink_close,
    .sink_ready = pumpsink_sink_ready,
    .sink_write = pumpsink_sink_write,
    .mem_alloc_hot = pumpsink_mem_alloc_hot,
    .mem_free_hot = pumpsink_mem_free_hot,
    // A graph's memory through the same counted pair: hot() counts both.
    .mem_alloc_graph = pumpsink_mem_alloc_hot,
    .mem_free_graph = pumpsink_mem_free_hot,
};

const audiodsp_port_ops_t *audiodsp_port_driver(void) {
    return &pumpsink_ops;
}

// arm(buffer, step, lie=False): capture into `buffer`, `step` bytes a write.
static mp_obj_t pumpsink_arm(size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t info;
    mp_get_buffer_raise(args[0], &info, MP_BUFFER_WRITE);
    MP_STATE_VM(pumpsink_buffer) = args[0];
    pumpsink_buf = info.buf;
    pumpsink_cap = info.len;
    pumpsink_len = 0;
    pumpsink_writes = 0;
    pumpsink_step = (uint32_t)mp_obj_get_int(args[1]);
    pumpsink_lie = n_args > 2 && mp_obj_is_true(args[2]);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pumpsink_arm_obj, 2, 3, pumpsink_arm);

// taken(): (bytes kept, writes made).
static mp_obj_t pumpsink_taken(void) {
    mp_obj_t items[2] = {
        mp_obj_new_int_from_uint(pumpsink_len),
        mp_obj_new_int_from_uint(pumpsink_writes),
    };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(pumpsink_taken_obj, pumpsink_taken);

// hot(): (hot blocks handed out, hot blocks given back).
static mp_obj_t pumpsink_hot(void) {
    mp_obj_t items[2] = {
        mp_obj_new_int_from_uint(__atomic_load_n(&pumpsink_hot_allocs, __ATOMIC_RELAXED)),
        mp_obj_new_int_from_uint(__atomic_load_n(&pumpsink_hot_frees, __ATOMIC_RELAXED)),
    };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(pumpsink_hot_obj, pumpsink_hot);

static uint32_t pumpsink_c_refuse;
static bool pumpsink_c_never;
static bool pumpsink_c_closed;
static size_t pumpsink_c_close_at;

static int pumpsink_c_write(void *ctx, const uint8_t *data, size_t len) {
    (void)ctx;
    pumpsink_writes++;
    if (pumpsink_c_closed) {
        return -1;
    }
    if (pumpsink_len >= pumpsink_c_close_at) {
        pumpsink_c_closed = true;
        return -1;
    }
    if (pumpsink_c_never) {
        return 0;
    }
    if (pumpsink_c_refuse) {
        pumpsink_c_refuse--;
        return 0;
    }
    size_t n = len < pumpsink_step ? len : pumpsink_step;
    if (n > pumpsink_cap - pumpsink_len) {
        n = pumpsink_cap - pumpsink_len;
    }
    memcpy(pumpsink_buf + pumpsink_len, data, n);
    pumpsink_len += n;
    return pumpsink_lie ? (int)len : (int)n;
}

static size_t pumpsink_c_space_cut;

// space_cut(n): report n bytes less room than there is.
static mp_obj_t pumpsink_space_cut(mp_obj_t n) {
    pumpsink_c_space_cut = (size_t)mp_obj_get_int(n);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(pumpsink_space_cut_obj, pumpsink_space_cut);

static int pumpsink_c_space(void *ctx) {
    (void)ctx;
    if (pumpsink_c_closed) {
        return -1;
    }
    // A probe may shrink what it reports, to stand for a ring partly full.
    const size_t left = pumpsink_cap - pumpsink_len;
    return pumpsink_c_never ? 0 : (int)(left > pumpsink_c_space_cut
        ? left - pumpsink_c_space_cut : 0);
}

// c_sink(rate, channels, refuse=0, close_at=-1, never=False): the armed
// capture as a pcm_c_sink_t, in bytes.
static mp_obj_t pumpsink_c_sink(size_t n_args, const mp_obj_t *args) {
    if (pumpsink_buf == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("arm() first"));
    }
    const uint32_t channels = (uint32_t)mp_obj_get_int(args[1]);
    pumpsink_c_refuse = n_args > 2 ? (uint32_t)mp_obj_get_int(args[2]) : 0;
    const mp_int_t close_at = n_args > 3 ? mp_obj_get_int(args[3]) : -1;
    pumpsink_c_close_at = close_at < 0 ? (size_t)-1 : (size_t)close_at;
    pumpsink_c_never = n_args > 4 && mp_obj_is_true(args[4]);
    pumpsink_c_closed = false;
    pumpsink_c_space_cut = 0;
    const pcm_c_sink_t sink = {
        .magic = PCM_C_SINK_MAGIC,
        .version = PCM_C_SINK_VERSION,
        .ctx = NULL,
        .write = pumpsink_c_write,
        .space = pumpsink_c_space,
        .rate = (uint32_t)mp_obj_get_int(args[0]),
        .channels = channels,
        .bits = 16,
        .frame_bytes = channels * 2u,
    };
    return mp_obj_new_bytes((const byte *)&sink, sizeof(sink));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pumpsink_c_sink_obj, 2, 5, pumpsink_c_sink);

static const mp_rom_map_elem_t pumpsink_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__pumpsink) },
    { MP_ROM_QSTR(MP_QSTR_arm), MP_ROM_PTR(&pumpsink_arm_obj) },
    { MP_ROM_QSTR(MP_QSTR_taken), MP_ROM_PTR(&pumpsink_taken_obj) },
    { MP_ROM_QSTR(MP_QSTR_hot), MP_ROM_PTR(&pumpsink_hot_obj) },
    { MP_ROM_QSTR(MP_QSTR_c_sink), MP_ROM_PTR(&pumpsink_c_sink_obj) },
    { MP_ROM_QSTR(MP_QSTR_space_cut), MP_ROM_PTR(&pumpsink_space_cut_obj) },
};
static MP_DEFINE_CONST_DICT(pumpsink_globals, pumpsink_globals_table);

const mp_obj_module_t pumpsink_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&pumpsink_globals,
};

MP_REGISTER_MODULE(MP_QSTR__pumpsink, pumpsink_module);
