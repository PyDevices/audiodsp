// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// audiopump - the spike's C pull loop, written against audioif's runtime
// neutral sample protocol.
//
// Three jobs, one loop:
//
//   1. `pull()` runs the loop on the calling thread and allocates NOTHING,
//      so `micropython.heap_lock()` around it turns "the audio pull does not
//      allocate" from a claim into a gate. `audiocore.get_buffer()` cannot
//      do this: it m_mallocs a copy of every block (audiocore/module.c), so
//      the harness fails the gate before any node gets a chance to.
//
//   2. `spawn()` runs the same loop off the interpreter thread -- a plain
//      pthread on unix, a FreeRTOS task pinned to the core the interpreter
//      is NOT on for esp32. Byte identity between the two is the whole
//      proof.
//
//   3. The loop's sink. On unix that is a file descriptor; on esp32 it is a
//      RAM ring the interpreter drains, an I2S TX channel, or both.
//
// The one thing the loop may not do is call into the MicroPython runtime.
// That rules out `audiosample_get_buffer()`, the funnel every node uses to
// pull the node behind it, because it calls `mp_proto_get_or_throw()` and
// `audiosample_check_for_deinit()` and both of those raise. So the protocol
// is resolved once, on the interpreter thread, into an
// `audioif_sample_source_t`, and the loop calls through that. Node-internal
// pulls still go through the funnel; see the spike notes for what that costs.

#include <stdint.h>
#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "audiocore/__init__.h"
#include "shared/audioif_port.h"
#include "shared/audioif_pump_lock.h"
#include "shared/audioif_sample.h"

#include "audiopump/audiopump.h"
#include "audiopump/audiopump_events.h"
#include "audiopump/audiopump_ring.h"
#include "audiopump/audiopump_tap.h"

// The same acquire/release pair the push ring uses, and here for the same
// reason: `volatile` orders the compiler, not the machine, and a 64-bit store
// on RV32 or Xtensa is two 32-bit stores with a window between them.
#if defined(__GNUC__) || defined(__clang__)
#define AUDIOPUMP_LOAD_ACQ(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define AUDIOPUMP_STORE_REL(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#else
#define AUDIOPUMP_LOAD_ACQ(p)     (*(volatile uint32_t *)(p))
#define AUDIOPUMP_STORE_REL(p, v) (*(volatile uint32_t *)(p) = (v))
#endif

// There is no platform #if chain in this file, and there is not allowed to be
// one: this is the DSP repo, whose C builds as a MicroPython usermod, as a
// CPython extension and inside CircuitPython, and tools/check_portable.py
// fails the build if an ESP-IDF, FreeRTOS, pthread or Win32 header turns up
// anywhere in src/.
//
// The thread, the mutex, the clock, the pacing and the sink all arrive through
// shared/audioif_port.h. A driver binds itself by defining
// audioif_port_driver(); with none bound the table is all NULLs and this loop
// runs exactly as the WebAssembly port runs it today -- one thread, no
// hardware, driven a block at a time from audiopump.service().
//
// What used to be spelled AUDIOPUMP_ESP / _WIN / _WASM is now asked of the
// bound driver at run time, so the trap those macros existed for cannot come
// back: AUDIOPUMP_ESP32 had to be defined by micropython.cmake because the IDF
// does not hand ESP_PLATFORM to a user C module, and getting it wrong compiled
// and LINKED the POSIX branch on esp32 (newlib has pthread.h) for a silently
// unpinned pump with no sink. `audiopump.driver()` answers that question out
// loud instead.

// The pump's own clock. The driver's if it has one -- esp_timer on a board,
// QueryPerformanceCounter on Windows, CLOCK_MONOTONIC on unix -- and
// MicroPython's portable HAL otherwise, which is what WebAssembly gets. Never
// clock_gettime from this file: this file does not get to know what a clock is
// made of.
static inline uint64_t audiopump_now_us(void) {
    const audioif_port_ops_t *port = audioif_port();
    if (port->now_us != NULL) {
        return port->now_us();
    }
    // 32 bits and wraps, on a port with no pump thread and nothing to pace.
    // Deltas across the wrap are still right for anything shorter than the
    // wrap; absolute wall time on such a port is not a number anybody reads.
    return (uint64_t)mp_hal_ticks_us();
}

// --- the runtime-neutral part ---------------------------------------------

// Layout of the caller's status bytearray: 32 x uint64_t, little endian,
// written by the loop and read by Python with struct.unpack_from. A
// bytearray rather than an array() so the width is not a typecode argument;
// MicroPython's GC blocks are 16-byte aligned, so the 64-bit stores are too.
// The width and the handful of words a C caller reads are in
// audiopump/audiopump.h; the rest are private to this file.
enum {
    STATUS_BLOCKS = 0,      // blocks pulled
    STATUS_BYTES = 1,       // bytes seen
    STATUS_DIGEST = 2,      // FNV-1a 64 over every byte, in order
    STATUS_LAST_RESULT = 3, // last audioif_buffer_result_t
    STATUS_RUNNING = 4,     // 1 while the pump thread is inside the loop
    STATUS_ERROR = 5,       // 0 none, 1 buffer error, 2 null buffer, 3 done
    STATUS_TID = 6,         // unix: pthread_self(); esp32: the pinned core
    STATUS_PARKED = 7,      // 1 while the pump is parked at a block boundary
    STATUS_RING_W = 8,      // bytes ever written into the ring
    STATUS_RING_R = 9,      // bytes ever drained out of it
    STATUS_RING_OVF = 10,   // blocks the ring had no room for
    STATUS_DRAIN_DIGEST = 11, // FNV-1a 64 over every byte drained, in order
    STATUS_PULL_US = 12,    // time inside audioif_sample_get, microseconds
    STATUS_SINK_US = 13,    // time inside the sink write
    STATUS_SINK_TIMEOUTS = 14,
    STATUS_SINK_BYTES = 15,
    STATUS_WALL_US = 16,    // first block to last, microseconds
    STATUS_PARKS = 17,      // times the pump parked
    STATUS_PARK_US = 18,    // total time parked
    STATUS_STACK_FREE = 19, // uxTaskGetStackHighWaterMark at the end
    STATUS_MAX_PULL_US = 20, // worst single block
    STATUS_DMA_BYTES = 21,  // bytes the I2S TX DMA actually clocked out
    STATUS_RX_BYTES = 22,   // bytes the I2S RX DMA actually clocked in
    STATUS_IN_TIMEOUTS = 23, // Input blocks the RX channel could not fill
    // --- what audioif's pump lock says (shared/audioif_pump_lock.h) ---
    STATUS_FAULT = 24,          // audioif_pump_fault_get(): why a pull gave up
    STATUS_LOCK_PUMP_WAIT = 25, // worst us the PUMP waited for a control swap
    STATUS_LOCK_CTRL_WAIT = 26, // worst us a CONTROL call waited for a pull
    STATUS_LOCK_CTRL_HELD = 27, // worst us the audio stood still inside a swap
    STATUS_LOCK_PUMP_TAKES = 28,
    STATUS_LOCK_CTRL_TAKES = 29,
    STATUS_FRAMES = 30,         // frames pulled since spawn(), what now() reads
    STATUS_EVENT_US_MAX = 31,   // worst single apply pass at a block boundary
    // --- what the output ring's back-pressure cost --------------------------
    STATUS_RING_WAITS = 32,     // times the pump waited for the drain
    STATUS_RING_WAIT_US = 33,   // total time it spent waiting
};

// How long the pump sleeps in one go while it waits for the ring's consumer.
// This is a CEILING and not a period: the drain wakes it (audiopump_drain
// below calls thread_wake), so in the ordinary case the sleep ends in a
// context switch and this number is never reached. It exists so that a driver
// whose wake is lost -- or one whose park_spin is still the old yield -- costs
// a bounded stall rather than a hung pump.
#define AUDIOPUMP_RING_WAIT_US (20000)

#define FNV_OFFSET (0xcbf29ce484222325ULL)
#define FNV_PRIME  (0x100000001b3ULL)

typedef struct {
    audioif_sample_source_t source;
    // The adapter the source's context points at. Lives here, not on a
    // stack, because the pump thread outlives the call that built it.
    const audiosample_p_t *protocol;
    mp_obj_t sample;
    // What the tail looked like when it was adopted. Checked at every block
    // boundary: if the type word behind `sample` is no longer this, the heap
    // that owned the graph has been reused under us and the only safe thing
    // left is to stop. See "teardown on soft reset" in the spike notes --
    // the finaliser below is the mechanism, this is the net under it.
    const void *sample_type;
    uint64_t *status;
    uint64_t blocks;
    // The RAM ring the interpreter drains. NULL when nobody asked for one.
    //
    // 32-BIT COUNTERS, as the push ring's are and for the reason it found:
    // these two words are the protocol between the pump thread and the
    // interpreter, and a 64-bit store is two stores on every 32-bit target we
    // ship. `ring_w - ring_r` in unsigned 32-bit arithmetic is the level even
    // across the wrap. The POSITIONS are tracked separately rather than as
    // `w % ring_len`, because a ring whose length does not divide 2^32 jumps
    // its position when the counter wraps -- at 48 kHz stereo that is once
    // every 6.2 hours, which is the kind of bug a customer finds.
    uint8_t *ring;
    uint32_t ring_len;
    uint32_t ring_w;            // pump stores, interpreter loads
    uint32_t ring_r;            // interpreter stores, pump loads
    uint32_t ring_wpos;         // pump only
    uint32_t ring_rpos;         // interpreter only
    uint64_t ring_w_total;      // pump only: the status word
    uint64_t ring_r_total;      // interpreter only
    uint64_t drain_digest;      // interpreter only
    // The clock, and what rides on it. `frames` is published with a release
    // store and read by audiopump.now(); see audiopump_events.c for why it is
    // 32 bits and what that costs.
    uint32_t frames;
    uint32_t frame_bytes;
    uint32_t block_frames;
    mp_obj_t events;            // an Events queue, or MP_OBJ_NULL
    mp_obj_t tap;               // a Tap, or MP_OBJ_NULL
    // The format conversion, when the source is not already signed 16-bit
    // stereo. NULL when it is, which is every graph audioif builds -- this
    // costs nothing to carry and one branch a block to skip.
    uint8_t *conv;
    uint32_t conv_len;
    uint8_t conv_bits;
    uint8_t conv_channels;
    bool conv_signed;
    volatile bool stop;
    volatile bool park_req;
    volatile bool parked;
    volatile bool finished;
    // The pump is asleep waiting for room in the output ring. The DRAIN reads
    // this, on the interpreter thread, and wakes the pump when it has given
    // space back. A stale read either way costs nothing: a stale true is one
    // wake nobody needed, and a stale false is bounded by
    // AUDIOPUMP_RING_WAIT_US.
    volatile bool ring_wait;
    bool to_sink;               // write every block to the driver's sink
    uint32_t sink_timeout_ms;
    // Hold each block until its wall-clock moment, so the desktop
    // pump runs at the rate a DMA sink would run it at. Nothing on the
    // desktop paces the pump otherwise, and without a pace ANY statement
    // about a Python-timed note being late is a statement about how fast this
    // machine spins. Off by default, and never compiled on esp32, where the
    // sink is the pace.
    bool paced;
    uint32_t pace_rate;
    // Rewind the tail instead of ending when it says DONE. This is where
    // CircuitPython puts it too: `I2SOut.play(sample, loop=True)` loops in the
    // OUTPUT -- `audiosample_reset_buffer` from inside the DMA fill -- because
    // a RawSample has no idea it is being looped. Here the pump IS the output,
    // so the flag is the pump's. It is a plain bool and only the loop reads
    // it; a caller who wants to stop a loop calls stop().
    bool loop;
    volatile bool retarget_req; // re-read `sample` at the next block boundary
    // What `loop` becomes WITH that swap: -1 leave it, 0 off, 1 on. It has to
    // travel with the retarget rather than be stored straight into `loop`,
    // because the swap only happens at the next block boundary and the OLD
    // tail is pulled until then. A single-buffer RawSample says DONE on EVERY
    // pull, so lowering the flag early ends the loop on the old tail -- the
    // pump stops with "the source ran out" one block before the graph it was
    // being pointed at ever ran.
    volatile int retarget_loop;
    // --- what the loop accumulates -------------------------------------
    //
    // These were locals in audiopump_run(). They live here so the SAME loop
    // can be entered a block at a time from audiopump.service() on a port with
    // no thread, and pick up exactly where it left off -- one digest, one
    // block count, one wall clock across every call. The threaded ports still
    // run the loop once, to completion, and never look at `service_mode`.
    uint64_t acc_digest;
    uint64_t acc_blocks;
    uint64_t acc_bytes;
    uint64_t acc_error;
    uint64_t acc_pull_us;
    uint64_t acc_sink_us;
    uint64_t acc_park_us;
    uint64_t acc_max_pull_us;
    uint64_t acc_sink_bytes;
    uint64_t acc_sink_timeouts;
    uint64_t acc_parks;
    uint64_t acc_ring_ovf;
    uint64_t acc_ring_waits;
    uint64_t acc_ring_wait_us;
    uint64_t acc_event_us_max;
    uint64_t wall_start;
    audioif_buffer_result_t acc_result;
    bool begun;                 // run_begin() has published the first words
    // wasm: the loop is entered from Python and must never block. It stops
    // when the output ring has no room for another block instead of dropping
    // one -- which is the "make the ring block the pump" fix the desktop
    // driver asks for, in the form a single-threaded port can have it.
    bool service_mode;
    uint32_t max_block_bytes;   // what the tail said it can hand back
    uint64_t park_enter_us;     // wasm: when the park the loop returned on began
} audiopump_ctx_t;

