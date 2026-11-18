// audiopump.DriftResampler for CircuitPython. See DriftResampler.h.
//
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices

#include <string.h>

#include "py/runtime.h"

#include "shared-module/audiopump/DriftResampler.h"
#include "shared-bindings/audiocore/__init__.h"

void audiopump_driftresampler_construct(audiopump_driftresampler_obj_t *self,
    audiopump_ring_obj_t *ring, uint32_t out_rate, uint32_t target,
    uint32_t max_ppm, float bandwidth) {
    const uint32_t channels = ring->base.channel_count;
    const uint32_t frame = channels * 2u;
    const uint32_t frames = ring->block / frame;
    const uint32_t in_rate = ring->base.sample_rate;
    const uint32_t least = audiodsp_drift_min_target(frames, in_rate, out_rate,
        max_ppm);
    const uint32_t work_frames = audiodsp_drift_capacity(frames, in_rate,
        out_rate, max_ppm);

    self->ring = ring;
    self->work = m_malloc(sizeof(float) * work_frames * channels);
    self->scratch_frames = least;
    self->scratch = m_malloc(sizeof(int16_t) * least * channels);
    self->out[0] = m_malloc(frames * frame);
    self->out[1] = m_malloc(frames * frame);
    memset(self->out[0], 0, frames * frame);
    memset(self->out[1], 0, frames * frame);
    self->which = 0;
    self->last = 0;
    self->pulls = 0;
    self->left_reads = 0;
    self->right_reads = 0;
    audiodsp_drift_init(&self->drift, self->work, channels, frames, in_rate,
        out_rate, target, max_ppm, bandwidth);

    self->base.sample_rate = out_rate;
    self->base.max_buffer_length = frames * frame;
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)channels;
    self->base.samples_signed = true;
    self->base.single_buffer = false;
}

void audiopump_driftresampler_deinit(audiopump_driftresampler_obj_t *self) {
    audiosample_mark_deinit(&self->base);
}

void audiopump_driftresampler_reset_buffer(audiopump_driftresampler_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    if (single_channel_output && channel == 1) {
        return;
    }
    // Keeps the loop's state and what is queued, as the Ring does: play()
    // calls this, and a rate estimate is worth keeping across it.
    self->which = 0;
    self->left_reads = self->pulls;
    self->right_reads = self->pulls;
}

static int16_t *drift_pull_block(audiopump_driftresampler_obj_t *self) {
    audiopump_ring_obj_t *ring = self->ring;
    const uint32_t frame = self->drift.channels * 2u;
    const uint32_t block = self->drift.frames * frame;
    const uint8_t index = self->which;
    int16_t *dst = self->out[index];
    self->which ^= 1;
    self->last = index;

    const uint32_t underruns = self->drift.underruns;
    const uint32_t level = audiopump_ring_readable(ring) / frame;
    const uint32_t need = audiodsp_drift_plan(&self->drift, level);
    if (need == 0u) {
        memset(dst, 0, block);
        if (self->drift.underruns != underruns) {
            audiopump_ring_starve(ring, block);
        }
    } else {
        // In pieces the size of the scratch buffer: the block that starts
        // playback also drops what the ring holds beyond the target.
        uint32_t left = need;
        while (left != 0u) {
            const uint32_t n = left < self->scratch_frames ? left : self->scratch_frames;
            audiopump_ring_read(ring, (uint8_t *)self->scratch, n * frame);
            audiodsp_drift_push(&self->drift, self->scratch, n);
            left -= n;
        }
        audiodsp_drift_render(&self->drift, dst);
    }
    self->pulls++;
    return dst;
}

audioio_get_buffer_result_t audiopump_driftresampler_get_buffer(
    audiopump_driftresampler_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length) {
    if (!single_channel_output) {
        channel = 0;
    }
    const uint32_t reads = channel == 1 ? self->right_reads : self->left_reads;
    uint8_t *dst;
    if (reads == self->pulls) {
        dst = (uint8_t *)drift_pull_block(self);
    } else {
        dst = (uint8_t *)self->out[self->last];
    }
    if (channel == 1) {
        self->right_reads++;
        if (self->drift.channels == 2u) {
            // The right channel of interleaved stereo starts one sample in.
            dst += 2;
        }
    } else {
        self->left_reads++;
    }
    *buffer = dst;
    *buffer_length = self->drift.frames * self->drift.channels * 2u;
    return GET_BUFFER_MORE_DATA;
}
