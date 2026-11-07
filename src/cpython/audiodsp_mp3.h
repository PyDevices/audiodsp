// audiomp3's decoder for CPython, from C.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// The decoder is the Helix fixed-point MP3 decoder the board builds compile
// (Adafruit_MP3, fetched into .deps/mp3 by setup.py or scripts/fetch_deps.sh).
// What surrounds it -- the input buffer, ID3v2 skipping, sync-word search,
// the two output buffers and the rewind for a loop -- follows
// src/audiomp3/MP3Decoder.c step for step, so a file decodes to the same
// bytes here as on a board. That file is written against MicroPython's
// object and stream APIs; this one reads its stream through two callbacks.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    // Copy up to `len` bytes of the stream into `buf`. Return how many, 0 at
    // the end of the stream, AUDIODSP_MP3_EAGAIN when a non-blocking stream
    // has nothing yet, or AUDIODSP_MP3_EREAD when the read failed.
    long (*read)(void *stream, void *buf, size_t len);
    // Move by `offset` from SEEK_SET or SEEK_CUR and return the new position,
    // or -1 when the stream can't.
    int64_t (*seek)(void *stream, int64_t offset, int whence);
} audiodsp_mp3_io_t;

typedef struct audiodsp_mp3 audiodsp_mp3_t;

#define AUDIODSP_MP3_EREAD   (-1)  // the stream's read callback failed
#define AUDIODSP_MP3_ENOMEM  (-2)  // an allocation failed
#define AUDIODSP_MP3_EPARSE  (-3)  // no MP3 frame in the stream
#define AUDIODSP_MP3_EAGAIN  (-4)  // read callback only: nothing to read yet

// audiocore's get_buffer results.
#define AUDIODSP_MP3_DONE  0
#define AUDIODSP_MP3_MORE  1
#define AUDIODSP_MP3_ERROR 2

// Open a stream and read its first frame header. `inbuf_size` is the input
// buffer's size in bytes (MP3Decoder's default is 2048). Returns NULL and sets
// *error when it can't.
audiodsp_mp3_t *audiodsp_mp3_open(const audiodsp_mp3_io_t *io, void *stream,
    size_t inbuf_size, int *error);
void audiodsp_mp3_close(audiodsp_mp3_t *m);

int audiodsp_mp3_sample_rate(const audiodsp_mp3_t *m);
int audiodsp_mp3_channels(const audiodsp_mp3_t *m);
// The bytes every decoded buffer holds: one frame, every channel.
size_t audiodsp_mp3_buffer_bytes(const audiodsp_mp3_t *m);

// Decode the next frame, as MP3Decoder's get_buffer does. Returns
// AUDIODSP_MP3_DONE, _MORE or _ERROR with *pcm and *nbytes set, or
// AUDIODSP_MP3_EREAD when the stream's read failed. *pcm points into the
// decoder, and stays valid until the call after next.
int audiodsp_mp3_decode(audiodsp_mp3_t *m, const int16_t **pcm, size_t *nbytes);
// Start again from the top for a loop, as MP3Decoder's reset_buffer does:
// only once the stream has ended, and only if it seeks. Returns 0, or
// AUDIODSP_MP3_EREAD.
int audiodsp_mp3_rewind(audiodsp_mp3_t *m);

// rms_level and samples_decoded, as MP3Decoder reports them.
double audiodsp_mp3_rms_level(const audiodsp_mp3_t *m);
uint32_t audiodsp_mp3_samples_decoded(const audiodsp_mp3_t *m);