// Why a service() call gave the thread back. Two bits, so the answer and the
// block count fit in one small int and service() need not allocate.
#define AUDIOPUMP_SERVICE_SHIFT (2)
#define AUDIOPUMP_SERVICE_MASK (3)
enum {
    AUDIOPUMP_SERVICE_MORE = 0,   // budget spent; there is more to pull
    AUDIOPUMP_SERVICE_FULL = 1,   // the ring has no room for another block
    AUDIOPUMP_SERVICE_PARKED = 2, // a park was asked for
    AUDIOPUMP_SERVICE_DONE = 3,   // the loop left; status says why
};

static audiopump_ctx_t audiopump_ctx;

// A thread (or a service loop) has adopted a tail and nothing has torn it
// down yet. One flag for every port: which of the two it is comes from
// ctx.service_mode, and that comes from whether a driver supplied a thread.
static bool audiopump_live;

// The control side's wait, in the two places the interpreter has to hold for
// the pump: park()'s poll and teardown's bounded wait. The driver's, because
// one of its callers is a finaliser and mp_hal_delay_ms ends in
// mp_handle_pending, which can raise -- on esp32 that was a vTaskDelay written
// out by hand. mp_hal_delay_us is the fallback and does not raise on any port
// here.
static void audiopump_wait_us(uint64_t us) {
    const audioif_port_ops_t *port = audioif_port();
    if (port->sleep_us != NULL) {
        port->sleep_us(us);
    } else {
        mp_hal_delay_us((mp_uint_t)us);
    }
}

// The protocol adapter, as a pair of functions with no MicroPython in them
// beyond the already-resolved function pointers.
static audioif_status_t audiopump_reset(void *context,
    bool single_channel_output, uint8_t audio_channel) {
    audiopump_ctx_t *ctx = context;
    ctx->protocol->reset_buffer(ctx->sample, single_channel_output,
        audio_channel);
    return AUDIOIF_STATUS_OK;
}

static audioif_status_t audiopump_get(void *context,
    bool single_channel_output, uint8_t audio_channel,
    const uint8_t **buffer, uint32_t *buffer_length,
    audioif_buffer_result_t *result) {
    audiopump_ctx_t *ctx = context;
    // The tail's deinit check, which nothing else does. This adapter calls
    // the protocol DIRECTLY -- that is the whole point of resolving it once
    // on the interpreter thread -- so it bypasses audiosample_get_buffer and
    // the guard that lives there. The guard covers every node INSIDE the
    // graph and not the one the pump is holding.
    //
    // Found by the storm: a class deinit()ing its own Mixer while the pump
    // held it, and mix_down_one_voice writing into word_buffer=0x0. Under
    // the lock now, deinit cannot land mid-pull; this catches the case where
    // it landed between two pulls, which is legitimate and must be silence
    // with a reason rather than a write through NULL.
    if (audiosample_deinited((audiosample_base_t *)MP_OBJ_TO_PTR(ctx->sample))) {
        audioif_pump_fault_set(AUDIOIF_PUMP_FAULT_DEINITED);
        *buffer = NULL;
        *buffer_length = 0;
        *result = AUDIOIF_BUFFER_ERROR;
        return AUDIOIF_STATUS_DEINITIALIZED;
    }
    uint8_t *raw = NULL;
    audioio_get_buffer_result_t got = ctx->protocol->get_buffer(ctx->sample,
        single_channel_output, audio_channel, &raw, buffer_length);
    *buffer = raw;
    *result = (audioif_buffer_result_t)got;
    return AUDIOIF_STATUS_OK;
}

static const audioif_sample_ops_t audiopump_ops = {
    .reset_buffer = audiopump_reset,
    .get_buffer = audiopump_get,
};

// One pulled block, converted into the scratch: signed 16-bit stereo,
// always, which is what CircuitPython's own output produces ("Mono samples
// will be converted to stereo by copying value to both the left channel and
// the right channel"). Returns the converted length in bytes.
//
// It is here rather than in the interpreter for the reason the header gives,
// and it obeys the middle loop's rule exactly: no mp_* call, no allocation,
// nothing that takes a lock. `audiosample_convert_*` are audiocore's own,
// the same functions CircuitPython's outputs call.
static uint32_t audiopump_convert_block(audiopump_ctx_t *ctx,
    const uint8_t *in, uint32_t length) {
    const uint32_t in_frame = (uint32_t)ctx->conv_channels
        * (uint32_t)(ctx->conv_bits / 8);
    uint32_t frames = in_frame ? length / in_frame : 0;
    // Never past the end of the scratch, whatever a node claimed its block
    // size was: a short write is audible, a long one is somebody else's heap.
    if (frames > ctx->conv_len / 4) {
        frames = ctx->conv_len / 4;
    }
    int16_t *out = (int16_t *)(void *)ctx->conv;
    if (ctx->conv_bits == 16 && ctx->conv_signed) {
        // Signed 16-bit reaches here only as MONO: a stereo one needs no
        // conversion at all and a mono one on a stereo channel does.
        audiosample_convert_s16m_s16s(out,
            (const int16_t *)(const void *)in, frames);
    } else if (ctx->conv_bits == 16 && ctx->conv_channels == 2) {
        audiosample_convert_u16s_s16s(out,
            (const uint16_t *)(const void *)in, frames);
    } else if (ctx->conv_bits == 16) {
        audiosample_convert_u16m_s16s(out,
            (const uint16_t *)(const void *)in, frames);
    } else if (ctx->conv_signed && ctx->conv_channels == 2) {
        audiosample_convert_s8s_s16s(out, (const int8_t *)in, frames);
    } else if (ctx->conv_signed) {
        audiosample_convert_s8m_s16s(out, (const int8_t *)in, frames);
    } else if (ctx->conv_channels == 2) {
        audiosample_convert_u8s_s16s(out, in, frames);
    } else {
        audiosample_convert_u8m_s16s(out, in, frames);
    }
    return frames * 4;
}

// The loop, in three pieces: what it publishes before the first block, the
// blocks themselves, and what it publishes after the last one. A threaded port
// calls all three back to back (audiopump_run below) and never notices the
// seam; wasm calls the middle one once per timer tick.
//
// No mp_* call, no allocation, no libc that could take a lock the interpreter
// also takes -- in the middle piece. That rule is what makes heap_lock() a
// gate, and splitting the function does not relax it.
static void audiopump_run_begin(audiopump_ctx_t *ctx) {
    ctx->acc_digest = FNV_OFFSET;
    ctx->acc_result = AUDIOIF_BUFFER_MORE_DATA;
    ctx->status[STATUS_RUNNING] = 1;
    // The core on a board, the thread id on a desktop -- and 0 where there is
    // one thread and it is the interpreter's, which is the honest answer and
    // what the byte-identity probe asserts against on WebAssembly.
    const audioif_port_ops_t *port = audioif_port();
    ctx->status[STATUS_TID] = port->status_tid != NULL ? port->status_tid() : 0;
    ctx->wall_start = audiopump_now_us();
    ctx->begun = true;
}

static void audiopump_run_end(audiopump_ctx_t *ctx) {
    ctx->status[STATUS_WALL_US] = audiopump_now_us() - ctx->wall_start;
    ctx->status[STATUS_DIGEST] = ctx->acc_digest;
    ctx->status[STATUS_LAST_RESULT] = (uint64_t)ctx->acc_result;
    ctx->status[STATUS_ERROR] = ctx->acc_error;
    const audioif_port_ops_t *port = audioif_port();
    if (port->stack_free != NULL) {
        ctx->status[STATUS_STACK_FREE] = (uint64_t)port->stack_free();
    }
    if (port->sink_dma_bytes != NULL) {
        ctx->status[STATUS_DMA_BYTES] = port->sink_dma_bytes();
    }
    ctx->status[STATUS_RUNNING] = 0;
    ctx->finished = true;
}

