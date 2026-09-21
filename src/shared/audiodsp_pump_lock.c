// The pump lock. See audiodsp_pump_lock.h for the contract.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// There are no platform branches in this file any more. It had three --
// FreeRTOS, pthread, Win32 -- and they were the only reason this repo's C
// included freertos/semphr.h, pthread.h and windows.h. The mutex, the clock
// and the thread identity now arrive through shared/audiodsp_port.h, and a
// port that supplies none of them gets exactly what WebAssembly always got:
// calls that do nothing, because there is no second thread to keep out.

#include "shared/audiodsp_pump_lock.h"

#include "shared/audiodsp_port.h"

// Measuring costs two clock reads per acquire. A block is thousands of
// microseconds and an acquire is tens of nanoseconds, so this is never the
// thing that matters -- but it is the only way to answer "how long did the
// audio stand still", so it is on by default and switchable.
#ifndef AUDIODSP_PUMP_LOCK_STATS
#define AUDIODSP_PUMP_LOCK_STATS (1)
#endif

static audiodsp_pump_lock_stats_t audiodsp_pump_lock_counters;
static volatile bool audiodsp_pump_is_active;
static volatile uint32_t audiodsp_pump_fault;

// Who is pulling. Stamped by acquire_pump, which runs on the pump's own
// thread; cleared when the pump goes away. One word, whatever the port's
// thread handle really is -- the only question asked of it is "the same one
// again?", so a value that is stable per thread is all it has to be.
static volatile uintptr_t audiodsp_pump_thread;
static volatile bool audiodsp_pump_thread_set;

// Set while the lock is held by ANY holder, with the microsecond it was taken.
// Only read by the releaser, which is the holder, so it needs no protection of
// its own.
static uint64_t audiodsp_pump_lock_taken_us;

#if AUDIODSP_PUMP_LOCK_STATS
static uint64_t audiodsp_pump_lock_now_us(void) {
    const audiodsp_port_ops_t *port = audiodsp_port();
    return port->now_us != NULL ? port->now_us() : 0;
}
#else
#define audiodsp_pump_lock_now_us() (0ULL)
#endif

// The atomics the ledger's counters use. Same builtins the push ring uses,
// and for the same reason: `volatile` orders the compiler, not the machine.
#if defined(__GNUC__) || defined(__clang__)
#define AUDIODSP_LOCK_ADD(p, v) __atomic_add_fetch((p), (v), __ATOMIC_ACQ_REL)
#define AUDIODSP_LOCK_GET(p)    __atomic_load_n((p), __ATOMIC_ACQUIRE)
#else
#define AUDIODSP_LOCK_ADD(p, v) (*(p) += (v))
#define AUDIODSP_LOCK_GET(p)    (*(p))
#endif

// Where a take or a give was called from, for the ledger. A compiler builtin
// on GCC and Clang, an intrinsic on MSVC -- declared here rather than pulled in
// through <intrin.h>, so this file still includes nothing that is not ISO C.
// The CPython wheel is built with MSVC on Windows, which is the one compiler
// in the matrix that does not have `__builtin_return_address`: every local
// build (GCC, MinGW, emscripten, the two ESP32 toolchains) had it, and the
// first CI run on windows-latest failed at link with an unresolved external.
#if defined(_MSC_VER) && !defined(__clang__)
void *_ReturnAddress(void);
#pragma intrinsic(_ReturnAddress)
#define AUDIODSP_LOCK_CALLER() _ReturnAddress()
#elif defined(__GNUC__) || defined(__clang__)
#define AUDIODSP_LOCK_CALLER() __builtin_return_address(0)
#else
#define AUDIODSP_LOCK_CALLER() ((void *)0)
#endif

// --- the mutex itself -----------------------------------------------------
//
// Whatever the driver gave us, or nothing. "Nothing" is the honest answer
// where there is no pump: the calls stay in the source of every node -- there
// is exactly one control-path shape in the palette and it does not fork per
// port -- and on a single-threaded build they cost a load and a branch.

#if AUDIODSP_PUMP_LOCK_LEDGER

