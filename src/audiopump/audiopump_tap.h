// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"

extern const mp_obj_type_t audiopump_tap_type;

// Called by the pump, after the tail has produced its block. One memcpy, no
// lock, no allocation, and nothing at all when no tap is attached.
void audiopump_tap_write(mp_obj_t tap, const uint8_t *buffer, uint32_t length);

// For a C reader that wants the output as a STREAM rather than a window: a
// cast task on another core that must carry every byte the pump played, and
// cannot wait on the interpreter (a garbage collection over a large heap
// holds Python for seconds; the pump and this reader do not stop for it).
//
// `*cursor` is the reader's own position in the tap's byte counter; start it
// at audiopump_tap_position(). Copies the whole frames written since then,
// at most `max` bytes, advances `*cursor`, and returns the byte count. When
// the writer has lapped the reader, the oldest bytes are gone: the read
// jumps to the newest `cap` bytes and `*lapped` is set. Safe from any core
// or task while the pump writes; it neither locks nor allocates, and it
// leaves the Python-side readinto() mark alone.
uint32_t audiopump_tap_position(mp_obj_t tap);
uint32_t audiopump_tap_frame_bytes(mp_obj_t tap);
uint32_t audiopump_tap_read_since(mp_obj_t tap, uint32_t *cursor,
    uint8_t *dst, uint32_t max, bool *lapped);
