// Helix's buffers.c, compiled with this port's allocator.
//
// buffers.c takes its eight decoder buffers from MPDEC_ALLOCATOR(x) when that
// is defined, and from static arrays when it is not. The static arrays are
// 23,816 bytes of .bss, which on an ESP32-S3 is internal RAM the Wi-Fi, BLE
// and display drivers want. Defined, they come from the GC heap the first time
// an MP3Decoder is made, as in CircuitPython's own port, and each decoder gets
// buffers of its own.
//
// The macros used to be set with set_source_files_properties() in
// micropython.cmake. That property belongs to the directory that sets it, and
// the ESP32 build compiles every module source twice, once in the main
// component's directory and once more in the project's: the linked copy was
// the one without the flags. Defining the macros in a file of our own works
// the same in every build, whichever directory compiles it. buffers.c itself
// is not changed.
//
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include "audiomp3/mp3_alloc.h"

#define MPDEC_ALLOCATOR(x) mp3_alloc(x)
#define MPDEC_FREE(x) mp3_free(x)

#include "buffers.c"
