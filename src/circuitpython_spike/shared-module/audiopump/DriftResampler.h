// audiopump.DriftResampler for CircuitPython: plays a Ring filled on one
// clock at another clock's rate, steering its ratio to hold the Ring at a
// target fill.
//
// The DSP and the control loop are shared/audiodsp_drift.c, byte for byte the
// code MicroPython and CPython run. This half reads the Ring through its
// consumer calls (Ring.h), at whatever pace the ratio asks for, so it must be
// the Ring's only consumer: play the DriftResampler, never the Ring itself.
//
// It lives in audiopump rather than beside audiospeed.Resampler because
// audiospeed is CircuitPython's own module, and a module of ours never
// shadows one of CircuitPython's.
//
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/audiocore/__init__.h"
#include "shared-module/audiopump/Ring.h"
#include "shared/audiodsp_drift.h"

typedef struct {
    audiosample_base_t base;
    audiopump_ring_obj_t *ring;
    audiodsp_drift_t drift;
    float *work;
    int16_t *scratch;       // the frames one block reads from the ring
    uint32_t scratch_frames;
    int16_t *out[2];
    uint8_t which;          // the next of out[] to fill
    uint8_t last;           // the one handed out most recently
    // An output that plays the two channels from separate pulls asks once
    // per channel per block; the second is handed the same block (Ring.h).
    uint32_t pulls;
    uint32_t left_reads;
    uint32_t right_reads;
} audiopump_driftresampler_obj_t;

// The arguments are checked by the binding.
void audiopump_driftresampler_construct(audiopump_driftresampler_obj_t *self,
    audiopump_ring_obj_t *ring, uint32_t out_rate, uint32_t target,
    uint32_t max_ppm, float bandwidth);
void audiopump_driftresampler_deinit(audiopump_driftresampler_obj_t *self);

void audiopump_driftresampler_reset_buffer(audiopump_driftresampler_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audiopump_driftresampler_get_buffer(
    audiopump_driftresampler_obj_t *self, bool single_channel_output,
    uint8_t channel, uint8_t **buffer, uint32_t *buffer_length);
