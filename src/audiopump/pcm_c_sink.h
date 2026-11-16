// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// audiodsp's copy: the pump is a consumer (audiopump.spawn(sink=...)). Keep it
// identical to the publisher's below the licence lines.
//
// pcm_c_sink_t: a PCM sink one native module publishes and another writes to
// from its own task, without the interpreter moving the bytes (usbif#43).
//
// This header is meant to be COPIED. A consumer (earful, say) vendors this
// file rather than including anything else of usbhost's, so the two modules
// share a struct layout and nothing more: no link-time dependency, no shared
// MicroPython type. Any publisher can fill the same struct; usbhost's UAC host
// output is the first, and audiodsp's audiopump.Ring could be the second.
//
// --- how a consumer gets one -----------------------------------------------
//
// The publisher's Python object has a c_sink() method returning `bytes`: the
// struct itself, by value. The consumer copies it into its own storage with
// pcm_c_sink_copy() below, which also checks it. Nothing
// in the struct points into the MicroPython heap, so the copy never dangles:
// `write` and `space` are functions in firmware, and `ctx` is whatever the
// publisher needs to recognise the stream the sink was issued for (usbhost
// passes a generation number, not a pointer).
//
//     mp_obj_t dest[2];
//     mp_load_method_maybe(pcm, MP_QSTR_c_sink, dest);
//     if (dest[0] == MP_OBJ_NULL) { ... no C sink: keep the Python pump ... }
//     mp_obj_t blob = mp_call_method_n_kw(0, 0, dest);       // pcm.c_sink()
//     mp_buffer_info_t info;
//     mp_get_buffer_raise(blob, &info, MP_BUFFER_READ);
//     if (!pcm_c_sink_copy(&self->sink, info.buf, info.len)) {
//         ... not a sink this code understands: keep the Python pump ...
//     }
//
// --- the contract every publisher keeps -------------------------------------
//
// 1. write() and space() may be called from any task or core, including one
//    the interpreter knows nothing about. Neither blocks, allocates, raises,
//    or touches the MicroPython runtime. Both are short: a bounded memcpy.
//
// 2. write() takes up to `len` bytes, in whole frames, and returns how many
//    it took. 0 is normal: the ring is full, or another producer holds it at
//    this instant; come back later. It never waits for room.
//
// 3. Once the publisher's stream is closed, write() and space() return -1,
//    for good. A sink stays closed even if the publisher later opens a new
//    stream: that stream issues a new sink, and the old copy keeps answering
//    -1. A negative return means "stop and forget this sink".
//
// 4. Closing never frees memory a write() is still inside. close() marks the
//    sink closed, then waits for every write() and space() already in
//    progress to return before the ring's storage goes away. A write racing
//    close() on another core therefore does one of two things: it finishes
//    against live storage and its bytes are discarded with the stream, or it
//    returns -1 without touching the ring. It never writes freed memory.
//
// 5. The caller's own obligations: pass whole frames of the published format
//    (`frame_bytes` each), and scale its own samples. Volume on the Python
//    side (audiodev's PCMOutput) is not applied to bytes written here.
//
// One producer at a time is the ring's design. A publisher may let two
// producers race without corrupting the ring (usbhost does: the loser gets 0),
// but interleaving two streams of audio is never what anyone wants.
#ifndef PCM_C_SINK_H
#define PCM_C_SINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// 'PCMS', as a little-endian word: the bytes read "PCMS" in a hex dump.
#define PCM_C_SINK_MAGIC   (0x534D4350u)
#define PCM_C_SINK_VERSION (1u)

typedef struct pcm_c_sink {
    uint32_t magic;         // PCM_C_SINK_MAGIC
    uint32_t version;       // PCM_C_SINK_VERSION; a consumer refuses newer majors
    void *ctx;              // the publisher's; pass it back unchanged
    // Bytes taken, >= 0, in whole frames; -1 once the stream is closed.
    int (*write)(void *ctx, const uint8_t *data, size_t len);
    // Bytes a write can take right now, in whole frames; -1 once closed.
    int (*space)(void *ctx);
    uint32_t rate;          // frames per second
    uint32_t channels;      // interleaved
    uint32_t bits;          // significant bits per sample
    uint32_t frame_bytes;   // bytes per frame on the wire: channels x container
} pcm_c_sink_t;

// Copy the struct out of `buf` into `out`, and say whether it is one this
// header understands. Checks size, magic, version and the two functions, so
// a consumer handed the wrong object gets false rather than a call through
// garbage. Copies first, with memcpy, so `buf` need not be aligned and may be
// collected as soon as this returns.
static inline bool pcm_c_sink_copy(pcm_c_sink_t *out, const void *buf, size_t len) {
    if (out == NULL || buf == NULL || len < sizeof(pcm_c_sink_t)) {
        return false;
    }
    memcpy(out, buf, sizeof(pcm_c_sink_t));
    return out->magic == PCM_C_SINK_MAGIC
        && out->version == PCM_C_SINK_VERSION
        && out->write != NULL
        && out->space != NULL
        && out->frame_bytes != 0;
}

#endif // PCM_C_SINK_H
