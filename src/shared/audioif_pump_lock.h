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
// Where there is no pump there is no mutex: the no-op backend compiles the
// calls away, which is what WebAssembly and any single-threaded host get.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- which backend --------------------------------------------------------
//
// ESP_PLATFORM is NOT defined for a user C module by the IDF -- it is a CMake
// variable and a definition inside IDF components, and the POSIX branch below
// LINKS on esp32 anyway because IDF's newlib has pthread.h. That trap already
// cost the spike a whole firmware (see the notes, "the trap that nearly ate
// the run"), so audioif's micropython.cmake defines
// AUDIOIF_PUMP_LOCK_FREERTOS itself when IDF_TARGET is set, and this file
// keys off that first.
#if defined(AUDIOIF_PUMP_LOCK_FREERTOS) || defined(ESP_PLATFORM)
#define AUDIOIF_PUMP_LOCK_BACKEND_FREERTOS (1)
#elif defined(AUDIOIF_PUMP_LOCK_NONE) || defined(__EMSCRIPTEN__)
#define AUDIOIF_PUMP_LOCK_BACKEND_NONE (1)
#elif defined(_WIN32)
#define AUDIOIF_PUMP_LOCK_BACKEND_WIN32 (1)
#elif defined(__unix__) || defined(__APPLE__)
#define AUDIOIF_PUMP_LOCK_BACKEND_PTHREAD (1)
#else
#define AUDIOIF_PUMP_LOCK_BACKEND_NONE (1)
#endif

// --- the lock -------------------------------------------------------------

// Take and drop the lock from the CONTROL path. Around the final swap only.
void audioif_pump_lock_acquire(void);
void audioif_pump_lock_release(void);

// Take and drop the lock from the PUMP. Held for one whole block pull; the
// two are separate entry points so the cost of each side is measured apart
// without having to ask which thread is calling.
void audioif_pump_lock_acquire_pump(void);
void audioif_pump_lock_release_pump(void);

// One statement between a lock and an unlock, for the common case where the
// swap really is one store. Deliberately NOT a block form: if what you are
// writing does not fit on one line, look again at whether all of it has to
// be inside the lock.
#define AUDIOIF_PUMP_SWAP(stmt) \
    do { \
        audioif_pump_lock_acquire(); \
        stmt; \
        audioif_pump_lock_release(); \
    } while (0)

// --- is a pump running ----------------------------------------------------
//
// Set by whatever adopts a graph, from the control thread, before the pump
// thread exists and after it has gone. It is not the lock: the lock is always
// taken. This answers "may this node be pulled by a thread with no
// interpreter on it", which is a different question, and the only things that
// ask it are the file-backed sources that must refuse.
void audioif_pump_set_active(bool active);
bool audioif_pump_active(void);

// True only on the thread that is inside a block pull right now. The pump
// stamps its own identity when it takes the lock, which is the first thing it
// does on its own thread, so this needs no plumbing from the port.
//
// This is the question a node has to ask, not "is a pump running": a file
// source read by a prefetcher ON the interpreter thread while a pump plays
// elsewhere is exactly the design that is coming, and it is fine.
bool audioif_pump_on_pump_thread(void);

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
    AUDIOIF_PUMP_FAULT_NONE = 0,
    AUDIOIF_PUMP_FAULT_NO_PROTOCOL = 1, // mp_proto_get_or_throw's case
    AUDIOIF_PUMP_FAULT_DEINITED = 2,    // audiosample_check_for_deinit's case
    AUDIOIF_PUMP_FAULT_UNPUMPABLE = 3,  // a file-backed source in a pulled graph
    AUDIOIF_PUMP_FAULT_IO = 4,          // a seek or read that would have raised
};

// First fault wins: the first thing that went wrong is the cause, and
// everything after it is consequence.
void audioif_pump_fault_set(uint32_t code);
uint32_t audioif_pump_fault_get(void);
void audioif_pump_fault_clear(void);

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
} audioif_pump_lock_stats_t;

const audioif_pump_lock_stats_t *audioif_pump_lock_stats(void);
void audioif_pump_lock_stats_reset(void);

#ifdef __cplusplus
}
#endif
