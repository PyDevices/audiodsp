// The pump lock: one mutex, so nobody has to remember to park.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// A C pump thread pulls an effect graph block by block while the interpreter
// goes on moving knobs on the nodes inside it. Those two threads write and
// read the same words. The spike proved the collision is real and fatal:
// `AllPass.stop()` writes {source, pending, pending_frames} as three words
// and the pull reads two of them, so a stop landing between them hands the
// DSP a length as a pointer -- x86-64 and RISC-V, same statement, same
// faulting value 0x400 (docs/spikes/live-audio-path-notes.md).
//
// The spike made that safe from Python, with `audiopump.park()` around every
// call. If the pump is the ONLY audio path, nobody -- no user, no author of
// any of the 52 effect classes or 53 instruments -- can be trusted to
// remember. So the lock lives here, in the shared C, and the control path
// takes it ITSELF, at the write.
//
// --- the contract, and it is small --------------------------------------
//
//   The pump holds the lock for the duration of one block pull.
//   The control path holds it around the FINAL SWAP ONLY.
//
// "Final swap only" is not style. Whatever is inside the lock stops the
// audio for exactly that long, so:
//
//   NEVER hold it across an allocation. A collection inside a held lock
//   starves the audio for a whole GC -- on the P4 the worst block under a
//   GC storm was 28.5 ms against a 30 ms DMA cushion, so one collection
//   inside the lock IS the hole you hear.
//
//   NEVER hold it across anything that can raise. A longjmp out of a held
//   lock leaves it held forever and the next acquire deadlocks, which is a
//   dead board rather than a bad noise. mp_raise_*, mp_obj_get_float,
//   m_new, m_malloc, a Python call, a VFS read: all of them are outside.
//
//   So: validate, allocate and compute FIRST, on the interpreter thread,
//   where raising is free; then lock, store, unlock.
//
// The mutex is recursive on purpose. A node's pull legitimately re-enters
// helpers that lock (a coefficient refresh from inside get_buffer), and a
// control-path entry legitimately calls another locked helper. Recursive
// means neither has to know about the other.
//
// It is a priority-inheriting mutex on FreeRTOS (xSemaphoreCreateRecursive-
// Mutex gives that), because the pump runs above the interpreter and an
// interpreter holding the swap must be lifted to finish it.
//
// Where there is no pump there is no mutex: with no driver bound the calls
// cost a load and a branch, which is what WebAssembly, CircuitPython and the
// CPython wheel get.
//
// There is no #if chain in here choosing a backend any more, and that is the
// point of the split: the mutex, the clock and the thread identity arrive
// through shared/audiodsp_port.h, whose default table is all NULLs. Which
// driver bound is a RUN-TIME question -- `audiopump.driver()` -- rather than
// a macro nobody can see from the build log. The trap that cost the spike a
// whole firmware (ESP_PLATFORM is not defined for a user C module, so the
// POSIX branch compiled and LINKED on esp32 because IDF's newlib has
// pthread.h) cannot be spelled in this file at all now.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- the lock -------------------------------------------------------------

// Take and drop the lock from the CONTROL path. Around the final swap only.
void audiodsp_pump_lock_acquire(void);
void audiodsp_pump_lock_release(void);

// Take and drop the lock from the PUMP. Held for one whole block pull; the
// two are separate entry points so the cost of each side is measured apart
// without having to ask which thread is calling.
void audiodsp_pump_lock_acquire_pump(void);
void audiodsp_pump_lock_release_pump(void);

// Taken by the PULL FUNNEL itself, on whichever thread is pulling.
//
// A pull is a critical section, and the caller does not get to decide that.
// audiomixer's play() primes its new voice by pulling it -- and if the pump
// is pulling the same node at that moment, two pulls share one node's
// {pending, pending_frames} and the second one hands the DSP a length as a
// pointer. That is the 0x400 crash again, arriving from the control side
// rather than from a setter, and no amount of locking the SETTERS would have
// caught it. So the funnel locks, and every caller of a pull -- the pump, a
// node pulling the node behind it, a play() priming its source, and anything
// nobody has written yet -- is safe without knowing it.
//
// Unmeasured, because it is almost always a recursive re-take inside a lock
// the pump already holds, and counting those would drown the two numbers
// that matter.
void audiodsp_pump_lock_acquire_nested(void);
void audiodsp_pump_lock_release_nested(void);

// One statement between a lock and an unlock, for the common case where the
// swap really is one store. Deliberately NOT a block form: if what you are
// writing does not fit on one line, look again at whether all of it has to
// be inside the lock.
#define AUDIODSP_PUMP_SWAP(stmt) \
    do { \
        audiodsp_pump_lock_acquire(); \
        stmt; \
        audiodsp_pump_lock_release(); \
    } while (0)

// --- is a pump running ----------------------------------------------------
//
// Set by whatever adopts a graph, from the control thread, before the pump
// thread exists and after it has gone. It is not the lock: the lock is always
// taken. This answers "may this node be pulled by a thread with no
// interpreter on it", which is a different question, and the only things that
// ask it are the file-backed sources that must refuse.
void audiodsp_pump_set_active(bool active);
bool audiodsp_pump_active(void);

// True only on the thread that is inside a block pull right now. The pump
// stamps its own identity when it takes the lock, which is the first thing it
// does on its own thread, so this needs no plumbing from the port.
//
// This is the question a node has to ask, not "is a pump running": a file
// source read by a prefetcher ON the interpreter thread while a pump plays
// elsewhere is exactly the design that is coming, and it is fine.
bool audiodsp_pump_on_pump_thread(void);

