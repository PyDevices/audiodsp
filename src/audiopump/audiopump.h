// The two things the platform driver needs from the engine.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// The traffic goes the other way almost everywhere: the engine asks the driver
// for a thread, a mutex, a clock and a sink through shared/audioif_port.h. These
// two go this way because the state they touch is the engine's.

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Arm the soft-reset guard. Anything that takes ownership of hardware calls
// this, because a soft reset frees the heap out from under whatever is
// holding it and the guard's finaliser is how the teardown gets run at all --
// the esp32 port has no hook a user C module can register in its soft-reset
// path, but gc_sweep_all() runs __del__ on every object that has one. A
// driver that opens an I2S channel without a pump spawned still needs that
// channel closed.
void audiopump_arm_guard(void);

// True while a pump is adopted and its loop has not finished. Asked by a
// driver entry point that cannot share the hardware with a running pump --
// two owners of one I2S channel is the failure that sounds like silence.
bool audiopump_is_running(void);

#ifdef __cplusplus
}
#endif
