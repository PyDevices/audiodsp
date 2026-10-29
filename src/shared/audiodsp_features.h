// audiofeatures' engine: speech features for a wake word model, from PCM at
// any rate.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// Samples go in as interleaved s16 frames at any rate from 16 kHz up. They are
// mixed to mono and, unless they are already 16 kHz, resampled to 16 kHz
// through a windowed-sinc low-pass that is flat to 7.5 kHz and at least 60 dB
// down from 8.5 kHz. 16 kHz input skips the resampler, so the samples reach the
// frontend untouched.
//
// The frontend is TensorFlow Lite Micro's microfrontend
// (audiodsp_microfrontend.c) with the settings microWakeWord's models were
// trained on: a 30 ms window every 10 ms, 40 mel channels from 125 Hz to
// 7.5 kHz, noise reduction, PCAN gain control and a log scale. Each 10 ms of
// audio makes one slice of 40 values, which wait in a queue until read, either
// raw (uint16, about 0 to 670) or as the int8 a model takes.

#pragma once

#include <stddef.h>
#include <stdint.h>

#define AUDIODSP_FEATURES_SIZE 40           // values per slice
#define AUDIODSP_FEATURES_RATE 16000        // the frontend's own rate
#define AUDIODSP_FEATURES_STEP_MS 10        // audio per slice
#define AUDIODSP_FEATURES_WINDOW_MS 30
#define AUDIODSP_FEATURES_MIN_RATE 16000
#define AUDIODSP_FEATURES_MAX_RATE 384000

typedef struct audiodsp_features audiodsp_features_t;

typedef struct {
    uint32_t slices;        // slices made since creation or reset
    uint32_t dropped;       // slices lost because the queue was full
    uint32_t waiting;       // slices in the queue now
    uint32_t capacity;      // the queue's size in slices
    uint32_t rate;          // the input rate last fed, 0 before any
    uint8_t resampling;     // 1 when that rate goes through the resampler
} audiodsp_features_stats_t;

// A frontend whose queue holds up to `capacity` slices; NULL when out of memory.
audiodsp_features_t *audiodsp_features_new(uint32_t capacity);
void audiodsp_features_free(audiodsp_features_t *f);

// Forget all audio and queued slices, as if new.
void audiodsp_features_reset(audiodsp_features_t *f);

// Interleaved s16 frames. A rate other than the last one fed starts the
// resampler over. Returns 0, or -1 when the rate is out of range or the
// resampler couldn't get memory. When the queue is full the oldest slice goes.
int audiodsp_features_feed_s16(audiodsp_features_t *f, const int16_t *pcm, size_t frames,
    uint32_t channels, uint32_t rate);

// How many more slices the queue can take before it drops one.
uint32_t audiodsp_features_room(const audiodsp_features_t *f);
uint32_t audiodsp_features_waiting(const audiodsp_features_t *f);

// Up to max_slices of the oldest queued slices, removed from the queue.
// Returns how many were written: AUDIODSP_FEATURES_SIZE values each.
size_t audiodsp_features_read_u16(audiodsp_features_t *f, uint16_t *out, size_t max_slices);
size_t audiodsp_features_read_s8(audiodsp_features_t *f, int8_t *out, size_t max_slices);

// The int8 a microWakeWord model takes for one raw value: ESPHome's
// value * 256 / 666 - 128, rounded, in integers.
int8_t audiodsp_features_quantize(uint16_t value);

void audiodsp_features_stats(const audiodsp_features_t *f, audiodsp_features_stats_t *st);
