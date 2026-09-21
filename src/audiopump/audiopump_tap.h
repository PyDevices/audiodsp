// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices

#pragma once

#include <stdint.h>

#include "py/obj.h"

extern const mp_obj_type_t audiopump_tap_type;

// Called by the pump, after the tail has produced its block. One memcpy, no
// lock, no allocation, and nothing at all when no tap is attached.
void audiopump_tap_write(mp_obj_t tap, const uint8_t *buffer, uint32_t length);