// Pull at most `budget` blocks. Returns one of AUDIOPUMP_SERVICE_*; a threaded
// caller passes UINT64_MAX and only ever gets DONE.
static int audiopump_run_blocks(audiopump_ctx_t *ctx, uint64_t budget) {
    uint64_t digest = ctx->acc_digest;
    uint64_t blocks = ctx->acc_blocks;
    uint64_t bytes = ctx->acc_bytes;
    uint64_t error = ctx->acc_error;
    uint64_t pull_us = ctx->acc_pull_us;
    uint64_t sink_us = ctx->acc_sink_us;
    uint64_t park_us = ctx->acc_park_us;
    uint64_t max_pull_us = ctx->acc_max_pull_us;
    uint64_t sink_bytes = ctx->acc_sink_bytes;
    uint64_t sink_timeouts = ctx->acc_sink_timeouts;
    uint64_t parks = ctx->acc_parks;
    uint64_t ring_ovf = ctx->acc_ring_ovf;
    uint64_t ring_waits = ctx->acc_ring_waits;
    uint64_t ring_wait_us = ctx->acc_ring_wait_us;
    uint64_t event_us_max = ctx->acc_event_us_max;
    audioif_buffer_result_t result = ctx->acc_result;
    const uint64_t wall_start = ctx->wall_start;
    uint64_t spent = 0;
    int why = AUDIOPUMP_SERVICE_DONE;
    // Resolved once per call rather than per block: this is the hot loop and
    // the binding cannot change under it.
    const audioif_port_ops_t *port = audioif_port();
    // Can this build make the pump WAIT for the ring rather than drop into it?
    // Three things have to be true and none of them changes inside the loop:
    // there is a ring, there is a thread of our own to put to sleep, and the
    // driver has a wait that a wake can end. A build missing any of them keeps
    // the old drop-and-count behaviour, which is still the honest thing on a
    // port that cannot sleep -- and STATUS_RING_OVF then means what it always
    // meant.
    const bool ring_waits_here = ctx->ring != NULL && !ctx->service_mode
        && port->park_spin != NULL;

    while (blocks < ctx->blocks && !ctx->stop) {
        if (spent >= budget) {
            why = AUDIOPUMP_SERVICE_MORE;
            break;
        }
        // The ring is the pace, on every port that has one. A block that will
        // not fit is not dropped and not overwritten: the pull simply does not
        // happen until the consumer has made room. That is what an I2S write
        // does on a board, and it is why `ovf` is 0 by construction rather
        // than by luck -- by the time a drop counter says a block was lost the
        // block is gone, so there is no policy that can be built on top of it.
        //
        // The two ports differ only in who does the waiting. With no thread
        // (wasm, and any build whose driver did not bind) the loop gives the
        // thread back and the caller comes round again after its drain. With a
        // thread the loop SLEEPS here, and a `stop` or a `park` still gets
        // through on the next turn because both of them wake it.
        if (ctx->service_mode && ctx->ring != NULL) {
            const uint32_t level = ctx->ring_w - ctx->ring_r;
            const uint32_t room = ctx->ring_len - level;
            const uint32_t need = ctx->max_block_bytes ? ctx->max_block_bytes : 1;
            if (room < need) {
                why = AUDIOPUMP_SERVICE_FULL;
                break;
            }
        } else if (ring_waits_here && !ctx->park_req && ctx->max_block_bytes
                   && ctx->max_block_bytes <= ctx->ring_len) {
            // `!park_req` is not belt and braces. Without it a park asked for
            // while the ring is full is a LIVE LOCK: the wait below exits at
            // once because park_req is set, `continue` goes back to the top,
            // the ring is still full so it waits again, and the park branch
            // below is never reached. Measured as park() returning False after
            // 897 ms of a spinning core.
            // The `need <= ring_len` guard is not defensive tidiness. A ring
            // shorter than ONE block of this graph can never hold one -- an
            // audiocore.RawSample hands back its whole buffer, so a half-second
            // tone is a 96 kB block -- and waiting for room that cannot arrive
            // would be a hang instead of a fault. That case falls through to
            // the drop below, exactly as before, and STATUS_RING_OVF names it.
            const uint32_t need = ctx->max_block_bytes;
            if (ctx->ring_len - (ctx->ring_w - AUDIOPUMP_LOAD_ACQ(&ctx->ring_r))
                < need) {
                const uint64_t w0 = audiopump_now_us();
                // Both published BEFORE the first sleep. The flag, because a
                // drain that runs between the check and the sleep must still
                // see a waiter and wake it. The COUNT, because a pump that is
                // still inside the wait is exactly the state anything watching
                // wants to know about -- publishing it on the way out instead
                // made a stopped pump read as a pump that had never waited,
                // and the gate for this change reported 0 waits while it was
                // sitting in one.
                ctx->ring_wait = true;
                ring_waits++;
                ctx->status[STATUS_RING_WAITS] = ring_waits;
                while (!ctx->stop && !ctx->park_req) {
                    if (ctx->ring_len
                        - (ctx->ring_w - AUDIOPUMP_LOAD_ACQ(&ctx->ring_r))
                        >= need) {
                        break;
                    }
                    port->park_spin(AUDIOPUMP_RING_WAIT_US);
                }
                ctx->ring_wait = false;
                ring_wait_us += audiopump_now_us() - w0;
                ctx->status[STATUS_RING_WAIT_US] = ring_wait_us;
                // Round again rather than falling through: `stop` ends the
                // loop at the top, a park is handled below on the next turn,
                // and a retarget that landed while we slept is read inside the
                // lock as usual. Nothing here has advanced `spent`, so the
                // budget test at the top cannot loop.
                continue;
            }
        }
        // The park, at the block boundary and nowhere else. The control
        // thread asks; the pump finishes the block it is in, says it has
        // parked, and waits. See docs/spikes/live-audio-path-handoff.md.
        if (ctx->park_req) {
            // Nothing can clear park_req while this call holds the only
            // thread, so waiting here is a hang, not a park. Give the thread
            // back parked, count the park once however many service() calls
            // arrive while it lasts, and charge the time when it ends.
            if (ctx->service_mode) {
                if (!ctx->parked) {
                    parks++;
                    ctx->parked = true;
                    ctx->status[STATUS_PARKED] = 1;
                    ctx->status[STATUS_PARKS] = parks;
                    ctx->acc_parks = parks;
                    ctx->park_enter_us = audiopump_now_us();
                }
                why = AUDIOPUMP_SERVICE_PARKED;
                break;
            }
            const uint64_t park_start = audiopump_now_us();
            parks++;
            ctx->parked = true;
            ctx->status[STATUS_PARKED] = 1;
            ctx->status[STATUS_PARKS] = parks;
            while (ctx->park_req && !ctx->stop) {
                if (port->park_spin == NULL) {
                    // No driver, and service_mode left above -- so this is a
                    // pull() that somehow got here. Break rather than spin, so
                    // it returns instead of hanging the only thread there is.
                    break;
                }
                // The driver's wait. It BLOCKS on all three ports -- a
                // direct-to-task notify on esp32, an auto-reset event on
                // Windows, a condvar on unix -- and the wake at the other end
                // is what ends it. 100 ms is a ceiling, not a period. The two
                // desktop branches used to yield here instead of waiting,
                // which made this loop a tight spin on a whole core for as
                // long as the pump was parked; see the driver's own note.
                port->park_spin(100000);
            }
            ctx->parked = false;
            ctx->status[STATUS_PARKED] = 0;
            park_us += audiopump_now_us() - park_start;
            ctx->status[STATUS_PARK_US] = park_us;
            if (ctx->stop) {
                break;
            }
            // Round again rather than pulling. The ring may have filled while
            // this was parked -- an unpark is not a promise that there is
            // room -- and pulling here would drop exactly the block the wait
            // above exists to keep.
            continue;
        }
        // The other end of the park above: park_req has gone, so charge the
        // time it lasted and let the block through.
        if (ctx->service_mode && ctx->parked) {
            ctx->parked = false;
            ctx->status[STATUS_PARKED] = 0;
            park_us += audiopump_now_us() - ctx->park_enter_us;
            ctx->status[STATUS_PARK_US] = park_us;
        }

        const uint8_t *buffer = NULL;
        uint32_t length = 0;
        const uint64_t t0 = audiopump_now_us();
        // The lock, held for exactly one block pull and nothing else. The
        // control path takes the same lock around its final swap, so a
        // rewire can no longer land in the middle of a pull -- which is what
        // killed Phaser 5 times out of 5 on the desktop and panicked core 0
        // on the P4 at the same statement. Nobody has to call park() for
        // this; the lock is inside audioif, at the write.
        //
        // NOT held across the sink write below: that blocks for up to a DMA
        // block, and holding it there would make every knob wait for the
        // speaker instead of for the arithmetic.
        audioif_pump_lock_acquire_pump();
        // Re-read the tail INSIDE the lock. A retarget that swapped it is
        // holding this lock while it does, so either we see the whole swap or
        // none of it -- the registry entry the handoff page designs, which is
        // one word of state and one lock rather than a park.
        if (ctx->retarget_req) {
            ctx->retarget_req = false;
            if (ctx->retarget_loop >= 0) {
                ctx->loop = ctx->retarget_loop != 0;
                ctx->retarget_loop = -1;
            }
            ctx->sample_type = (const void *)
                ((mp_obj_base_t *)MP_OBJ_TO_PTR(ctx->sample))->type;
            // And what the NEW tail can hand back in one pull, because that is
            // the number the ring reserves room for above. A retarget from a
            // 512-byte Mixer to a 96 kB RawSample used to leave the reservation
            // at 512 and the write would then drop the block it could not fit
            // -- which on the service port was a stall and is now a drop the
            // wait was supposed to have prevented. One word, read out of the
            // base struct with no runtime call, inside the lock the swap used.
            ctx->max_block_bytes = (uint32_t)
                ((audiosample_base_t *)MP_OBJ_TO_PTR(ctx->sample))
                ->max_buffer_length;
        }
        // The net under the finaliser, and it has to be INSIDE the lock and
        // AFTER the retarget: a retarget to a different class legitimately
        // changes the type word, and checking before the swap was applied
        // called every live class swap a re-inited heap. A type word that is
        // no longer the one adopted, with no retarget pending, does mean the
        // heap holding the graph has been re-inited and re-used: stop, rather
        // than hand the DSP a pointer out of somebody else's object.
        if (ctx->sample_type != NULL
            && (const void *)((mp_obj_base_t *)MP_OBJ_TO_PTR(ctx->sample))->type
               != ctx->sample_type) {
            audioif_pump_lock_release_pump();
            error = 4;
            break;
        }
        // The timestamped events, applied at the top of the block and
        // nowhere else -- inside the lock the pump already holds, so an
        // insert from the interpreter either lands wholly before this pass or
        // wholly after it. Every event whose frame falls inside the block
        // about to be pulled goes now; one already behind goes now too and is
        // counted late. That is block-accurate and no better: the nodes
        // produce fixed-length blocks and splitting one is not a small
        // change. See docs/spikes/live-audio-path-events.md.
        if (ctx->events != MP_OBJ_NULL) {
            const uint64_t e0 = audiopump_now_us();
            (void)audiopump_events_apply(ctx->events, ctx->frames,
                ctx->block_frames);
            const uint64_t edt = audiopump_now_us() - e0;
            if (edt > event_us_max) {
                event_us_max = edt;
                ctx->status[STATUS_EVENT_US_MAX] = event_us_max;
            }
        }
        audioif_status_t status = audioif_sample_get(&ctx->source, false, 0,
            &buffer, &length, &result);
        audioif_pump_lock_release_pump();
        const uint64_t dt = audiopump_now_us() - t0;
        pull_us += dt;
        if (dt > max_pull_us) {
            max_pull_us = dt;
        }
        // A pull no longer raises: audioif's funnel returns GET_BUFFER_ERROR
        // and leaves a code in its fault register instead of longjmping off a
        // thread that has no interpreter state to allocate the exception
        // from. So a deinited node, a missing protocol or a file-backed
        // source in the graph all arrive HERE, as a number, and the pump
        // stops pulling that source and publishes why.
        if (status != AUDIOIF_STATUS_OK || result == AUDIOIF_BUFFER_ERROR) {
            error = 1;
            ctx->status[STATUS_FAULT] = audioif_pump_fault_get();
            break;
        }
        const uint32_t fault = audioif_pump_fault_get();
        if (fault != AUDIOIF_PUMP_FAULT_NONE) {
            // A node swallowed the error into silence -- every node in the
            // palette treats GET_BUFFER_ERROR from its source as "produce
            // zeros" -- so the result came back clean and the graph is quietly
            // wrong. That is the failure worth catching: stop, and say which.
            error = 5;
            ctx->status[STATUS_FAULT] = fault;
            break;
        }
        if (buffer == NULL) {
            error = 2;
            break;
        }
        // A stop that arrived while the pull was running must not be followed
        // by a sink write: the write blocks for up to sink_timeout_ms, and a
        // teardown waiting for this task is waiting exactly that long.
        if (ctx->stop) {
            break;
        }
        // The conversion, before anything looks at the block: the digest,
        // the ring, the tap and the sink all see what will be clocked, not
        // what the source happened to store. One branch when there is
        // nothing to convert, which is every graph audioif builds.
        if (ctx->conv != NULL && length) {
            length = audiopump_convert_block(ctx, buffer, length);
            buffer = ctx->conv;
        }
        for (uint32_t i = 0; i < length; i++) {
            digest ^= buffer[i];
            digest *= FNV_PRIME;
        }

        // The ring, if there is one. Single producer (here), single consumer
        // (audiopump.drain on the interpreter thread).
        //
        // The room was reserved above and the pump waited for it, so on a
        // threaded port with a driver that can sleep this branch cannot
        // overflow and STATUS_RING_OVF is 0 by construction. It is still
        // written, and it still counts, because two cases reach it: a ring
        // shorter than one block of this graph (which no wait can fix), and a
        // driver with no park_spin at all. Overrun drops the block rather than
        // overwriting what the consumer has not taken, so either of those is a
        // counter rather than a corruption.
        if (ctx->ring != NULL && length) {
            const uint32_t r = AUDIOPUMP_LOAD_ACQ(&ctx->ring_r);
            if (ctx->ring_w - r + length > ctx->ring_len) {
                ring_ovf++;
                ctx->status[STATUS_RING_OVF] = ring_ovf;
            } else {
                uint32_t at = ctx->ring_wpos;
                uint32_t first = ctx->ring_len - at;
                if (first > length) {
                    first = length;
                }
                memcpy(ctx->ring + at, buffer, first);
                if (length > first) {
                    memcpy(ctx->ring, buffer + first, length - first);
                }
                ctx->ring_wpos = (at + length) % ctx->ring_len;
                ctx->ring_w_total += length;
                // Release, and last: the bytes are in place before the
                // interpreter is told they are there.
                AUDIOPUMP_STORE_REL(&ctx->ring_w, ctx->ring_w + length);
                ctx->status[STATUS_RING_W] = ctx->ring_w_total;
            }
        }

        // The tap. One memcpy after the tail, out of the path: nothing the
        // reader does can stall the audio, and a reader that falls behind
        // loses old audio rather than new.
        if (ctx->tap != MP_OBJ_NULL && length) {
            audiopump_tap_write(ctx->tap, buffer, length);
        }

        // The clock. Published AFTER the block is in the ring and in the tap,
        // so now() never names a frame the pump has not finished producing.
        if (ctx->frame_bytes) {
            const uint32_t nframes = length / ctx->frame_bytes;
            if (nframes) {
                ctx->block_frames = nframes;
            }
            AUDIOPUMP_STORE_REL(&ctx->frames, ctx->frames + nframes);
            ctx->status[STATUS_FRAMES] = ctx->frames;
        }

        // The sink, whatever it is: a file descriptor on a desktop, an I2S
        // channel on a board. One call either way, and the engine never learns
        // which -- a write(2) and an i2s_channel_write are the same shape and
        // the difference between them was never the pump's business.
        if (ctx->to_sink && length && port->sink_write != NULL) {
            bool timed_out = false;
            const uint64_t s0 = audiopump_now_us();
            const uint32_t written = port->sink_write(buffer, length,
                ctx->sink_timeout_ms, &timed_out);
            sink_us += audiopump_now_us() - s0;
            if (timed_out) {
                sink_timeouts++;
            }
            sink_bytes += written;
        }

        // The pace, if one was asked for: hold until this block's moment.
        // Absolute deadlines against the run's start, so a late block is
        // caught up rather than accumulated -- the same reason the drum
        // machine's step timer uses a deadline and not a sleep. A board never
        // asks: the sink is the pace there.
        if (ctx->paced && ctx->pace_rate && ctx->frame_bytes
            && port->sleep_us != NULL) {
            const uint64_t due = wall_start
                + (uint64_t)ctx->frames * 1000000ULL / ctx->pace_rate;
            const uint64_t at = audiopump_now_us();
            if (due > at) {
                port->sleep_us(due - at);
            }
        }

        bytes += length;
        blocks++;
        spent++;
        // Publish as we go so a watching interpreter sees progress.
        ctx->status[STATUS_BLOCKS] = blocks;
        ctx->status[STATUS_BYTES] = bytes;
        ctx->status[STATUS_PULL_US] = pull_us;
        ctx->status[STATUS_SINK_US] = sink_us;
        ctx->status[STATUS_SINK_BYTES] = sink_bytes;
        ctx->status[STATUS_SINK_TIMEOUTS] = sink_timeouts;
        ctx->status[STATUS_MAX_PULL_US] = max_pull_us;
        const audioif_pump_lock_stats_t *lock = audioif_pump_lock_stats();
        ctx->status[STATUS_LOCK_PUMP_WAIT] = lock->pump_wait_us_max;
        ctx->status[STATUS_LOCK_CTRL_WAIT] = lock->ctrl_wait_us_max;
        ctx->status[STATUS_LOCK_CTRL_HELD] = lock->ctrl_held_us_max;
        ctx->status[STATUS_LOCK_PUMP_TAKES] = lock->pump_takes;
        ctx->status[STATUS_LOCK_CTRL_TAKES] = lock->ctrl_takes;
        if (port->sink_dma_bytes != NULL) {
            ctx->status[STATUS_DMA_BYTES] = port->sink_dma_bytes();
        }
        if (port->sink_rx_bytes != NULL) {
            ctx->status[STATUS_RX_BYTES] = port->sink_rx_bytes();
        }
        if (result == AUDIOIF_BUFFER_DONE) {
            if (!ctx->loop) {
                error = 3;
                break;
            }
            // The block that came back WITH the DONE has already been
            // digested, ringed, tapped and written above -- so the last block
            // of the sample is played, and only then is the tail rewound.
            // CircuitPython's i2s_fill_buffer does exactly this and in this
            // order; getting it the other way round clips the final block of
            // every lap.
            //
            // reset goes through the same resolved ops table the pull does,
            // so it never enters the runtime. A RawSample with one buffer
            // says DONE on every pull, which is why a looped RawSample is
            // this branch every single block and has to be this cheap.
            (void)audioif_sample_reset(&ctx->source, false, 0);
            result = AUDIOIF_BUFFER_MORE_DATA;
        }
    }

    ctx->acc_digest = digest;
    ctx->acc_blocks = blocks;
    ctx->acc_bytes = bytes;
    ctx->acc_error = error;
    ctx->acc_pull_us = pull_us;
    ctx->acc_sink_us = sink_us;
    ctx->acc_park_us = park_us;
    ctx->acc_max_pull_us = max_pull_us;
    ctx->acc_sink_bytes = sink_bytes;
    ctx->acc_sink_timeouts = sink_timeouts;
    ctx->acc_parks = parks;
    ctx->acc_ring_ovf = ring_ovf;
    ctx->acc_ring_waits = ring_waits;
    ctx->acc_ring_wait_us = ring_wait_us;
    ctx->acc_event_us_max = event_us_max;
    ctx->acc_result = result;
    return why;
}

