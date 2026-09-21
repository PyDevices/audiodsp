// The default port: one thread, no hardware. See shared/audiodsp_port.h.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices

#include "shared/audiodsp_port.h"

// Every hook NULL. That is not a stub waiting to be filled in -- it is the
// whole of what a port with one thread and no sink needs, and three of the
// four builds this repo ships take it: WebAssembly, CircuitPython and the
// CPython wheel.
static const audiodsp_port_ops_t audiodsp_port_none_ops = {
    .name = "none",
};

#if defined(__GNUC__) || defined(__clang__)
#define AUDIODSP_PORT_WEAK __attribute__((weak))
#else
// No weak symbols: a driver linked into this binary would collide at link
// time rather than override, which is a loud failure and the right one. Every
// toolchain this repo builds with (gcc, clang, emcc, MinGW-w64, xtensa/riscv
// gcc) has them.
#define AUDIODSP_PORT_WEAK
#endif

AUDIODSP_PORT_WEAK const audiodsp_port_ops_t *audiodsp_port_driver(void) {
    return &audiodsp_port_none_ops;
}

const audiodsp_port_ops_t *audiodsp_port(void) {
    // Resolved once. The call is a link-time constant in practice, but the
    // cache also means a driver that answered once cannot answer differently
    // half way through a run.
    static const audiodsp_port_ops_t *bound;
    if (bound == NULL) {
        const audiodsp_port_ops_t *ops = audiodsp_port_driver();
        bound = (ops != NULL && ops->name != NULL) ? ops : &audiodsp_port_none_ops;
    }
    return bound;
}

const char *audiodsp_port_name(void) {
    return audiodsp_port()->name;
}

bool audiodsp_port_threaded(void) {
    return audiodsp_port()->thread_start != NULL;
}
