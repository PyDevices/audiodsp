// Ported from CircuitPython's shared-bindings+shared-module/audiospeed/
// SpeedChanger.{h,c} (upstream repo: https://github.com/adafruit/circuitpython,
// MIT). Merged into one header/source pair -- see audiomixer/Mixer.h for why
// this port doesn't keep the shared-bindings/shared-module split.
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Tod Kurt
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "audiocore/__init__.h"
#include "py/obj.h"

// Fixed-point 16.16 format
#define SPEED_SHIFT 16

typedef struct {
    audiosample_base_t base;
    mp_obj_t source;
    uint8_t *output_buffer;
    uint32_t output_buffer_length; // in bytes, allocated size
    // Source buffer cache
    uint8_t *src_buffer;
    uint32_t src_buffer_length; // in bytes
    uint32_t src_sample_count;  // in frames
    // Phase accumulator and rate in 16.16 fixed-point (units: source frames)
    uint32_t phase;
    uint32_t rate_fp; // 16.16 fixed-point rate
    bool source_done;      // source returned DONE on last get_buffer
    bool source_exhausted; // source DONE and we consumed all of it
} audiospeed_speedchanger_obj_t;

extern const mp_obj_type_t audiospeed_speedchanger_type;

// Shared with Resampler, whose first member is a SpeedChanger.
mp_obj_t audiospeed_speedchanger_sources(mp_obj_t self_in,
    uint8_t index);

void common_hal_audiospeed_speedchanger_construct(audiospeed_speedchanger_obj_t *self,
    mp_obj_t source, uint32_t rate_fp);
void common_hal_audiospeed_speedchanger_deinit(audiospeed_speedchanger_obj_t *self);
void common_hal_audiospeed_speedchanger_set_rate(audiospeed_speedchanger_obj_t *self, uint32_t rate_fp);
uint32_t common_hal_audiospeed_speedchanger_get_rate(audiospeed_speedchanger_obj_t *self);

void audiospeed_speedchanger_reset_buffer(audiospeed_speedchanger_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audiospeed_speedchanger_get_buffer(audiospeed_speedchanger_obj_t *self,
    bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length);