// What every threaded port runs: one call, start to finish.
static void audiopump_run(audiopump_ctx_t *ctx) {
    audiopump_run_begin(ctx);
    (void)audiopump_run_blocks(ctx, UINT64_MAX);
    audiopump_run_end(ctx);
}

// --- the MicroPython side, all of it on the interpreter thread ------------

// The pump's pointers are invisible to the collector, so everything it
// touches is rooted here: the graph tail, the status bytearray, the ring and
// the guard object whose finaliser is the soft-reset teardown.
// See the spike notes, "what the pump holds".
// 4 is the event queue and 5 the tap: the pump holds a raw pointer to each,
// and a queue holding a Note nobody else references any more is exactly the
// case the collector would otherwise be right about.
// 6 is the conversion scratch: the loop holds a raw pointer into it and the
// only other reference is a field of a C object, so it is rooted here beside
// the ring for the same reason the ring is.
MP_REGISTER_ROOT_POINTER(mp_obj_t audiopump_held[7]);
#define AUDIOPUMP_HELD_CONVERT (6)
#define AUDIOPUMP_HELD_EVENTS (4)
#define AUDIOPUMP_HELD_TAP (5)

static void audiopump_prepare(mp_obj_t sample, mp_obj_t blocks_in,
    mp_obj_t status_in, mp_obj_t ring_in, audiopump_ctx_t *ctx) {
    mp_buffer_info_t info;
    mp_get_buffer_raise(status_in, &info, MP_BUFFER_WRITE);
    if (info.len < AUDIOPUMP_STATUS_BYTES) {
        mp_raise_ValueError(MP_ERROR_TEXT("status too small"));
    }
    memset(info.buf, 0, AUDIOPUMP_STATUS_BYTES);

    memset(ctx, 0, sizeof(*ctx));

    if (ring_in != MP_OBJ_NULL && ring_in != mp_const_none) {
        mp_buffer_info_t ring;
        mp_get_buffer_raise(ring_in, &ring, MP_BUFFER_WRITE);
        if (ring.len < 64) {
            mp_raise_ValueError(MP_ERROR_TEXT("ring too small"));
        }
        ctx->ring = ring.buf;
        ctx->ring_len = (uint32_t)ring.len;
    }

    // This is the call that would longjmp on a thread with no interpreter,
    // so it happens here and its result is what the loop carries.
    const audiosample_p_t *protocol = mp_proto_get_or_throw(
        MP_QSTR_protocol_audiosample, sample);
    audiosample_check_for_deinit(MP_OBJ_TO_PTR(sample));
    audioif_pump_fault_clear();

    ctx->protocol = protocol;
    ctx->sample = sample;
    ctx->sample_type = (const void *)((mp_obj_base_t *)MP_OBJ_TO_PTR(sample))->type;
    // The clock's units, off the graph rather than guessed. `block_frames` is
    // only a seed: the loop replaces it with the length of the block it
    // actually got, every block, so a graph whose tail produces something
    // other than its declared maximum corrects itself on the first pull.
    audiosample_base_t *base = MP_OBJ_TO_PTR(sample);
    // What one pull can hand back. The service loop needs it to know whether
    // the ring has room for another block BEFORE it pulls one, because after
    // the pull there is nowhere to put what it got.
    ctx->max_block_bytes = (uint32_t)base->max_buffer_length;
    ctx->frame_bytes = (uint32_t)base->channel_count
        * (uint32_t)(base->bits_per_sample / 8);
    ctx->block_frames = ctx->frame_bytes
        ? base->max_buffer_length / ctx->frame_bytes : 0;
    if (ctx->block_frames == 0) {
        ctx->block_frames = 1;
    }
    // Whatever was attached stays attached across a respawn: the roots are
    // the authority, not the context, which prepare() has just memset.
    ctx->events = MP_STATE_VM(audiopump_held)[AUDIOPUMP_HELD_EVENTS];
    ctx->tap = MP_STATE_VM(audiopump_held)[AUDIOPUMP_HELD_TAP];
    ctx->status = info.buf;
    // Read the clock once here, on the interpreter thread, before any pump
    // thread exists. On Windows that is what latches QueryPerformanceFrequency
    // -- lazily, from whichever thread asks first -- and doing it here means
    // the pump never races the interpreter for it.
    (void)audiopump_now_us();
    ctx->blocks = (uint64_t)mp_obj_get_int(blocks_in);
    ctx->sink_timeout_ms = 200;
    ctx->source.ops = &audiopump_ops;
    ctx->source.context = ctx;
    ctx->source.info = NULL;

    MP_STATE_VM(audiopump_held)[0] = sample;
    MP_STATE_VM(audiopump_held)[1] = status_in;
    MP_STATE_VM(audiopump_held)[2] = ring_in;
}

