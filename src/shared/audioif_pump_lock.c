// The pump lock. See audioif_pump_lock.h for the contract.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// There are no platform branches in this file any more. It had three --
// FreeRTOS, pthread, Win32 -- and they were the only reason this repo's C
// included freertos/semphr.h, pthread.h and windows.h. The mutex, the clock
// and the thread identity now arrive through shared/audioif_port.h, and a
// port that supplies none of them gets exactly what WebAssembly always got:
// calls that do nothing, because there is no second thread to keep out.

#include "shared/audioif_pump_lock.h"

#include "shared/audioif_port.h"

// Measuring costs two clock reads per acquire. A block is thousands of
// microseconds and an acquire is tens of nanoseconds, so this is never the
// thing that matters -- but it is the only way to answer "how long did the
// audio stand still", so it is on by default and switchable.
#ifndef AUDIOIF_PUMP_LOCK_STATS
#define AUDIOIF_PUMP_LOCK_STATS (1)
#endif

static audioif_pump_lock_stats_t audioif_pump_lock_counters;
static volatile bool audioif_pump_is_active;
static volatile uint32_t audioif_pump_fault;

// Who is pulling. Stamped by acquire_pump, which runs on the pump's own
// thread; cleared when the pump goes away. One word, whatever the port's
// thread handle really is -- the only question asked of it is "the same one
// again?", so a value that is stable per thread is all it has to be.
static volatile uintptr_t audioif_pump_thread;
static volatile bool audioif_pump_thread_set;

// Set while the lock is held by ANY holder, with the microsecond it was taken.
// Only read by the releaser, which is the holder, so it needs no protection of
// its own.
static uint64_t audioif_pump_lock_taken_us;

#if AUDIOIF_PUMP_LOCK_STATS
static uint64_t audioif_pump_lock_now_us(void) {
    const audioif_port_ops_t *port = audioif_port();
    return port->now_us != NULL ? port->now_us() : 0;
}
#else
#define audioif_pump_lock_now_us() (0ULL)
#endif

// --- the mutex itself -----------------------------------------------------
//
// Whatever the driver gave us, or nothing. "Nothing" is the honest answer
// where there is no pump: the calls stay in the source of every node -- there
// is exactly one control-path shape in the palette and it does not fork per
// port -- and on a single-threaded build they cost a load and a branch.

static void audioif_pump_lock_take(void) {
    const audioif_port_ops_t *port = audioif_port();
    if (port->lock_take != NULL) {
        port->lock_take();
    }
}

static void audioif_pump_lock_give(void) {
    const audioif_port_ops_t *port = audioif_port();
    if (port->lock_give != NULL) {
        port->lock_give();
    }
}

// --- the two sides --------------------------------------------------------

void audioif_pump_lock_acquire(void) {
    #if AUDIOIF_PUMP_LOCK_STATS
    const uint64_t t0 = audioif_pump_lock_now_us();
    audioif_pump_lock_take();
    const uint64_t t1 = audioif_pump_lock_now_us();
    const uint64_t waited = t1 - t0;
    audioif_pump_lock_counters.ctrl_takes++;
    audioif_pump_lock_counters.ctrl_wait_us += waited;
    if (waited > audioif_pump_lock_counters.ctrl_wait_us_max) {
        audioif_pump_lock_counters.ctrl_wait_us_max = waited;
    }
    audioif_pump_lock_taken_us = t1;
    #else
    audioif_pump_lock_take();
    #endif
}

void audioif_pump_lock_release(void) {
    #if AUDIOIF_PUMP_LOCK_STATS
    const uint64_t held = audioif_pump_lock_now_us() - audioif_pump_lock_taken_us;
    if (held > audioif_pump_lock_counters.ctrl_held_us_max) {
        audioif_pump_lock_counters.ctrl_held_us_max = held;
    }
    #endif
    audioif_pump_lock_give();
}

void audioif_pump_lock_acquire_pump(void) {
    const audioif_port_ops_t *port = audioif_port();
    if (port->self_id != NULL) {
        audioif_pump_thread = port->self_id();
        audioif_pump_thread_set = true;
    }
    #if AUDIOIF_PUMP_LOCK_STATS
    const uint64_t t0 = audioif_pump_lock_now_us();
    audioif_pump_lock_take();
    const uint64_t waited = audioif_pump_lock_now_us() - t0;
    audioif_pump_lock_counters.pump_takes++;
    audioif_pump_lock_counters.pump_wait_us += waited;
    if (waited > audioif_pump_lock_counters.pump_wait_us_max) {
        audioif_pump_lock_counters.pump_wait_us_max = waited;
    }
    #else
    audioif_pump_lock_take();
    #endif
}

void audioif_pump_lock_release_pump(void) {
    audioif_pump_lock_give();
}

void audioif_pump_lock_acquire_nested(void) {
    audioif_pump_lock_take();
}

void audioif_pump_lock_release_nested(void) {
    audioif_pump_lock_give();
}

// --- the rest -------------------------------------------------------------

void audioif_pump_set_active(bool active) {
    audioif_pump_is_active = active;
    if (!active) {
        audioif_pump_thread_set = false;
        audioif_pump_thread = 0;
    }
}

bool audioif_pump_active(void) {
    return audioif_pump_is_active;
}

bool audioif_pump_on_pump_thread(void) {
    if (!audioif_pump_is_active || !audioif_pump_thread_set) {
        return false;
    }
    const audioif_port_ops_t *port = audioif_port();
    if (port->self_id == NULL) {
        return false;
    }
    return audioif_pump_thread == port->self_id();
}

void audioif_pump_fault_set(uint32_t code) {
    if (audioif_pump_fault == AUDIOIF_PUMP_FAULT_NONE) {
        audioif_pump_fault = code;
    }
}

uint32_t audioif_pump_fault_get(void) {
    return audioif_pump_fault;
}

void audioif_pump_fault_clear(void) {
    audioif_pump_fault = AUDIOIF_PUMP_FAULT_NONE;
}

const audioif_pump_lock_stats_t *audioif_pump_lock_stats(void) {
    return &audioif_pump_lock_counters;
}

void audioif_pump_lock_stats_reset(void) {
    audioif_pump_lock_counters = (audioif_pump_lock_stats_t) { 0 };
}
