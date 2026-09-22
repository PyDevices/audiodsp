// Where the pump's per-block path is placed. One macro, no platform in it.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices

#pragma once

// --- where the per-block path is placed -----------------------------------
//
// Empty everywhere, and it is the BUILD that fills it in. On the esp32 ports
// the pump's block loop runs from flash through the cache, and an ESP-IDF
// flash write disables that cache: measured on an ESP32-P4-WIFI6-Touch-LCD-4B
// at 48 kHz stereo, one 64 ms `open/write/flush/close` of 4 kB cost the
// speaker about 6 ms of audio, and the deficit is permanent because the wire
// paces the pump and it can never catch the time back (audiodsp#142).
//
// Meanwhile `audioif`'s DMA completion callbacks ARE in IRAM, so during that
// write the byte counter goes on rising and the ring goes on draining while
// the one half that could refill it is the half that cannot execute. The two
// halves of the same path disagree about whether they need the cache.
//
// It is a macro with no platform in it because this repository's C is not
// allowed to know what platform it is on -- `tools/check_portable.py` bans
// `IRAM_ATTR` by name, and rightly: the header it comes from is an IDF
// header. So the esp32 build glue passes the expansion in, as a plain
// section attribute that needs no header:
//
//     -DAUDIODSP_HOT=__attribute__((section(".iram1.audiodsp")))
//
// which is what `IRAM_ATTR` expands to, minus the `__COUNTER__` suffix the
// IDF uses to keep each function in its own section. The IDF's linker
// fragments glob `.iram1*`, so a fixed name lands in the same place.
//
// OFF BY DEFAULT, on every port including esp32. IRAM is scarce -- a few
// hundred kB shared with every other driver that wants it -- so this is a
// question of which functions rather than which files, and nobody has paid
// for the answer yet. `micropython.cmake` turns it on for
// `AUDIODSP_PUMP_IRAM=1`.
#ifndef AUDIODSP_HOT
#define AUDIODSP_HOT
#endif
