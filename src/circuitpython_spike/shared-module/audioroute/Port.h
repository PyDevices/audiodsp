// audioroute.Port for CircuitPython. See audioif's src/audioroute/Port.h for
// what the node is for and why it exists; this is the same node, split the
// way CircuitPython splits every node -- the struct and the pull here, the
// Python-facing type in shared-bindings.
//
// There is NO pump lock in this half, and that is the whole difference
// between the two copies. The MicroPython twin takes `audioif_pump_lock_*`
// around `play()` and `deinit()` because on that build a C pump thread may be
// inside a pull at the moment a setter re-points the port. CircuitPython has
// no pump: its audio output pulls the graph from an interrupt or from the
// same thread, `audioif_pump_lock.c` is not in `copy_manifest.txt`, and
// nothing in this tree declares the hooks. So the lock is ABSENT here rather
// than being a fourth platform branch of it -- which is also the shape Brad's
// restructure wants, with the lock reduced to hooks that default to no-ops.
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
    //: What the port currently plays. One aligned word, so a pull either
    //: reads the whole old source or the whole new one.
    mp_obj_t source;
} audioroute_port_obj_t;

void audioroute_port_reset_buffer(audioroute_port_obj_t *self,
    bool single_channel_output, uint8_t channel);
audioio_get_buffer_result_t audioroute_port_get_buffer(
    audioroute_port_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length);
