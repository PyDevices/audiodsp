// audiopump.Ring for CircuitPython: the stream sample.
//
// A ring Python writes PCM into that is also an audiosample, so
// CircuitPython's own outputs (audiobusio.I2SOut, audioio.AudioOut,
// audiopwmio.PWMAudioOut) and its audiomixer.Mixer pull it in C, with no
// Python in the pull path. It is the same node as audiodsp's MicroPython
// `audiopump.Ring` (src/audiopump/audiopump_ring.c), with the same Python
// surface, so one producer runs on both. On MicroPython the pump pulls it;
// here the board's audio output is the pump.
//
// One producer (the interpreter), one consumer (the output's fill, which
// runs from a background callback on most ports and from an interrupt on a
// few, stm's PWMAudioOut among them). `w` and `r` are 32-bit wrapping byte
// counters, each written by one side only, and `w - r` is the level even
// across the wrap. `clear()` is the one producer call that would have to
// write the consumer's state, so it does not: it posts a request the next
// pull carries out, which keeps it safe against a consumer that interrupts
// the producer halfway through.
//
// Underrun is a whole block of silence and a count, and what is in the ring
// stays there. Overrun is a short write and a count.
//
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/audiocore/__init__.h"

typedef struct {
    audiosample_base_t base;

    uint8_t *ring;          // cap bytes, a whole number of blocks
    uint32_t cap;           // capacity in bytes
    uint32_t block;         // bytes handed out per pull
    uint32_t frame;         // bytes per frame; cap and block are multiples

    // The protocol. 32-bit, wrapping, one owner each.
    uint32_t w;             // producer writes, consumer reads
    uint32_t r;             // consumer writes, producer reads
    // Where in the buffer those point. Carried, not recomputed as `w % cap`:
    // a capacity that does not divide 2^32 would make the position jump when
    // the counters wrap.
    uint32_t wpos;          // producer only
    uint32_t rpos;          // consumer only

    // clear(): the producer fills in clear_w and clear_wpos, then bumps
    // clear_seq; the consumer moves r and rpos there at its next pull and
    // copies clear_seq into clear_done.
    uint32_t clear_w;       // producer
    uint32_t clear_wpos;    // producer
    uint32_t clear_seq;     // producer
    uint32_t clear_done;    // consumer

    // Handed to the output. Two, so a consumer may still hold the previous
    // block when the next pull comes.
    uint8_t *out[2];
    uint8_t which;          // the next of out[] to fill
    uint8_t last;           // the one handed out most recently

    // An output that plays the two channels from separate pulls (two DACs
    // on one sample) asks once per channel per block. Whichever asks first
    // takes a new block; the other is handed the same one.
    uint32_t left_reads;
    uint32_t right_reads;

    // Statistics, each written by one side.
    uint32_t underruns;     // consumer: pulls that found less than a block
    uint32_t overruns;      // producer: writes that could not take it all
    uint32_t pulls;         // consumer: blocks handed out, silence included
    uint64_t wrote;         // producer: bytes accepted
    uint64_t read;          // consumer: bytes handed out as real audio
    uint64_t starved;       // consumer: bytes handed out as silence
    uint64_t in_digest;     // producer: FNV-1a 64 over every byte accepted
    uint64_t out_digest;    // consumer: over every real byte handed out
} audiopump_ring_obj_t;

// Sets up a ring of `capacity` blocks of `frames` frames. The caller has
// checked the arguments.
void audiopump_ring_construct(audiopump_ring_obj_t *self,
    uint32_t sample_rate, uint32_t channel_count, uint32_t frames,
    uint32_t capacity);

// Takes whole frames, as many as fit, and returns the bytes taken. Never
// blocks, allocates or raises.
uint32_t audiopump_ring_write(audiopump_ring_obj_t *self,
    const uint8_t *src, uint32_t len);
uint32_t audiopump_ring_space(audiopump_ring_obj_t *self);
uint32_t audiopump_ring_level(audiopump_ring_obj_t *self);
void audiopump_ring_clear(audiopump_ring_obj_t *self);

void audiopump_ring_reset_buffer(audiopump_ring_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audiopump_ring_get_buffer(
    audiopump_ring_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length);