// --- the fault register ---------------------------------------------------
//
// A pull may not raise: the exception audit found the raise dies one frame
// EARLIER than the longjmp, inside gc_alloc building the exception object, on
// a thread with no interpreter state to allocate from (notes, "a raise on the
// pump thread dies allocating the exception"). So the funnel returns
// GET_BUFFER_ERROR and leaves a code here instead. The pull loop stops
// pulling that source and publishes the code; the next control call reports
// it.
enum {
    AUDIODSP_PUMP_FAULT_NONE = 0,
    AUDIODSP_PUMP_FAULT_NO_PROTOCOL = 1, // mp_proto_get_or_throw's case
    AUDIODSP_PUMP_FAULT_DEINITED = 2,    // audiosample_check_for_deinit's case
    AUDIODSP_PUMP_FAULT_UNPUMPABLE = 3,  // a file-backed source in a pulled graph
    AUDIODSP_PUMP_FAULT_IO = 4,          // a seek or read that would have raised
    AUDIODSP_PUMP_FAULT_LOOP = 5,        // a port pulled while already inside itself
};

// First fault wins: the first thing that went wrong is the cause, and
// everything after it is consequence.
void audiodsp_pump_fault_set(uint32_t code);
uint32_t audiodsp_pump_fault_get(void);
void audiodsp_pump_fault_clear(void);

// --- what the lock cost ---------------------------------------------------
//
// Carried here rather than in the pump, because the question "is the lock far
// under a block" has to be answerable on any port that builds this file, and
// because the control side's wait is invisible from the pump.
typedef struct {
    uint64_t pump_takes;
    uint64_t pump_wait_us;      // total, waiting to get in
    uint64_t pump_wait_us_max;  // the worst one: the pull the control path delayed
    uint64_t ctrl_takes;
    uint64_t ctrl_wait_us;
    uint64_t ctrl_wait_us_max;  // the worst one: a control call waiting on a pull
    uint64_t ctrl_held_us_max;  // the longest the audio was stopped by a swap
} audiodsp_pump_lock_stats_t;

const audiodsp_pump_lock_stats_t *audiodsp_pump_lock_stats(void);
void audiodsp_pump_lock_stats_reset(void);

// --- the ledger, for a hang that has to name its holder --------------------
//
// Off unless a build asks for it (-DAUDIODSP_PUMP_LOCK_LEDGER=1), and it costs
// nothing at all when it is off: every call below compiles to nothing and the
// counters do not exist.
//
// What it is for: a lock that never comes back tells you nothing from the
// outside. WHO holds it, how deep, who is waiting behind them, and what the
// last few takes and gives were -- that is the difference between "a hang" and
// a cause. The Windows driver's watchdog prints this when nothing has moved
// for a few seconds.

#ifndef AUDIODSP_PUMP_LOCK_LEDGER
#define AUDIODSP_PUMP_LOCK_LEDGER (0)
#endif

// Where in the pump's block the loop is, when it is the pump that is stuck.
enum {
    AUDIODSP_PUMP_PHASE_IDLE = 0,
    AUDIODSP_PUMP_PHASE_TOP = 1,
    AUDIODSP_PUMP_PHASE_RING_WAIT = 2,
    AUDIODSP_PUMP_PHASE_PARK = 3,
    AUDIODSP_PUMP_PHASE_LOCK = 4,
    AUDIODSP_PUMP_PHASE_PULL = 5,
    AUDIODSP_PUMP_PHASE_DIGEST = 6,
    AUDIODSP_PUMP_PHASE_SINK = 7,
    AUDIODSP_PUMP_PHASE_PACE = 8,
    AUDIODSP_PUMP_PHASE_RESET = 9,
    AUDIODSP_PUMP_PHASE_END = 10,
};

#if AUDIODSP_PUMP_LOCK_LEDGER

#define AUDIODSP_PUMP_LOCK_LEDGER_SLOTS (256)

typedef struct {
    uint64_t us;
    uintptr_t tid;
    void *ra;           // the caller of the acquire/release, for a symbol
    uint8_t site;       // 0 ctrl, 1 pump, 2 nested
    uint8_t what;       // 0 want, 1 got, 2 gave
    int16_t depth;      // after the event
    int32_t waiters;
} audiodsp_pump_lock_event_t;

typedef struct {
    uintptr_t owner;
    int32_t depth;
    int32_t waiters;
    uint64_t takes;
    uint64_t gives;
    uint32_t phase;
    uint32_t next;      // where the ring will write next
    // The oldest acquire that has not come back, and who is inside it. This
    // is the one a hang is made of: a lock whose counters are RACING while
    // one thread has been queued behind them for seconds.
    uint64_t want_us;
    uintptr_t want_tid;
    uint8_t want_site;
    uint64_t bad_gives;     // releases that took the recursion count below 0
    uint8_t bad_site;       // and where the first one came from
    uintptr_t bad_tid;
    void *bad_ra;
    const audiodsp_pump_lock_event_t *events;
} audiodsp_pump_lock_ledger_t;

void audiodsp_pump_lock_ledger_read(audiodsp_pump_lock_ledger_t *out);
void audiodsp_pump_lock_phase_set(uint32_t phase);
#define AUDIODSP_PUMP_PHASE(p) audiodsp_pump_lock_phase_set(p)

#else

#define AUDIODSP_PUMP_PHASE(p) ((void)0)

#endif

#ifdef __cplusplus
}
#endif