// The `sink=` argument, and it is one argument with two meanings because the
// call sites have always used it that way: a PATH on a desktop, where the
// driver opens a file and the loop only ever writes to it, and TRUE on a
// board, where the sink is the I2S channel the driver's own Python surface
// already opened. Neither of them is a thing this file knows how to make.
static void audiopump_open_sink(mp_obj_t sink) {
    if (sink == MP_OBJ_NULL || sink == mp_const_none) {
        return;
    }
    const audioif_port_ops_t *port = audioif_port();
    if (mp_obj_is_str(sink)) {
        if (port->sink_open == NULL) {
            mp_raise_ValueError(MP_ERROR_TEXT(
                "this build's driver has no file sink"));
        }
        if (!port->sink_open(mp_obj_str_get_str(sink))) {
            mp_raise_OSError(MP_ENOENT);
        }
        audiopump_ctx.to_sink = true;
        return;
    }
    if (mp_obj_is_true(sink)) {
        if (port->sink_ready == NULL || !port->sink_ready()) {
            mp_raise_ValueError(MP_ERROR_TEXT("no i2s sink open"));
        }
        audiopump_ctx.to_sink = true;
    }
}

static void audiopump_close_sink(void) {
    const audioif_port_ops_t *port = audioif_port();
    if (port->sink_close != NULL) {
        port->sink_close();
    }
    audiopump_ctx.to_sink = false;
}

static mp_obj_t audiopump_pull(size_t n_args, const mp_obj_t *args) {
    // The one global context, so drain() and park() mean the same thing
    // whether the loop is on this thread or the pump's.
    audiopump_ctx_t *ctx = &audiopump_ctx;
    audiopump_prepare(args[0], args[1], args[2],
        n_args > 4 ? args[4] : MP_OBJ_NULL, ctx);
    if (n_args > 3) {
        audiopump_open_sink(args[3]);
    }
    audiopump_run(ctx);
    audiopump_close_sink();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiopump_pull_obj, 3, 5,
    audiopump_pull);

static mp_obj_t audiopump_reset_graph(mp_obj_t sample) {
    const audiosample_p_t *protocol = mp_proto_get_or_throw(
        MP_QSTR_protocol_audiosample, sample);
    protocol->reset_buffer(sample, false, 0);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_reset_graph_obj,
    audiopump_reset_graph);

// What the graph says it is, so the caller can open a sink that matches it
// without guessing. Every field is read straight out of audiosample_base_t.
static mp_obj_t audiopump_info(mp_obj_t sample) {
    (void)mp_proto_get_or_throw(MP_QSTR_protocol_audiosample, sample);
    audiosample_base_t *base = MP_OBJ_TO_PTR(sample);
    mp_obj_t items[6] = {
        mp_obj_new_int_from_uint(base->sample_rate),
        mp_obj_new_int_from_uint(base->channel_count),
        mp_obj_new_int_from_uint(base->bits_per_sample),
        mp_obj_new_int_from_uint(base->max_buffer_length),
        mp_obj_new_bool(base->samples_signed),
        mp_obj_new_bool(base->single_buffer),
    };
    return mp_obj_new_tuple(6, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_info_obj, audiopump_info);

// --- the pump thread ------------------------------------------------------
//
// There is no thread in this file. The driver makes one and calls this on it;
// what it is -- a FreeRTOS task pinned to the core the interpreter is not on,
// a _beginthreadex CRT thread, a pthread -- is the driver's business and the
// reasons it chose that are written down beside its own code.

static void audiopump_entry(void *arg) {
    audiopump_run((audiopump_ctx_t *)arg);
}

// --- teardown, and the thing that makes it happen on a soft reset ---------
//
// The board half found that a soft reset frees the graph out from under a
// running pump and nothing notices: MicroPython re-inits the GC over the same
// region, the FreeRTOS task is a C task and keeps pulling, and the audio goes
// on until something reuses the memory. A pump that survives the heap that
// owns its graph is worse than one that crashes.
//
// The esp32 port has no hook a user C module can register in its soft-reset
// path -- main.c's soft_reset_exit calls a fixed list of `_deinit()`s and
// there is no MICROPY_BOARD_END_SOFT_RESET on this port. But two lines above
// that list it calls `gc_sweep_all()`, which runs `__del__` on EVERY object
// that has one, reachable or not (py/gc.c:604, gc_sweep_run_finalisers).
// So the cheapest robust mechanism is an object with a finaliser: it is
// rooted here so an ordinary collection never touches it, and a soft reset
// finalises it anyway. No port patch.
//
// Finalisers run before gc_sweep_free_blocks, so the graph is still intact
// while this runs -- and audioif has no finalisers of its own outside
// audiomp3, so nothing can deinit a node ahead of us.

// `release_guard` is true only when the finaliser itself is calling: the guard
// object is being swept, so the root must be dropped or it dangles into a heap
// that is about to be re-inited -- and `arm_guard` would then see a non-NULL
// slot and never make a live one, leaving the NEXT soft reset with nothing to
// finalise. An explicit shutdown() keeps it: the object is still alive and
// still rooted, and unrooting it there would leave it unreachable but not yet
// swept, so the next gc.collect() would finalise it and tear down whatever
// pump had been spawned in between. Two opposite failures, one flag.
static void audiopump_teardown(bool release_guard) {
    audiopump_ctx.stop = true;
    audiopump_ctx.park_req = false;
    audioif_pump_set_active(false);
    const audioif_port_ops_t *port = audioif_port();
    if (audiopump_live && !audiopump_ctx.service_mode) {
        if (port->thread_wake != NULL) {
            port->thread_wake();
        }
        // The loop checks `stop` at the block boundary and again between the
        // pull and the sink write, so the longest it can be away is one pull.
        // The driver's sleep, not mp_hal_delay_ms: this runs inside a
        // finaliser under the GC mutex, and mp_hal_delay_ms ends in
        // mp_handle_pending, which can raise.
        uint32_t waited_ms = 0;
        while (!audiopump_ctx.finished && waited_ms < 2000) {
            audiopump_wait_us(10000);
            waited_ms += 10;
        }
        if (port->thread_release != NULL) {
            port->thread_release();
        }
    }
    // Nothing to join in service mode: the loop only ever runs inside a
    // service() call, and this is not one. `stop` above means the next
    // service() leaves at once.
    audiopump_live = false;
    if (port->sink_close != NULL) {
        port->sink_close();
    }
    // Whatever the driver holds that the VM cannot free for it: the I2S
    // channel on a board, the pace timer on Windows.
    if (port->teardown != NULL) {
        port->teardown();
    }
    // Everything in here points into a heap that is about to be re-inited.
    memset(&audiopump_ctx, 0, sizeof(audiopump_ctx));
    MP_STATE_VM(audiopump_held)[0] = MP_OBJ_NULL;
    MP_STATE_VM(audiopump_held)[1] = MP_OBJ_NULL;
    MP_STATE_VM(audiopump_held)[2] = MP_OBJ_NULL;
    MP_STATE_VM(audiopump_held)[AUDIOPUMP_HELD_CONVERT] = MP_OBJ_NULL;
    // The queue and the tap go with everything else. A queue holds Notes and
    // samples out of a heap that is about to be re-inited, and holding it
    // past a soft reset would be the same bug as holding the graph.
    MP_STATE_VM(audiopump_held)[AUDIOPUMP_HELD_EVENTS] = MP_OBJ_NULL;
    MP_STATE_VM(audiopump_held)[AUDIOPUMP_HELD_TAP] = MP_OBJ_NULL;
    if (release_guard) {
        MP_STATE_VM(audiopump_held)[3] = MP_OBJ_NULL;
    }
}

typedef struct {
    mp_obj_base_t base;
} audiopump_guard_obj_t;

static mp_obj_t audiopump_guard_del(mp_obj_t self_in) {
    (void)self_in;
    audiopump_teardown(true);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_guard_del_obj, audiopump_guard_del);

static const mp_rom_map_elem_t audiopump_guard_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&audiopump_guard_del_obj) },
};
static MP_DEFINE_CONST_DICT(audiopump_guard_locals,
    audiopump_guard_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    audiopump_guard_type,
    MP_QSTR_Guard,
    MP_TYPE_FLAG_NONE,
    locals_dict, &audiopump_guard_locals
    );

// Called by anything that takes ownership of hardware or a graph. Rooted, so
// a normal gc.collect() never finalises it; unrooted objects are finalised by
// gc_sweep_all() regardless, which is the whole point.
void audiopump_arm_guard(void) {
    if (MP_STATE_VM(audiopump_held)[3] == MP_OBJ_NULL) {
        MP_STATE_VM(audiopump_held)[3] = MP_OBJ_FROM_PTR(
            mp_obj_malloc_with_finaliser(audiopump_guard_obj_t,
                &audiopump_guard_type));
    }
}

bool audiopump_is_running(void) {
    return audiopump_live && !audiopump_ctx.finished;
}

static mp_obj_t audiopump_running(void) {
    return mp_obj_new_bool(audiopump_is_running());
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_running_obj, audiopump_running);

// The explicit form of what the finaliser does. Safe to call twice.
static mp_obj_t audiopump_shutdown(void) {
    audiopump_teardown(false);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_shutdown_obj, audiopump_shutdown);