// The ledger. Nothing in here is synchronised, on purpose: it is read by a
// watchdog thread while the two threads that matter are stuck, and a reader
// that took the lock to read the lock's state would be the eleventh thing
// waiting on it. Every field is one word, written by its owner, and a torn
// ring slot is worth more than no ring at all.
static audiodsp_pump_lock_event_t audiodsp_pump_lock_ring[
    AUDIODSP_PUMP_LOCK_LEDGER_SLOTS];
static volatile uint32_t audiodsp_pump_lock_ring_next;
static volatile uintptr_t audiodsp_pump_lock_owner;
static int32_t audiodsp_pump_lock_depth;
static int32_t audiodsp_pump_lock_waiters;
static uint64_t audiodsp_pump_lock_takes;
static uint64_t audiodsp_pump_lock_gives;
static volatile uint32_t audiodsp_pump_lock_phase;
static volatile uint64_t audiodsp_pump_lock_want_us;
static volatile uintptr_t audiodsp_pump_lock_want_tid;
static volatile uint8_t audiodsp_pump_lock_want_site;
static uint64_t audiodsp_pump_lock_bad_gives;
// This thread's own recursion count, kept beside the mutex's.
static __thread int32_t audiodsp_pump_lock_mine;
static volatile uint8_t audiodsp_pump_lock_bad_site;
static volatile uintptr_t audiodsp_pump_lock_bad_tid;
static void *volatile audiodsp_pump_lock_bad_ra;

static uintptr_t audiodsp_pump_lock_self(void) {
    const audiodsp_port_ops_t *port = audiodsp_port();
    return port->self_id != NULL ? port->self_id() : 0;
}

static volatile bool audiodsp_pump_lock_frozen;

static void audiodsp_pump_lock_note(uint8_t site, uint8_t what, void *ra) {
    if (audiodsp_pump_lock_frozen) {
        return;
    }
    const uint32_t at = audiodsp_pump_lock_ring_next++
        % AUDIODSP_PUMP_LOCK_LEDGER_SLOTS;
    audiodsp_pump_lock_event_t *e = &audiodsp_pump_lock_ring[at];
    e->us = audiodsp_pump_lock_now_us();
    e->tid = audiodsp_pump_lock_self();
    e->ra = ra;
    e->site = site;
    e->what = what;
    e->depth = (int16_t)audiodsp_pump_lock_depth;
    e->waiters = audiodsp_pump_lock_waiters;
}

void audiodsp_pump_lock_ledger_read(audiodsp_pump_lock_ledger_t *out) {
    out->owner = audiodsp_pump_lock_owner;
    out->depth = audiodsp_pump_lock_depth;
    out->waiters = audiodsp_pump_lock_waiters;
    out->takes = audiodsp_pump_lock_takes;
    out->gives = audiodsp_pump_lock_gives;
    out->phase = audiodsp_pump_lock_phase;
    out->next = audiodsp_pump_lock_ring_next;
    out->want_us = audiodsp_pump_lock_want_us;
    out->want_tid = audiodsp_pump_lock_want_tid;
    out->want_site = audiodsp_pump_lock_want_site;
    out->bad_gives = audiodsp_pump_lock_bad_gives;
    out->bad_site = audiodsp_pump_lock_bad_site;
    out->bad_tid = audiodsp_pump_lock_bad_tid;
    out->bad_ra = audiodsp_pump_lock_bad_ra;
    out->events = audiodsp_pump_lock_ring;
}

void audiodsp_pump_lock_phase_set(uint32_t phase) {
    audiodsp_pump_lock_phase = phase;
}

#define AUDIODSP_LEDGER_WANT(site, ra) \
    do { \
        (void)AUDIODSP_LOCK_ADD(&audiodsp_pump_lock_waiters, 1); \
        if ((site) == 0 && audiodsp_pump_lock_want_us == 0) { \
            audiodsp_pump_lock_want_tid = audiodsp_pump_lock_self(); \
            audiodsp_pump_lock_want_site = (site); \
            audiodsp_pump_lock_want_us = audiodsp_pump_lock_now_us(); \
        } \
        audiodsp_pump_lock_note((site), 0, (ra)); \
    } while (0)
