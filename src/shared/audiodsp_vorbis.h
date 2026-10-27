// Decode an Ogg Vorbis stream to 16-bit PCM, from C.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// audiovorbis.VorbisDecoder is built on this, on every target, and so can any
// other C module in the same firmware: hand it a stream through three
// callbacks and pull interleaved int16 frames out. The decoder underneath is
// Xiph's Tremor (lowmem branch, integer only, BSD-3-Clause), carried in
// third_party/tremor and compiled into audiodsp_vorbis.c with every external
// symbol renamed, so a module that links its own Tremor still links beside
// this one.
//
//     static long my_read(void *s, void *buf, size_t len) { ... }
//     static const audiodsp_vorbis_io_t io = { my_read, NULL, NULL };
//
//     int err;
//     audiodsp_vorbis_t *v = audiodsp_vorbis_open(&io, my_stream, &err);
//     if (v == NULL) { ... audiodsp_vorbis_strerror(err) ... }
//     int16_t pcm[1024 * 2];
//     long n;
//     while ((n = audiodsp_vorbis_read(v, pcm, 1024)) > 0) {
//         ... n frames, audiodsp_vorbis_channels(v) samples each ...
//     }
//     audiodsp_vorbis_close(v);
//
// Threads: a decoder belongs to one thread at a time; different decoders may
// run on different threads at once. Nothing here touches an interpreter.
//
// Memory: the decoder allocates through the bound driver's memory hooks
// (shared/audiodsp_port.h), which put it in PSRAM on a board that has it,
// and through the C library's malloc everywhere else. Nothing comes from a
// MicroPython or CPython heap. Header parsing and decoding also take stack: see
// docs/reference/vorbis.md for what a stream needs of both.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Copy up to `len` bytes of the stream into `buf`. Return how many, 0 at
    // the end of the stream, or -1 when the read failed.
    long (*read)(void *stream, void *buf, size_t len);

    // Move to `offset` relative to SEEK_SET, SEEK_CUR or SEEK_END (<stdio.h>)
    // and return 0, or return -1 when the stream can't. NULL means it never
    // can (a socket, a pipe): the decoder then plays it once, front to back,
    // and neither rewinds nor knows its length.
    int (*seek)(void *stream, int64_t offset, int whence);

    // Where the stream is, in bytes from its start, or -1. Used with seek;
    // NULL is allowed only when seek is.
    long (*tell)(void *stream);
} audiodsp_vorbis_io_t;

typedef struct audiodsp_vorbis audiodsp_vorbis_t;

// Errors. Every function that can fail returns one of these, negative.
#define AUDIODSP_VORBIS_EREAD       (-1)  // the stream's read callback failed
#define AUDIODSP_VORBIS_ENOTVORBIS  (-2)  // not Ogg, or Ogg but not Vorbis
#define AUDIODSP_VORBIS_EBADHEADER  (-3)  // Vorbis, but its headers are damaged
#define AUDIODSP_VORBIS_EVERSION    (-4)  // a Vorbis version this decoder can't read
#define AUDIODSP_VORBIS_ENOMEM      (-5)  // an allocation failed
#define AUDIODSP_VORBIS_ENOSEEK     (-6)  // a seek on a stream that can't
#define AUDIODSP_VORBIS_EINVAL      (-7)  // a bad argument, or a closed decoder
#define AUDIODSP_VORBIS_EFAULT      (-8)  // the decoder's state is corrupt

// Open a stream and read its headers. Returns NULL and sets *error (when
// error isn't NULL) if it isn't a stream this can play. `io` must outlive the
// decoder; `stream` is handed back to the callbacks and never freed here.
audiodsp_vorbis_t *audiodsp_vorbis_open(const audiodsp_vorbis_io_t *io,
    void *stream, int *error);

// Free everything the decoder holds. NULL is fine. The stream is the
// caller's: it is neither read nor closed here.
void audiodsp_vorbis_close(audiodsp_vorbis_t *v);

int audiodsp_vorbis_channels(const audiodsp_vorbis_t *v);
long audiodsp_vorbis_sample_rate(const audiodsp_vorbis_t *v);

// Frames in the whole stream, or -1 for a stream that can't seek.
int64_t audiodsp_vorbis_frames(audiodsp_vorbis_t *v);

// The frame the next read starts at.
int64_t audiodsp_vorbis_tell(audiodsp_vorbis_t *v);

// Decode up to `frames` frames into `pcm`, interleaved, channels() samples a
// frame, native-endian int16. Returns the frames written (fewer than asked
// is normal: it stops at a packet's end), 0 at the end of the stream, or an
// error. A gap in the stream (a lost page) is skipped, not reported.
//
// A chained stream whose later links change the rate or the channel count
// ends at the first such link: one decoder plays one format.
long audiodsp_vorbis_read(audiodsp_vorbis_t *v, int16_t *pcm, size_t frames);

// Move to `frame` (0 is the start). Needs a seekable stream.
int audiodsp_vorbis_seek(audiodsp_vorbis_t *v, int64_t frame);

// A short English sentence for an error code.
const char *audiodsp_vorbis_strerror(int error);

// What the decoder has allocated, in bytes, now and at most since the last
// reset. Only in a build with AUDIODSP_VORBIS_HEAP_STATS defined; elsewhere
// both read 0.
void audiodsp_vorbis_heap_stats(size_t *live, size_t *peak);
void audiodsp_vorbis_heap_stats_reset(void);

#ifdef __cplusplus
}
#endif
