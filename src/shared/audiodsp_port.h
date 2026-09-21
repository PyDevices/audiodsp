// What the portable engine needs from a port, and nothing else.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// This repo's C has never included an ESP-IDF, FreeRTOS, pthread or Win32
// header on `main` and must not start: the same sources build as a MicroPython
// usermod, as a CPython extension and inside CircuitPython. The pull loop
// nevertheless needs a thread, a mutex, a clock and a place to put audio, and
// all four of those are platform. So they arrive through here.
//
// Every hook may be NULL. A table of nothing but NULLs is a complete and
// correct port: one thread, no hardware, `service()` drives the loop, and the
// lock compiles away to two calls that do nothing. That is exactly how the
// WebAssembly build runs today and it is what the CPython wheel gets.
//
// --- how a driver binds ---------------------------------------------------
//
// It defines `audiodsp_port_driver()`, which overrides the weak default below
// at link time. Weak override is the zero-cost way and it has one trap: in a
// STATIC ARCHIVE the weak default can satisfy the reference first, the
// driver's object is never pulled in, and the default wins silently. The
// driver dodges that by putting its hook table in the SAME translation unit as
// its MicroPython module definition -- `MP_REGISTER_MODULE` puts an undefined
// reference to that module in the firmware's module table, so the object is
// always pulled in, archive or not.
//
// That is an argument, not a proof, so the binding is checked at RUN TIME on
// every build: `audiopump.driver()` returns the name below and a build whose
// driver did not bind says "none" out loud instead of quietly pumping nothing.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What spawn() asks for when it wants a thread. Ignored wholesale by a driver
// that has only one kind.
typedef struct {
    int core;       // < 0: the driver picks (the core the interpreter is not on)
    int prio;
    int stack;      // bytes
    bool psram;     // take the stack from PSRAM where the port has it
} audiodsp_port_thread_cfg_t;

typedef struct {
    // "esp32", "pthread", "win32", "none". Never NULL.
    const char *name;

    // --- the recursive mutex ------------------------------------------------
    //
    // RECURSIVE, and priority-inheriting where the OS has it: the pump runs
    // above the interpreter on a board, so an interpreter holding the swap has
    // to be lifted to finish it. See shared/audiodsp_pump_lock.h for the
    // contract these two serve.
    void (*lock_take)(void);
    void (*lock_give)(void);

    // --- the clock ----------------------------------------------------------
    //
    // Monotonic microseconds. NULL means "no clock": every duration the engine
    // publishes reads zero, which is honest on a port that cannot time itself.
    uint64_t (*now_us)(void);

    // --- the thread ---------------------------------------------------------
    //
    // thread_start is the one hook whose absence changes the shape of the
    // engine: with no thread, spawn() adopts the graph and returns, and the
    // loop advances only inside audiopump.service().
    //
    // `entry` is called once, on the new thread, and returns when the loop is
    // done. `*where` is what spawn() hands back to Python: the core the thread
    // was pinned to, or -1 for "a thread, no affinity".
    bool (*thread_start)(void (*entry)(void *), void *arg,
        const audiodsp_port_thread_cfg_t *cfg, int *where);
    // Wake a pump that is parked or waiting to notice `stop`.
    void (*thread_wake)(void);
    // Join or delete the thread and free what it held. Called only after the
    // engine has seen the loop finish (or has given up waiting for it), and
    // safe to call when no thread was ever started.
    void (*thread_release)(void);
    // Who is calling, as one word; 0 means "nobody". Used only to answer "am I
    // on the pump thread", so any value that is stable per thread and unique
    // between the two threads that matter will do.
    uintptr_t (*self_id)(void);
    // What STATUS_TID should say. A different question from self_id: on esp32
    // the useful answer is which core the pump landed on, not the task handle.
    uint64_t (*status_tid)(void);
    // Stack the pump thread never used, in whatever unit the port counts.
    uint32_t (*stack_free)(void);

    // --- waiting ------------------------------------------------------------
    //
    // park_spin is the pump's own wait while parked: it must return when
    // thread_wake() is called, and it may return early. sleep_us is the
    // control side's wait -- pacing, and the bounded waits in teardown -- and
    // it MUST NOT raise, because one of its callers is a finaliser.
    void (*park_spin)(uint32_t max_us);
    void (*sleep_us)(uint64_t us);

    // --- the sink -----------------------------------------------------------
    //
    // One write, whatever it is written to. On a desktop that is the file
    // sink_open() opened; on a board it is the I2S channel the driver's own
    // Python surface opened, and sink_open is NULL there.
    bool (*sink_open)(const char *path);
    void (*sink_close)(void);
    // True when a hardware sink is open and a block may be written to it.
    bool (*sink_ready)(void);
    uint32_t (*sink_write)(const uint8_t *buffer, uint32_t length,
        uint32_t timeout_ms, bool *timed_out);
    // What the hardware actually clocked out and in. The only way an underrun
    // gets measured at all; zero on a port with no DMA to ask.
    uint64_t (*sink_dma_bytes)(void);
    uint64_t (*sink_rx_bytes)(void);

    // --- let go of everything -----------------------------------------------
    //
    // Called from the engine's teardown, which a soft reset reaches through a
    // finaliser. The driver drops whatever the VM cannot: the I2S channel on a
    // board, the waitable timer on Windows. Must not raise and must be safe to
    // call twice.
    void (*teardown)(void);
} audiodsp_port_ops_t;

// The bound driver, resolved once. Never NULL.
const audiodsp_port_ops_t *audiodsp_port(void);

// Convenience, and the two questions everything else asks.
const char *audiodsp_port_name(void);
bool audiodsp_port_threaded(void);

// Overridden by a driver; see the note at the top of this file.
const audiodsp_port_ops_t *audiodsp_port_driver(void);

#ifdef __cplusplus
}
#endif
