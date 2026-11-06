// The few C library calls audiodsp makes that a MicroPython port built
// without a C library doesn't have.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// The stm32 port links with -nostdlib: it has its own string functions and a
// single-precision libm, and no malloc at all. audiodsp's feature extractors
// and the vendored microfrontend allocate their working state with malloc and
// print setup errors to stderr. micropython.mk links the toolchain's libm for
// the double-precision maths and builds this file for the rest, on that port
// only. Every other port has a C library and never sees it.
//
// malloc is MicroPython's tracked allocator: memory from the GC heap that the
// collector keeps alive until it is freed, which is what a long-lived C
// allocation needs. A soft reset empties the heap, and audiodsp lets go of its
// state before then, through the finalisers that own it.

#include <errno.h>
#include <reent.h>
#include <stddef.h>
#include <stdio.h>

#include "py/mpconfig.h"
#include "py/misc.h"

#if !MICROPY_TRACKED_ALLOC
#error "audiodsp needs MICROPY_TRACKED_ALLOC on a port without a C library"
#endif

void *malloc(size_t size) {
    return m_tracked_calloc(1, size);
}

void *calloc(size_t nmemb, size_t size) {
    return m_tracked_calloc(nmemb, size);
}

void *realloc(void *ptr, size_t size) {
    return m_tracked_realloc(ptr, size);
}

void free(void *ptr) {
    m_tracked_free(ptr);
}

// stderr, for the microfrontend's setup errors, which go nowhere: the Python
// call that failed raises instead.
static struct _reent mp_libc_shim_reent;
struct _reent *_impure_ptr = &mp_libc_shim_reent;

int fputs(const char *s, FILE *stream) {
    (void)s;
    (void)stream;
    return 0;
}

// What libm sets on a domain or range error.
int *__errno(void) {
    return &mp_libc_shim_reent._errno;
}
