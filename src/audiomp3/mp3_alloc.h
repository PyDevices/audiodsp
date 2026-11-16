// Ported from CircuitPython's shared-module/audiomp3/__init__.h (upstream
// repo: https://github.com/adafruit/circuitpython, MIT). Renamed from
// `__init__.h` to `mp3_alloc.h` for this port's flat layout -- content
// unchanged. `-include`d into buffers.o's compile (see micropython.mk) so
// `MPDEC_ALLOCATOR(x)`/`MPDEC_FREE(x)` (the fetched
// mp3/src/buffers.c) resolve to these two symbols; every other vendored
// lib/mp3 source file is unaware of them.
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2019 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#pragma once

#include <stdlib.h>

extern void *mp3_alloc(size_t sz);
extern void mp3_free(void *ptr);
