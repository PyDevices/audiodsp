// The pump lock. See audioif_pump_lock.h for the contract.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices

#include "shared/audioif_pump_lock.h"

#if AUDIOIF_PUMP_LOCK_BACKEND_FREERTOS
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#elif AUDIOIF_PUMP_LOCK_BACKEND_PTHREAD
#include <pthread.h>
#include <time.h>
#elif AUDIOIF_PUMP_LOCK_BACKEND_WIN32
#include <windows.h>
#endif

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
// thread; cleared when the pump goes away.
#if AUDIOIF_PUMP_LOCK_BACKEND_FREERTOS
static TaskHandle_t audioif_pump_thread;
#define AUDIOIF_PUMP_SELF() xTaskGetCurrentTaskHandle()
#define AUDIOIF_PUMP_NOBODY (NULL)
#define AUDIOIF_PUMP_SAME(a, b) ((a) == (b))
#elif AUDIOIF_PUMP_LOCK_BACKEND_PTHREAD
static pthread_t audioif_pump_thread;
static volatile bool audioif_pump_thread_set;
#define AUDIOIF_PUMP_SELF() pthread_self()
#define AUDIOIF_PUMP_SAME(a, b) pthread_equal((a), (b))
#elif AUDIOIF_PUMP_LOCK_BACKEND_WIN32
static volatile DWORD audioif_pump_thread;
#define AUDIOIF_PUMP_SELF() GetCurrentThreadId()
#define AUDIOIF_PUMP_NOBODY (0)
#define AUDIOIF_PUMP_SAME(a, b) ((a) == (b))
#endif

// Set while the lock is held by ANY holder, with the microsecond it was taken.
// Only read by the releaser, which is the holder, so it needs no protection of
// its own.
static uint64_t audioif_pump_lock_taken_us;

#if AUDIOIF_PUMP_LOCK_STATS
static uint64_t audioif_pump_lock_now_us(void) {
    #if AUDIOIF_PUMP_LOCK_BACKEND_FREERTOS
    return (uint64_t)esp_timer_get_time();
    #elif AUDIOIF_PUMP_LOCK_BACKEND_PTHREAD
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
    #elif AUDIOIF_PUMP_LOCK_BACKEND_WIN32
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart * 1000000LL) / freq.QuadPart);
    #else
    return 0;
    #endif
}
#else
#define audioif_pump_lock_now_us() (0ULL)
#endif

// --- the mutex itself -----------------------------------------------------

#if AUDIOIF_PUMP_LOCK_BACKEND_FREERTOS

// xSemaphoreCreateRecursiveMutex, not a binary semaphore: this one both
// nests and carries priority inheritance, and the pump runs above the
// interpreter, so an interpreter holding a swap has to be lifted to finish
// it or the pump waits on a thread nothing is scheduling.
static SemaphoreHandle_t audioif_pump_mutex;

static void audioif_pump_lock_ensure(void) {
    if (audioif_pump_mutex == NULL) {
        audioif_pump_mutex = xSemaphoreCreateRecursiveMutex();
    }
}

static void audioif_pump_lock_take(void) {
    audioif_pump_lock_ensure();
    if (audioif_pump_mutex != NULL) {
        xSemaphoreTakeRecursive(audioif_pump_mutex, portMAX_DELAY);
    }
}

static void audioif_pump_lock_give(void) {
    if (audioif_pump_mutex != NULL) {
        xSemaphoreGiveRecursive(audioif_pump_mutex);
    }
}

#elif AUDIOIF_PUMP_LOCK_BACKEND_PTHREAD

// PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP exists on glibc and nowhere
// portably, so the attribute route and a pthread_once. It runs exactly once
// and never on the audio path after that.
static pthread_mutex_t audioif_pump_mutex;
static pthread_once_t audioif_pump_mutex_once = PTHREAD_ONCE_INIT;

static void audioif_pump_lock_make(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&audioif_pump_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void audioif_pump_lock_take(void) {
    pthread_once(&audioif_pump_mutex_once, audioif_pump_lock_make);
    pthread_mutex_lock(&audioif_pump_mutex);
}

static void audioif_pump_lock_give(void) {
    pthread_mutex_unlock(&audioif_pump_mutex);
}

#elif AUDIOIF_PUMP_LOCK_BACKEND_WIN32

// A CRITICAL_SECTION is recursive by definition, and InitializeCriticalSection
// on a static is safe to do under a one-shot interlocked flag.
static CRITICAL_SECTION audioif_pump_cs;
static LONG audioif_pump_cs_state;  // 0 none, 1 building, 2 ready

static void audioif_pump_lock_ensure(void) {
    if (InterlockedCompareExchange(&audioif_pump_cs_state, 1, 0) == 0) {
        InitializeCriticalSection(&audioif_pump_cs);
        InterlockedExchange(&audioif_pump_cs_state, 2);
    }
    while (InterlockedCompareExchange(&audioif_pump_cs_state, 2, 2) != 2) {
        Sleep(0);
    }
}

static void audioif_pump_lock_take(void) {
    audioif_pump_lock_ensure();
    EnterCriticalSection(&audioif_pump_cs);
}

static void audioif_pump_lock_give(void) {
    LeaveCriticalSection(&audioif_pump_cs);
}

#else

// No threads, so no pump, so no lock. The calls stay in the source of every
// node -- there is exactly one control-path shape in the palette and it does
// not fork per port.
static void audioif_pump_lock_take(void) {
}
static void audioif_pump_lock_give(void) {
}

#endif

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
    #if AUDIOIF_PUMP_LOCK_BACKEND_PTHREAD
    audioif_pump_thread = AUDIOIF_PUMP_SELF();
    audioif_pump_thread_set = true;
    #elif defined(AUDIOIF_PUMP_SELF)
    audioif_pump_thread = AUDIOIF_PUMP_SELF();
    #endif
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
        #if AUDIOIF_PUMP_LOCK_BACKEND_PTHREAD
        audioif_pump_thread_set = false;
        #elif defined(AUDIOIF_PUMP_NOBODY)
        audioif_pump_thread = AUDIOIF_PUMP_NOBODY;
        #endif
    }
}

bool audioif_pump_active(void) {
    return audioif_pump_is_active;
}

bool audioif_pump_on_pump_thread(void) {
    if (!audioif_pump_is_active) {
        return false;
    }
    #if AUDIOIF_PUMP_LOCK_BACKEND_PTHREAD
    return audioif_pump_thread_set
           && AUDIOIF_PUMP_SAME(audioif_pump_thread, AUDIOIF_PUMP_SELF());
    #elif defined(AUDIOIF_PUMP_SELF)
    return audioif_pump_thread != AUDIOIF_PUMP_NOBODY
           && AUDIOIF_PUMP_SAME(audioif_pump_thread, AUDIOIF_PUMP_SELF());
    #else
    return false;
    #endif
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