// A file-backed source cannot be pulled by a pump: it reads through the VFS
// from inside get_buffer, which re-enters the interpreter, and it raises
// there. audioif's WaveFile and MP3Decoder now refuse it from the inside --
// they publish AUDIOIF_PUMP_FAULT_UNPUMPABLE and go silent rather than
// crash -- but a graph that is nothing BUT a file is worth refusing at the
// door, with a sentence, instead of playing silence and leaving a number to
// be looked up.
//
// This catches the tail only. A file source sits at the HEAD of a graph, and
// the audiosample protocol has no "what is behind you" accessor to walk, so a
// deep one is caught at the first block instead of at the door. That is the
// honest limit of this check and it is written down in the notes.
static void audiopump_refuse_unpumpable(mp_obj_t sample) {
    const qstr name = mp_obj_get_type(sample)->name;
    if (name == MP_QSTR_WaveFile || name == MP_QSTR_MP3Decoder) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "a file-backed source cannot be pumped; it reads through the VFS "
            "inside the pull. Fill a ring from the interpreter instead."));
    }
}

static int audiopump_launch(int core, int prio, int stack, bool psram);

static mp_obj_t audiopump_spawn(size_t n_args, const mp_obj_t *pos_args,
    mp_map_t *kw_args) {
    enum { ARG_sample, ARG_blocks, ARG_status, ARG_sink, ARG_ring, ARG_core,
           ARG_prio, ARG_stack, ARG_psram, ARG_timeout_ms, ARG_pace, ARG_loop };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_sample,     MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_blocks,     MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_status,     MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_sink,       MP_ARG_OBJ,  { .u_obj = mp_const_none } },
        { MP_QSTR_ring,       MP_ARG_OBJ,  { .u_obj = mp_const_none } },
        { MP_QSTR_core,       MP_ARG_INT,  { .u_int = -1 } },
        { MP_QSTR_prio,       MP_ARG_INT,  { .u_int = 4 } },
        { MP_QSTR_stack,      MP_ARG_INT,  { .u_int = 16384 } },
        { MP_QSTR_psram,      MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_timeout_ms, MP_ARG_INT,  { .u_int = 200 } },
        // unix only; see `paced` in the context struct.
        { MP_QSTR_pace,       MP_ARG_BOOL, { .u_bool = false } },
        // Rewind the tail rather than end when it says DONE -- what
        // audiobusio.I2SOut(...).play(sample, loop=True) means.
        { MP_QSTR_loop,       MP_ARG_BOOL, { .u_bool = false } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed),
        allowed, args);

    // One global pump, so a second spawn would silently orphan the first --
    // the task that is already pulling, and the graph behind it. Refuse, and
    // say what to call. shutdown() is the door out; it is also what the soft
    // reset finaliser calls.
    if (audiopump_live) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "a pump is already spawned; call audiopump.shutdown() first"));
    }

    audiopump_refuse_unpumpable(args[ARG_sample].u_obj);
    audiopump_arm_guard();
    audiopump_prepare(args[ARG_sample].u_obj, args[ARG_blocks].u_obj,
        args[ARG_status].u_obj, args[ARG_ring].u_obj, &audiopump_ctx);
    audiopump_ctx.sink_timeout_ms = (uint32_t)args[ARG_timeout_ms].u_int;
    // From here on audioif knows a pump exists, so the file-backed sources
    // refuse to be pulled and the funnel reports rather than raises. Set
    // BEFORE the thread is created, cleared after it has gone, both from this
    // thread -- so there is a happens-before at each end and no flag race.
    audioif_pump_lock_stats_reset();
    audioif_pump_set_active(true);

    audiopump_ctx.loop = args[ARG_loop].u_bool;
    audiopump_ctx.retarget_loop = -1;
    audiopump_open_sink(args[ARG_sink].u_obj);
    if (args[ARG_pace].u_bool) {
        audiopump_ctx.paced = true;
        audiopump_ctx.pace_rate =
            ((audiosample_base_t *)MP_OBJ_TO_PTR(args[ARG_sample].u_obj))
            ->sample_rate;
    }

    return mp_obj_new_int(audiopump_launch(args[ARG_core].u_int,
        args[ARG_prio].u_int, args[ARG_stack].u_int, args[ARG_psram].u_bool));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audiopump_spawn_obj, 3, audiopump_spawn);

// Everything after the context is filled in: the one place the thread is
// started and the one place service mode is decided. Both spawn() and
// audiopump_c_spawn() end here, so a device binding cannot drift from the
// Python surface.
static int audiopump_launch(int core, int prio, int stack, bool psram) {
    const audioif_port_ops_t *port = audioif_port();
    if (port->thread_start == NULL) {
        // The shape a port with no thread gets -- and, just as usefully, the
        // shape a build gets when its driver did NOT bind. spawn() adopts the
        // tail, publishes the first status words and returns. Not one block
        // has been pulled. The loop advances only when audiopump.service() is
        // called -- from the audiodev driver's timer in a browser, from a
        // probe's while loop here.
        //
        // -2, not -1: a caller that prints what spawn() returned should be
        // able to tell "no core affinity" from "no thread at all", and on a
        // build that should have had one that number is the symptom.
        audiopump_ctx.service_mode = true;
        audiopump_live = true;
        audiopump_run_begin(&audiopump_ctx);
        return -2;
    }

    const audioif_port_thread_cfg_t cfg = {
        .core = core,
        .prio = prio,
        .stack = stack,
        .psram = psram,
    };
    int where = -1;
    if (!port->thread_start(audiopump_entry, &audiopump_ctx, &cfg, &where)) {
        audioif_pump_set_active(false);
        mp_raise_OSError(MP_ENOMEM);
    }
    audiopump_live = true;
    return where;
}

int audiopump_c_spawn(mp_obj_t sample, mp_obj_t status, uint64_t blocks,
    mp_obj_t sink, bool loop, bool pace, int core, uint32_t timeout_ms,
    const audiopump_convert_t *convert) {
    if (audiopump_live) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "a pump is already spawned; call audiopump.shutdown() first"));
    }
    audiopump_refuse_unpumpable(sample);
    audiopump_arm_guard();
    // Clamped, not promoted: audiopump_prepare reads this with
    // mp_obj_get_int, which is 31 bits on every 32-bit target we ship, and a
    // "play for ever" caller passing UINT64_MAX would get OverflowError
    // instead of audio. 2^30 blocks is 68 years at a 5.3 ms block.
    if (blocks > 0x3FFFFFFFULL) {
        blocks = 0x3FFFFFFFULL;
    }
    audiopump_prepare(sample, mp_obj_new_int((mp_int_t)blocks), status,
        MP_OBJ_NULL, &audiopump_ctx);
    audiopump_ctx.sink_timeout_ms = timeout_ms;
    // The conversion, if the caller asked for one. AFTER prepare(), which
    // memsets the context, and before the thread exists -- sizing the
    // scratch and taking its pointer are both interpreter-thread work.
    if (convert != NULL && convert->scratch != MP_OBJ_NULL
        && convert->scratch != mp_const_none) {
        mp_buffer_info_t scratch;
        mp_get_buffer_raise(convert->scratch, &scratch, MP_BUFFER_WRITE);
        const uint32_t in_frame = (uint32_t)convert->channels
            * (uint32_t)(convert->bits / 8);
        const uint32_t frames = in_frame
            ? audiopump_ctx.max_block_bytes / in_frame : 0;
        if (in_frame == 0 || frames == 0 || scratch.len < (size_t)frames * 4) {
            mp_raise_ValueError(MP_ERROR_TEXT(
                "convert scratch too small for one block"));
        }
        audiopump_ctx.conv = scratch.buf;
        audiopump_ctx.conv_len = (uint32_t)scratch.len;
        audiopump_ctx.conv_bits = convert->bits;
        audiopump_ctx.conv_channels = convert->channels;
        audiopump_ctx.conv_signed = convert->is_signed;
        // The clock counts what LEAVES, not what arrived: an 8-bit mono
        // frame is one byte on the way in and four on the way out, and
        // audiopump.now() names output frames. Same for the service loop's
        // idea of how much room one block needs.
        audiopump_ctx.frame_bytes = 4;
        audiopump_ctx.max_block_bytes = frames * 4;
        MP_STATE_VM(audiopump_held)[AUDIOPUMP_HELD_CONVERT] = convert->scratch;
    }
    audioif_pump_lock_stats_reset();
    audioif_pump_set_active(true);
    audiopump_ctx.loop = loop;
    audiopump_ctx.retarget_loop = -1;
    audiopump_open_sink(sink);
    if (pace) {
        audiopump_ctx.paced = true;
        audiopump_ctx.pace_rate =
            ((audiosample_base_t *)MP_OBJ_TO_PTR(sample))->sample_rate;
    }
    return audiopump_launch(core, 4, 16384, false);
}

bool audiopump_c_join(uint32_t timeout_ms) {
    if (!audiopump_live) {
        return true;
    }
    if (audiopump_ctx.service_mode) {
        // There is no thread to wait for and join() must not quietly become
        // the driver: a join that pulled the rest of the graph itself would
        // make every "the timer was late" measurement on this port a
        // measurement of join(). So it reports, and the caller keeps calling
        // service().
        if (!audiopump_ctx.finished) {
            return false;
        }
    } else {
        // Against the CLOCK, not against a count of how many sleeps were
        // asked for. mp_hal_delay_ms(2) is Sleep(2) on Windows and the
        // scheduler's tick there is 15.6 ms, so a hundred of them is a second
        // and a half: join(200) measured 1 166 547 us on that port and 200 156
        // on unix, off the same line of code. A caller who asked for 200 ms
        // and waited for 1.2 s has been given the wrong answer to the only
        // question join() answers.
        const uint64_t deadline = audiopump_now_us()
            + (uint64_t)timeout_ms * 1000ULL;
        while (!audiopump_ctx.finished && audiopump_now_us() < deadline) {
            mp_hal_delay_ms(2);
        }
        if (!audiopump_ctx.finished) {
            return false;
        }
        // On a board the task is parked in vTaskSuspend by now and deleting
        // it from here is the supported direction; on a desktop this is the
        // pthread_join or the WaitForSingleObject. Either way it is the
        // driver's call and it happens after the loop has been seen to end.
        const audioif_port_ops_t *port = audioif_port();
        if (port->thread_release != NULL) {
            port->thread_release();
        }
    }
    audiopump_live = false;
    const audioif_port_ops_t *port = audioif_port();
    if (port->sink_close != NULL) {
        port->sink_close();
    }
    audiopump_ctx.to_sink = false;
    audioif_pump_set_active(false);
    MP_STATE_VM(audiopump_held)[0] = MP_OBJ_NULL;
    MP_STATE_VM(audiopump_held)[1] = MP_OBJ_NULL;
    MP_STATE_VM(audiopump_held)[2] = MP_OBJ_NULL;
    MP_STATE_VM(audiopump_held)[AUDIOPUMP_HELD_CONVERT] = MP_OBJ_NULL;
    return true;
}

static mp_obj_t audiopump_join(size_t n_args, const mp_obj_t *args) {
    const uint32_t timeout_ms = n_args > 0
        ? (uint32_t)mp_obj_get_int(args[0]) : 30000;
    return mp_obj_new_bool(audiopump_c_join(timeout_ms));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiopump_join_obj, 0, 1,
    audiopump_join);

