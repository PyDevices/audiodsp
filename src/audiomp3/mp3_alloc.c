// Ported from CircuitPython's shared-module/audiomp3/__init__.c (upstream
// repo: https://github.com/adafruit/circuitpython, MIT). Renamed from
// `__init__.c` to `mp3_alloc.c` for this port's flat layout. This is the
// default (non-coverage) allocator CP itself uses for the Helix decoder's
// internal buffers: GC-heap `m_malloc_maybe`, same as every other buffer
// this port allocates (see docs/how-it-works/upstream-diff.md's
// `m_malloc_without_collect` -> `m_malloc` fix note for the established
// convention) rather than the unix coverage variant's raw
// malloc()/free() shortcut -- that shortcut has no effect on rendered PCM
// either way (it only changes where the memory comes from), so using the
// production path here doesn't cost any oracle-diff byte-exactness.
//
// Deviation from upstream, part 1: `mp3_free` uses `gc_free` (when GC is
// enabled), not `m_free`. Unlike CP's own `m_free(ptr)`, mainline's `m_free`
// needs an explicit size argument on any port with
// `MICROPY_MALLOC_USES_ALLOCATED_SIZE` (this workspace's unix port:
// `ports/unix/variants/mpconfigvariant_common.h`) -- but `mp3_free`'s
// callers (Helix's own `buffers.c`, via the `MPDEC_FREE(x)` macro) only
// ever pass a bare pointer, with no size in scope to give it. `gc_free`
// needs no size at all (the GC heap's own block header already carries
// it), so it's the correct symmetric free for anything `m_malloc_maybe`
// (itself GC-heap-backed whenever GC is enabled) handed out -- not a
// workaround, just the matching primitive. Falls back to plain `free()`
// when GC is disabled, matching what `m_malloc_maybe` itself falls back to
// in that configuration (py/malloc.c).
//
// Deviation from upstream, part 2: no `MP_WEAK` here (upstream uses it so a
// port can opt in to its own allocator via `CIRCUITPY_AUDIOMP3_USE_PORT_ALLOCATOR`
// -- this port has no such opt-in, so there is never a second, strong
// definition to prefer). Needed for this workspace's windows target: a
// weak-only symbol with no strong definition anywhere in the link is a
// known mingw-w64/GNU-ld PE-COFF gap (unlike ELF, where a lone weak
// definition resolves references just fine, confirmed working on this
// port's unix build) -- linking `micropython.exe` failed with "undefined
// reference to `mp3_alloc`/`mp3_free`" despite `mp3_alloc.o` compiling
// clean and `nm` showing both symbols correctly emitted as PE weak
// externals. unix is unaffected either way.
//
// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2013, 2014 Damien P. George
// SPDX-FileCopyrightText: Copyright (c) 2024 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2019 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include <stdlib.h>

#include "audiomp3/mp3_alloc.h"
#include "py/misc.h"
#include "py/mpconfig.h"
#include "shared/audiodsp_port.h"

#if MICROPY_ENABLE_GC
#include "py/gc.h"
#endif

// Deviation from upstream, part 3: the bound driver may hand out faster
// memory for these buffers (shared/audiodsp_port.h, "memory a decoder
// touches on every frame"). Helix reads and writes them many times per
// frame; from PSRAM on a classic ESP32 that halved the decode rate in more
// than half of the runs measured. With no such hooks nothing changes. The
// binding never changes after the first call, so mp3_free always matches
// mp3_alloc.
static const audiodsp_port_ops_t *mp3_port_hot(void) {
    const audiodsp_port_ops_t *p = audiodsp_port();
    return (p->mem_alloc_hot != NULL && p->mem_free_hot != NULL) ? p : NULL;
}

void *mp3_alloc(size_t sz) {
    const audiodsp_port_ops_t *p = mp3_port_hot();
    if (p != NULL) {
        return p->mem_alloc_hot(sz);
    }
    return m_malloc_maybe(sz);
}

void mp3_free(void *ptr) {
    const audiodsp_port_ops_t *p = mp3_port_hot();
    if (p != NULL) {
        p->mem_free_hot(ptr);
        return;
    }
    #if MICROPY_ENABLE_GC
    gc_free(ptr);
    #else
    free(ptr);
    #endif
}
