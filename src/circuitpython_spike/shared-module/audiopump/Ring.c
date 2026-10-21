// audiopump.Ring for CircuitPython: the ring and its pull. See Ring.h.
//
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices

#include <stdint.h>
#include <string.h>

#include "py/runtime.h"

#include "shared-module/audiopump/Ring.h"

#define RING_FNV_OFFSET (0xcbf29ce484222325ULL)
#define RING_FNV_PRIME  (0x100000001b3ULL)

// Acquire and release rather than bare `volatile`: `volatile` orders the
// compiler and not the machine. The producer's release of `w` publishes the
// bytes it copied in; the consumer's release of `r` gives back the room only
// after the bytes are copied out.
#if defined(__GNUC__) || defined(__clang__)
#define RING_LOAD_ACQ(p)      __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define RING_STORE_REL(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#else
#define RING_LOAD_ACQ(p)      (*(volatile uint32_t *)(p))
#define RING_STORE_REL(p, v)  (*(volatile uint32_t *)(p) = (v))
#endif

void audiopump_ring_construct(audiopump_ring_obj_t *self,
    uint32_t sample_rate, uint32_t channel_count, uint32_t frames,
    uint32_t capacity) {
    self->frame = channel_count * 2;
    self->block = frames * self->frame;
    self->cap = self->block * capacity;
    self->ring = m_new(uint8_t, self->cap);
    self->out[0] = m_new(uint8_t, self->block);
    self->out[1] = m_new(uint8_t, self->block);
    memset(self->ring, 0, self->cap);
    memset(self->out[0], 0, self->block);
    memset(self->out[1], 0, self->block);
    self->which = 0;
    self->last = 0;
    self->w = 0;
    self->r = 0;
    self->wpos = 0;
    self->rpos = 0;
    self->clear_w = 0;
    self->clear_seq = 0;
    self->clear_done = 0;
    self->left_reads = 0;
    self->right_reads = 0;
    self->underruns = 0;
    self->overruns = 0;
    self->pulls = 0;
    self->wrote = 0;
    self->read = 0;
    self->starved = 0;
    self->in_digest = RING_FNV_OFFSET;
    self->out_digest = RING_FNV_OFFSET;

    self->base.sample_rate = sample_rate;
    self->base.max_buffer_length = self->block;
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)channel_count;
    self->base.samples_signed = true;
    self->base.single_buffer = false;
}

// --- the producer side: the interpreter ------------------------------------

// Where the producer may count the consumer as having read to. A clear the
// consumer has not carried out yet has already dropped everything up to
// clear_w, so the room is counted from there. Read in this order, both
// answers can only be behind the consumer, never ahead of it, so the room is
// never overstated.
static uint32_t ring_producer_r(audiopump_ring_obj_t *self) {
    const uint32_t r = RING_LOAD_ACQ(&self->r);
    if (RING_LOAD_ACQ(&self->clear_done) != self->clear_seq) {
        return self->clear_w;
    }
    return r;
}

uint32_t audiopump_ring_write(audiopump_ring_obj_t *self,
    const uint8_t *src, uint32_t len) {
    const uint32_t w = self->w;
    const uint32_t room = self->cap - (w - ring_producer_r(self));
    uint32_t n = len;
    if (n > room) {
        n = room;
        self->overruns++;
    }
    n -= n % self->frame;
    if (n == 0) {
        return 0;
    }
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
        digest *= RING_FNV_PRIME;
    }
    self->in_digest = digest;
    self->wrote += n;
    self->wpos = (at + n) % self->cap;
    // Release, and last: the bytes are in before they are claimed to be.
    RING_STORE_REL(&self->w, w + n);
    return n;
}

uint32_t audiopump_ring_space(audiopump_ring_obj_t *self) {
    const uint32_t room = self->cap - (self->w - ring_producer_r(self));
    return room - (room % self->frame);
}

uint32_t audiopump_ring_level(audiopump_ring_obj_t *self) {
    return self->w - ring_producer_r(self);
}

// Drops what is queued, by asking the consumer to. One word says where to
// drop to, so a pull that lands in the middle of a second clear() still finds
// a whole one: any value clear_w has held is a point the producer has written
// to.
void audiopump_ring_clear(audiopump_ring_obj_t *self) {
    self->clear_w = self->w;
    RING_STORE_REL(&self->clear_seq, self->clear_seq + 1);
}

// --- the consumer side: the output's fill -----------------------------------
//
// Nothing in here allocates or raises: it runs from a background callback or
// an interrupt.

static void ring_take_clear(audiopump_ring_obj_t *self) {
    const uint32_t seq = RING_LOAD_ACQ(&self->clear_seq);
    if (seq == self->clear_done) {
        return;
    }
    const uint32_t r = self->r;
    const uint32_t to = self->clear_w;
    const uint32_t w = RING_LOAD_ACQ(&self->w);
    // A drop point is between r and w; anything else is a value from before
    // this pull's r moved on, and dropping to it would go backwards.
    if (to - r <= w - r) {
        self->rpos = (self->rpos + (to - r)) % self->cap;
        RING_STORE_REL(&self->r, to);
    }
    RING_STORE_REL(&self->clear_done, seq);
}

static uint8_t *ring_pull_block(audiopump_ring_obj_t *self) {
    ring_take_clear(self);

    const uint8_t index = self->which;
    uint8_t *dst = self->out[index];
    self->which ^= 1;
    self->last = index;

    const uint32_t block = self->block;
    const uint32_t r = self->r;
    const uint32_t level = RING_LOAD_ACQ(&self->w) - r;

    if (level < block) {
        // A whole block of silence, and what is in the ring stays there:
        // playing part of a block early only moves the hole.
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
            digest *= RING_FNV_PRIME;
        }
        self->out_digest = digest;
        self->read += block;
        self->rpos = (at + block) % self->cap;
        RING_STORE_REL(&self->r, r + block);
    }
    self->pulls++;
    return dst;
}

void audiopump_ring_reset_buffer(audiopump_ring_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    if (single_channel_output && channel == 1) {
        return;
    }
    // Deliberately does NOT drop what is queued: `MixerVoice.play()` and every
    // output's play() call this, and a ring filled before play() would lose
    // its head start.
    self->which = 0;
    self->left_reads = self->pulls;
    self->right_reads = self->pulls;
}

audioio_get_buffer_result_t audiopump_ring_get_buffer(
    audiopump_ring_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    if (!single_channel_output) {
        channel = 0;
    }
    const uint32_t reads = channel == 1 ? self->right_reads : self->left_reads;
    uint8_t *dst;
    if (reads == self->pulls) {
        dst = ring_pull_block(self);
    } else {
        dst = self->out[self->last];
    }
    if (channel == 1) {
        self->right_reads++;
        if (self->frame == 4) {
            // The right channel of interleaved stereo starts one sample in;
            // the output reads every other sample from there.
            dst += 2;
        }
    } else {
        self->left_reads++;
    }
    *buffer = dst;
    *buffer_length = self->block;
    return GET_BUFFER_MORE_DATA;
}