void audiopump_c_stop(void) {
    audiopump_ctx.stop = true;
    audiopump_ctx.park_req = false;
    const audioif_port_ops_t *port = audioif_port();
    if (port->thread_wake != NULL) {
        port->thread_wake();
    }
}

static mp_obj_t audiopump_stop(void) {
    audiopump_c_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_stop_obj, audiopump_stop);

uint64_t audiopump_c_status(unsigned index) {
    if (audiopump_ctx.status == NULL || index >= AUDIOPUMP_STATUS_WORDS) {
        return 0;
    }
    return audiopump_ctx.status[index];
}

bool audiopump_c_parked(void) {
    return audiopump_ctx.parked;
}

bool audiopump_c_service_mode(void) {
    return audiopump_ctx.service_mode;
}

void audiopump_c_service(uint64_t budget) {
    if (!audiopump_ctx.service_mode || !audiopump_live
        || audiopump_ctx.finished) {
        return;
    }
    if (audiopump_run_blocks(&audiopump_ctx, budget ? budget : UINT64_MAX)
        == AUDIOPUMP_SERVICE_DONE) {
        audiopump_run_end(&audiopump_ctx);
    }
}

// --- the park protocol ----------------------------------------------------
//
// park() returns True when the pump is sitting at a block boundary and will
// not touch the graph again until unpark(). Everything the handoff page
// calls a rewire goes between the two.

bool audiopump_c_park(uint32_t timeout_us) {
    if (!audiopump_live || audiopump_ctx.finished) {
        return true;
    }
    audiopump_ctx.park_req = true;
    // Wake it, because it may be asleep on a full output ring and nothing else
    // is going to drain that ring while this call is waiting for it to park.
    // Without this, park() on a back-pressured pump waits its whole timeout
    // and then reports failure -- which is every retarget and every teardown.
    {
        const audioif_port_ops_t *port = audioif_port();
        if (port->thread_wake != NULL) {
            port->thread_wake();
        }
    }
    if (audiopump_ctx.service_mode) {
        // The loop is not running -- this call IS the only thread -- so the
        // pump is already at a block boundary by construction and park() is a
        // store. The wait below would spin until the timeout and then report
        // success anyway; saying so straight is the same answer without the
        // 200 ms.
        return true;
    }
    uint32_t waited = 0;
    while (!audiopump_ctx.parked && !audiopump_ctx.finished
           && waited < timeout_us) {
        mp_hal_delay_us(20);
        waited += 20;
    }
    return audiopump_ctx.parked || audiopump_ctx.finished;
}

static mp_obj_t audiopump_park(size_t n_args, const mp_obj_t *args) {
    const uint32_t timeout_us = n_args > 0
        ? (uint32_t)mp_obj_get_int(args[0]) : 100000;
    return mp_obj_new_bool(audiopump_c_park(timeout_us));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiopump_park_obj, 0, 1,
    audiopump_park);

// --- the port with no thread ----------------------------------------------
//
// The same loop, entered from the interpreter. `max_blocks` caps how many
// blocks one call produces; 0 means "as many as the output ring has room
// for", which is the natural budget because the ring is what paces the pump
// here.
//
// Returns ONE small int, `blocks << 2 | why`, and the packing is not
// premature cleverness: on this port service() IS the pull's entry point, so
// micropython.heap_lock() around it has to hold, and a two-element tuple is
// an allocation per call. A small int is not. Unpack it with
// `why = r & 3; blocks = r >> 2`.
//
// On a threaded port it is a no-op that returns MORE with no blocks: the
// thread is already doing this, and a driver written against the wasm shape
// should not have to know which port it is on.
static mp_obj_t audiopump_service(size_t n_args, const mp_obj_t *args) {
    uint64_t budget = n_args > 0 ? (uint64_t)mp_obj_get_int(args[0]) : 0;
    if (budget == 0) {
        budget = UINT64_MAX;
    }
    int why = AUDIOPUMP_SERVICE_MORE;
    uint64_t before = audiopump_ctx.acc_blocks;
    if (audiopump_ctx.service_mode) {
        if (audiopump_live && !audiopump_ctx.finished) {
            why = audiopump_run_blocks(&audiopump_ctx, budget);
            if (why == AUDIOPUMP_SERVICE_DONE) {
                audiopump_run_end(&audiopump_ctx);
            }
        } else {
            why = AUDIOPUMP_SERVICE_DONE;
            before = audiopump_ctx.acc_blocks;
        }
    }
    const mp_uint_t done = (mp_uint_t)(audiopump_ctx.acc_blocks - before);
    return MP_OBJ_NEW_SMALL_INT((done << AUDIOPUMP_SERVICE_SHIFT) | why);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiopump_service_obj, 0, 1,
    audiopump_service);

// True where spawn() puts the loop on a thread of its own. False on wasm,
// where audiopump.service() is the driver -- and false on any build whose
// driver did not bind, which is the point: one question, asked once, rather
// than every caller sniffing for i2s_start or sys.platform.
static mp_obj_t audiopump_threaded(void) {
    return mp_obj_new_bool(audioif_port_threaded());
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_threaded_obj, audiopump_threaded);

// True where a full output ring makes the pump WAIT rather than drop the
// block. The question a desktop driver has to ask before it decides whether to
// park the pump between ticks: with back-pressure the ring is the pace and
// parking is pure cost, and without it a free-running pump loses audio.
//
// True on a threaded port whose driver has a wait a wake can end, and true in
// service mode, where the loop hands the thread back on a full ring and the
// caller's next service() is the wake. False only on a threaded build with no
// park_spin -- which is a driver that is not finished, and now says so.
static mp_obj_t audiopump_backpressure(void) {
    const audioif_port_ops_t *port = audioif_port();
    return mp_obj_new_bool(port->thread_start == NULL
        || port->park_spin != NULL);
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_backpressure_obj,
    audiopump_backpressure);

// WHICH driver bound: "esp32", "pthread", "win32" or "none". The one line that
// makes the split checkable from Python -- a build that should have a thread
// and says "none" has a link-order problem, not a mystery.
static mp_obj_t audiopump_driver(void) {
    return mp_obj_new_str_from_cstr(audioif_port_name());
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_driver_obj, audiopump_driver);

// Point the pump at a different tail. The handoff page's registry entry, cut
// down to the one thing the spike needs: a helper that swaps the effect class
// parks, builds the new graph, retargets, unparks. The protocol lookup and
// the deinit check happen here, on the interpreter thread, exactly as they do
// in spawn() -- both of them raise, and the pump thread has nothing to raise
// from.
void audiopump_c_retarget(mp_obj_t sample, int loop) {
    // No park. Everything that can raise or allocate happens first, on this
    // thread: the protocol lookup, the deinit check, the refusal. Then the
    // lock, then three stores, then the unlock -- and the pump either sees
    // the whole new tail or the whole old one.
    //
    // This is the handoff page's registry entry. The pump holds the entry,
    // not the tail object, so a Component that replaces its own `.output`
    // (Phaser._install_cascade does, on every macro-5 move) can say so
    // without the pump ever pulling the orphan it left behind.
    audiopump_refuse_unpumpable(sample);
    const audiosample_p_t *protocol = mp_proto_get_or_throw(
        MP_QSTR_protocol_audiosample, sample);
    audiosample_check_for_deinit(MP_OBJ_TO_PTR(sample));
    MP_STATE_VM(audiopump_held)[0] = sample;

    audioif_pump_lock_acquire();
    audiopump_ctx.protocol = protocol;
    audiopump_ctx.sample = sample;
    // The loop flag belongs to the tail, not to the pump's whole lifetime.
    // It is set at spawn and it used to STAY set, which is wrong the moment
    // the tail is swapped: a root Mixer replaced by the one client still
    // sounding is a tail whose loop is that client's, and a looping client
    // left alone on a live pump was stopping at the end of its first lap
    // because the flag was still the one the FIRST tail was spawned with.
    // -1 leaves it alone, for a caller that is only swapping the graph.
    audiopump_ctx.retarget_loop = loop;
    // The type word is re-read by the loop, inside the same lock, so the
    // soft-reset guard cannot see a half-updated pair.
    audiopump_ctx.retarget_req = true;
    audioif_pump_lock_release();
}

static mp_obj_t audiopump_retarget(size_t n_args, const mp_obj_t *pos_args,
    mp_map_t *kw_args) {
    enum { ARG_sample, ARG_loop };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_sample, MP_ARG_REQUIRED | MP_ARG_OBJ,
          { .u_obj = MP_OBJ_NULL } },
        // None -- the default -- leaves the flag as spawn() set it, which is
        // what every caller before this argument existed wanted.
        { MP_QSTR_loop,   MP_ARG_OBJ | MP_ARG_KW_ONLY,
          { .u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed),
        allowed, args);
    const int loop = args[ARG_loop].u_obj == mp_const_none
        ? -1 : (mp_obj_is_true(args[ARG_loop].u_obj) ? 1 : 0);
    audiopump_c_retarget(args[ARG_sample].u_obj, loop);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audiopump_retarget_obj, 1,
    audiopump_retarget);

void audiopump_c_unpark(void) {
    audiopump_ctx.park_req = false;
    const audioif_port_ops_t *port = audioif_port();
    if (port->thread_wake != NULL) {
        port->thread_wake();
    }
}

static mp_obj_t audiopump_unpark(void) {
    audiopump_c_unpark();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_unpark_obj, audiopump_unpark);

// --- the ring's consumer --------------------------------------------------

static mp_obj_t audiopump_drain(mp_obj_t out_in) {
    mp_buffer_info_t out;
    mp_get_buffer_raise(out_in, &out, MP_BUFFER_WRITE);
    audiopump_ctx_t *ctx = &audiopump_ctx;
    if (ctx->ring == NULL) {
        return MP_OBJ_NEW_SMALL_INT(0);
    }
    uint32_t available = AUDIOPUMP_LOAD_ACQ(&ctx->ring_w) - ctx->ring_r;
    uint32_t take = (uint32_t)(available > out.len ? out.len : available);
    if (take == 0) {
        return MP_OBJ_NEW_SMALL_INT(0);
    }
    uint32_t at = ctx->ring_rpos;
    uint32_t first = ctx->ring_len - at;
    if (first > take) {
        first = take;
    }
    memcpy(out.buf, ctx->ring + at, first);
    if (take > first) {
        memcpy((uint8_t *)out.buf + first, ctx->ring, take - first);
    }
    uint64_t digest = ctx->drain_digest ? ctx->drain_digest : FNV_OFFSET;
    const uint8_t *bytes = out.buf;
    for (uint32_t i = 0; i < take; i++) {
        digest ^= bytes[i];
        digest *= FNV_PRIME;
    }
    ctx->drain_digest = digest;
    ctx->ring_rpos = (at + take) % ctx->ring_len;
    ctx->ring_r_total += take;
    // Release: the bytes are out before the space is given back, so the pump
    // cannot land on top of them.
    AUDIOPUMP_STORE_REL(&ctx->ring_r, ctx->ring_r + take);
    ctx->status[STATUS_RING_R] = ctx->ring_r_total;
    ctx->status[STATUS_DRAIN_DIGEST] = digest;
    // And this is the other half of the back-pressure: the pump is asleep
    // because this ring was full, and the space has just been given back.
    // AFTER the release store, so the pump cannot wake, look, and find the
    // room still missing. Guarded on the flag rather than done unconditionally
    // because a drain runs every tick and a wake is a syscall.
    if (ctx->ring_wait) {
        const audioif_port_ops_t *port = audioif_port();
        if (port->thread_wake != NULL) {
            port->thread_wake();
        }
    }
    return mp_obj_new_int_from_uint(take);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopump_drain_obj, audiopump_drain);