#define AUDIODSP_LEDGER_GOT(site, ra) \
    do { \
        (void)AUDIODSP_LOCK_ADD(&audiodsp_pump_lock_waiters, -1); \
        if ((site) == 0) { \
            audiodsp_pump_lock_want_us = 0; \
        } \
        audiodsp_pump_lock_owner = audiodsp_pump_lock_self(); \
        (void)AUDIODSP_LOCK_ADD(&audiodsp_pump_lock_depth, 1); \
        (void)AUDIODSP_LOCK_ADD(&audiodsp_pump_lock_takes, 1); \
        audiodsp_pump_lock_note((site), 1, (ra)); \
    } while (0)
#define AUDIODSP_LEDGER_GAVE(site, ra) \
    do { \
        (void)AUDIODSP_LOCK_ADD(&audiodsp_pump_lock_depth, -1); \
        (void)AUDIODSP_LOCK_ADD(&audiodsp_pump_lock_gives, 1); \
        if (audiodsp_pump_lock_depth <= 0) { \
            audiodsp_pump_lock_owner = 0; \
        } \
        audiodsp_pump_lock_note((site), 2, (ra)); \
    } while (0)

#else
#define AUDIODSP_LEDGER_WANT(site, ra) ((void)0)
#define AUDIODSP_LEDGER_GOT(site, ra) ((void)0)
#define AUDIODSP_LEDGER_GAVE(site, ra) ((void)0)
#endif

static void audiodsp_pump_lock_take_at(uint8_t site, void *ra) {
    const audiodsp_port_ops_t *port = audiodsp_port();
    AUDIODSP_LEDGER_WANT(site, ra);
    if (port->lock_take != NULL) {
        port->lock_take();
    }
    AUDIODSP_LEDGER_GOT(site, ra);
    #if AUDIODSP_PUMP_LOCK_LEDGER
    audiodsp_pump_lock_mine++;
    #endif
    (void)site;
    (void)ra;
}

static void audiodsp_pump_lock_give_at(uint8_t site, void *ra) {
    const audiodsp_port_ops_t *port = audiodsp_port();
    AUDIODSP_LEDGER_GAVE(site, ra);
    #if AUDIODSP_PUMP_LOCK_LEDGER
    // PER THREAD, so nothing another thread does can be mistaken for this
    // one's. A release on a thread whose own count is already zero is a
    // release of something it never took -- the thing that turns a
    // CRITICAL_SECTION into a section nobody can ever enter again. Latch the
    // first one with its caller's return address; the driver prints it as an
    // image offset.
    if (audiodsp_pump_lock_mine-- <= 0 && audiodsp_pump_lock_bad_gives++ == 0) {
        audiodsp_pump_lock_bad_site = site;
        audiodsp_pump_lock_bad_tid = audiodsp_pump_lock_self();
        audiodsp_pump_lock_bad_ra = ra;
        // Stop writing the ring HERE. Two threads fill 256 slots in under a
        // millisecond, and the watchdog looks every 200 ms -- so without this
        // the history around the first bad release is gone long before anyone
        // reads it. Frozen, the ring IS that history.
        audiodsp_pump_lock_note((site), 2, ra);
        audiodsp_pump_lock_frozen = true;
    }
    #endif
    if (port->lock_give != NULL) {
        port->lock_give();
    }
    (void)site;
    (void)ra;
}

#define audiodsp_pump_lock_take(ra) audiodsp_pump_lock_take_at(0, (ra))
#define audiodsp_pump_lock_give(ra) audiodsp_pump_lock_give_at(0, (ra))

// --- the two sides --------------------------------------------------------