// --- the clock, the queue and the tap -------------------------------------

// Frames the pump has PULLED since spawn(). Not frames heard: what is
// audible is this minus whatever the sink is holding -- the ring depth on a
// board, which the board half measured at 0.85-3.5 ms. Schedule against this
// one, because it is the number the pump compares your event's frame against;
// subtract the depth only when you are asking what a listener has heard.
//
// Wraps at 2^32 frames, 24.9 hours at 48 kHz. Nothing has to be done about
// that in Python: `now() + n` is an ordinary int and `at()` masks it.
static mp_obj_t audiopump_now(void) {
    return mp_obj_new_int_from_uint(
        AUDIOPUMP_LOAD_ACQ(&audiopump_ctx.frames));
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_now_obj, audiopump_now);

// Attach or detach, and report. One pointer either way, so the lock is held
// for a store -- but it IS held, because a pump reading the pointer half-way
// through is the whole class of bug this spike is about.
static mp_obj_t audiopump_set_events(size_t n_args, const mp_obj_t *args) {
    if (n_args > 0) {
        mp_obj_t q = args[0];
        if (q != mp_const_none && !mp_obj_is_type(q, &audiopump_events_type)) {
            mp_raise_TypeError(MP_ERROR_TEXT("expected an Events queue"));
        }
        if (q == mp_const_none) {
            // Detach: the pump lets go first, then the root does.
            audioif_pump_lock_acquire();
            audiopump_ctx.events = MP_OBJ_NULL;
            audioif_pump_lock_release();
            MP_STATE_VM(audiopump_held)[AUDIOPUMP_HELD_EVENTS] = MP_OBJ_NULL;
        } else {
            // Attach: the root takes hold first, so the queue is reachable
            // before the pump can reach it.
            audiopump_arm_guard();
            MP_STATE_VM(audiopump_held)[AUDIOPUMP_HELD_EVENTS] = q;
            audioif_pump_lock_acquire();
            audiopump_ctx.events = q;
            audioif_pump_lock_release();
        }
    }
    mp_obj_t held = MP_STATE_VM(audiopump_held)[AUDIOPUMP_HELD_EVENTS];
    return held == MP_OBJ_NULL ? mp_const_none : held;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiopump_set_events_obj, 0, 1,
    audiopump_set_events);

static mp_obj_t audiopump_set_tap(size_t n_args, const mp_obj_t *args) {
    if (n_args > 0) {
        mp_obj_t t = args[0];
        if (t != mp_const_none && !mp_obj_is_type(t, &audiopump_tap_type)) {
            mp_raise_TypeError(MP_ERROR_TEXT("expected a Tap"));
        }
        if (t == mp_const_none) {
            audioif_pump_lock_acquire();
            audiopump_ctx.tap = MP_OBJ_NULL;
            audioif_pump_lock_release();
            MP_STATE_VM(audiopump_held)[AUDIOPUMP_HELD_TAP] = MP_OBJ_NULL;
        } else {
            audiopump_arm_guard();
            MP_STATE_VM(audiopump_held)[AUDIOPUMP_HELD_TAP] = t;
            audioif_pump_lock_acquire();
            audiopump_ctx.tap = t;
            audioif_pump_lock_release();
        }
    }
    mp_obj_t held = MP_STATE_VM(audiopump_held)[AUDIOPUMP_HELD_TAP];
    return held == MP_OBJ_NULL ? mp_const_none : held;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiopump_set_tap_obj, 0, 1,
    audiopump_set_tap);


// --- what the lock cost, and why a pull gave up ---------------------------
//
// The status block carries these too, but a storm wants them without a pump
// running and wants to zero them between rounds.

static mp_obj_t audiopump_lock_stats(void) {
    const audioif_pump_lock_stats_t *s = audioif_pump_lock_stats();
    mp_obj_t items[7] = {
        mp_obj_new_int_from_ull(s->pump_takes),
        mp_obj_new_int_from_ull(s->pump_wait_us),
        mp_obj_new_int_from_ull(s->pump_wait_us_max),
        mp_obj_new_int_from_ull(s->ctrl_takes),
        mp_obj_new_int_from_ull(s->ctrl_wait_us),
        mp_obj_new_int_from_ull(s->ctrl_wait_us_max),
        mp_obj_new_int_from_ull(s->ctrl_held_us_max),
    };
    return mp_obj_new_tuple(7, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_lock_stats_obj,
    audiopump_lock_stats);

static mp_obj_t audiopump_lock_reset(void) {
    audioif_pump_lock_stats_reset();
    audioif_pump_fault_clear();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_lock_reset_obj,
    audiopump_lock_reset);

static mp_obj_t audiopump_fault(void) {
    return mp_obj_new_int_from_uint(audioif_pump_fault_get());
}
static MP_DEFINE_CONST_FUN_OBJ_0(audiopump_fault_obj, audiopump_fault);

static const mp_rom_map_elem_t audiopump_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_lock_stats), MP_ROM_PTR(&audiopump_lock_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_lock_reset), MP_ROM_PTR(&audiopump_lock_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_fault), MP_ROM_PTR(&audiopump_fault_obj) },
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiopump) },
    { MP_ROM_QSTR(MP_QSTR_pull), MP_ROM_PTR(&audiopump_pull_obj) },
    { MP_ROM_QSTR(MP_QSTR_spawn), MP_ROM_PTR(&audiopump_spawn_obj) },
    { MP_ROM_QSTR(MP_QSTR_join), MP_ROM_PTR(&audiopump_join_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audiopump_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_park), MP_ROM_PTR(&audiopump_park_obj) },
    { MP_ROM_QSTR(MP_QSTR_unpark), MP_ROM_PTR(&audiopump_unpark_obj) },
    { MP_ROM_QSTR(MP_QSTR_retarget), MP_ROM_PTR(&audiopump_retarget_obj) },
    { MP_ROM_QSTR(MP_QSTR_drain), MP_ROM_PTR(&audiopump_drain_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&audiopump_reset_graph_obj) },
    { MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&audiopump_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_running), MP_ROM_PTR(&audiopump_running_obj) },
    { MP_ROM_QSTR(MP_QSTR_shutdown), MP_ROM_PTR(&audiopump_shutdown_obj) },
    // The port with no thread, and the one question that tells a driver
    // whether it has to call service() itself. Both exist on every port.
    { MP_ROM_QSTR(MP_QSTR_service), MP_ROM_PTR(&audiopump_service_obj) },
    { MP_ROM_QSTR(MP_QSTR_threaded), MP_ROM_PTR(&audiopump_threaded_obj) },
    { MP_ROM_QSTR(MP_QSTR_backpressure),
      MP_ROM_PTR(&audiopump_backpressure_obj) },
    { MP_ROM_QSTR(MP_QSTR_SERVICE_MORE), MP_ROM_INT(AUDIOPUMP_SERVICE_MORE) },
    { MP_ROM_QSTR(MP_QSTR_SERVICE_FULL), MP_ROM_INT(AUDIOPUMP_SERVICE_FULL) },
    { MP_ROM_QSTR(MP_QSTR_SERVICE_PARKED),
      MP_ROM_INT(AUDIOPUMP_SERVICE_PARKED) },
    { MP_ROM_QSTR(MP_QSTR_SERVICE_DONE), MP_ROM_INT(AUDIOPUMP_SERVICE_DONE) },
    { MP_ROM_QSTR(MP_QSTR_SERVICE_MASK), MP_ROM_INT(AUDIOPUMP_SERVICE_MASK) },
    { MP_ROM_QSTR(MP_QSTR_SERVICE_SHIFT),
      MP_ROM_INT(AUDIOPUMP_SERVICE_SHIFT) },
    // Which driver bound, and whether it gave us a thread. The hardware that
    // answers to it -- i2s_start, i2s_stop, i2s_dma_bytes, Input, rt_probe --
    // is NOT here any more: it belongs to the driver and it ships in the
    // driver's own module, `_audioif`.
    { MP_ROM_QSTR(MP_QSTR_driver), MP_ROM_PTR(&audiopump_driver_obj) },
    // The push side. Every port, not just esp32: the ring is RAM and a
    // memcpy, so the unix build is where its correctness is settled.
    { MP_ROM_QSTR(MP_QSTR_Ring), MP_ROM_PTR(&audiopump_ring_type) },
    // Sequenced music, and the way out to a meter. Same reasoning: RAM,
    // arithmetic and a memcpy, so the unix build settles both.
    { MP_ROM_QSTR(MP_QSTR_Events), MP_ROM_PTR(&audiopump_events_type) },
    { MP_ROM_QSTR(MP_QSTR_Tap), MP_ROM_PTR(&audiopump_tap_type) },
    { MP_ROM_QSTR(MP_QSTR_now), MP_ROM_PTR(&audiopump_now_obj) },
    { MP_ROM_QSTR(MP_QSTR_events), MP_ROM_PTR(&audiopump_set_events_obj) },
    { MP_ROM_QSTR(MP_QSTR_tap), MP_ROM_PTR(&audiopump_set_tap_obj) },
    // The closed set, as module constants rather than strings: an op is
    // compared on the pump thread and a qstr lookup there is a runtime call.
    { MP_ROM_QSTR(MP_QSTR_PRESS), MP_ROM_INT(AUDIOPUMP_OP_PRESS) },
    { MP_ROM_QSTR(MP_QSTR_RELEASE), MP_ROM_INT(AUDIOPUMP_OP_RELEASE) },
    { MP_ROM_QSTR(MP_QSTR_RELEASE_ALL), MP_ROM_INT(AUDIOPUMP_OP_RELEASE_ALL) },
    { MP_ROM_QSTR(MP_QSTR_PLAY), MP_ROM_INT(AUDIOPUMP_OP_PLAY) },
    { MP_ROM_QSTR(MP_QSTR_STOP), MP_ROM_INT(AUDIOPUMP_OP_STOP) },
    { MP_ROM_QSTR(MP_QSTR_LEVEL), MP_ROM_INT(AUDIOPUMP_OP_LEVEL) },
    { MP_ROM_QSTR(MP_QSTR_STATUS_BYTES),
      MP_ROM_INT(AUDIOPUMP_STATUS_BYTES) },
    { MP_ROM_QSTR(MP_QSTR_STATUS_WORDS),
      MP_ROM_INT(AUDIOPUMP_STATUS_WORDS) },
};
static MP_DEFINE_CONST_DICT(audiopump_globals, audiopump_globals_table);

const mp_obj_module_t audiopump_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audiopump_globals,
};
MP_REGISTER_MODULE(MP_QSTR_audiopump, audiopump_module);