void audiodsp_pump_lock_acquire(void) {
    #if AUDIODSP_PUMP_LOCK_STATS
    const uint64_t t0 = audiodsp_pump_lock_now_us();
    audiodsp_pump_lock_take(AUDIODSP_LOCK_CALLER());
    const uint64_t t1 = audiodsp_pump_lock_now_us();
    const uint64_t waited = t1 - t0;
    audiodsp_pump_lock_counters.ctrl_takes++;
    audiodsp_pump_lock_counters.ctrl_wait_us += waited;
    if (waited > audiodsp_pump_lock_counters.ctrl_wait_us_max) {
        audiodsp_pump_lock_counters.ctrl_wait_us_max = waited;
    }
    audiodsp_pump_lock_taken_us = t1;
    #else
    audiodsp_pump_lock_take(AUDIODSP_LOCK_CALLER());
    #endif
}

void audiodsp_pump_lock_release(void) {
    #if AUDIODSP_PUMP_LOCK_STATS
    const uint64_t held = audiodsp_pump_lock_now_us() - audiodsp_pump_lock_taken_us;
    if (held > audiodsp_pump_lock_counters.ctrl_held_us_max) {
        audiodsp_pump_lock_counters.ctrl_held_us_max = held;
    }
    #endif
    audiodsp_pump_lock_give(AUDIODSP_LOCK_CALLER());
}

void audiodsp_pump_lock_acquire_pump(void) {
    const audiodsp_port_ops_t *port = audiodsp_port();
    if (port->self_id != NULL) {
        audiodsp_pump_thread = port->self_id();
        audiodsp_pump_thread_set = true;
    }
    #if AUDIODSP_PUMP_LOCK_STATS
    const uint64_t t0 = audiodsp_pump_lock_now_us();
    audiodsp_pump_lock_take_at(1, AUDIODSP_LOCK_CALLER());
    const uint64_t waited = audiodsp_pump_lock_now_us() - t0;
    audiodsp_pump_lock_counters.pump_takes++;
    audiodsp_pump_lock_counters.pump_wait_us += waited;
    if (waited > audiodsp_pump_lock_counters.pump_wait_us_max) {
        audiodsp_pump_lock_counters.pump_wait_us_max = waited;
    }
    #else
    audiodsp_pump_lock_take_at(1, AUDIODSP_LOCK_CALLER());
    #endif
}

void audiodsp_pump_lock_release_pump(void) {
    audiodsp_pump_lock_give_at(1, AUDIODSP_LOCK_CALLER());
}

void audiodsp_pump_lock_acquire_nested(void) {
    audiodsp_pump_lock_take_at(2, AUDIODSP_LOCK_CALLER());
}

void audiodsp_pump_lock_release_nested(void) {
    audiodsp_pump_lock_give_at(2, AUDIODSP_LOCK_CALLER());
}

// --- the rest -------------------------------------------------------------

void audiodsp_pump_set_active(bool active) {
    audiodsp_pump_is_active = active;
    if (!active) {
        audiodsp_pump_thread_set = false;
        audiodsp_pump_thread = 0;
    }
}

bool audiodsp_pump_active(void) {
    return audiodsp_pump_is_active;
}

bool audiodsp_pump_on_pump_thread(void) {
    if (!audiodsp_pump_is_active || !audiodsp_pump_thread_set) {
        return false;
    }
    const audiodsp_port_ops_t *port = audiodsp_port();
    if (port->self_id == NULL) {
        return false;
    }
    return audiodsp_pump_thread == port->self_id();
}

void audiodsp_pump_fault_set(uint32_t code) {
    if (audiodsp_pump_fault == AUDIODSP_PUMP_FAULT_NONE) {
        audiodsp_pump_fault = code;
    }
}

uint32_t audiodsp_pump_fault_get(void) {
    return audiodsp_pump_fault;
}

void audiodsp_pump_fault_clear(void) {
    audiodsp_pump_fault = AUDIODSP_PUMP_FAULT_NONE;
}

const audiodsp_pump_lock_stats_t *audiodsp_pump_lock_stats(void) {
    return &audiodsp_pump_lock_counters;
}

void audiodsp_pump_lock_stats_reset(void) {
    audiodsp_pump_lock_counters = (audiodsp_pump_lock_stats_t) { 0 };
}
