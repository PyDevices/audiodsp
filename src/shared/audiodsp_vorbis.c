// Ogg Vorbis decoding for every audiodsp target: Tremor, compiled here.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// Tremor's .c files are included into this one translation unit rather than
// listed in each build. That buys three things the five build systems
// (MicroPython's make and CMake, CircuitPython's make, setuptools, and the
// Android wheel) would otherwise each have to get right on their own:
//
// - one source file to add, with no include path;
// - Tremor's symbols renamed (audiodsp_tremor_names.h) before any of its
//   headers is read, so another copy of Tremor in the same firmware links;
// - Tremor's warnings silenced here, for its code only, so a port that
//   builds with -Werror keeps every warning on for ours.
//
// Tremor itself is changed only where third_party/tremor/ORIGIN.txt says:
// two portability fixes, and codebook scratch moved off the stack.

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <malloc.h>
#else
#include <alloca.h>
#endif

#include "shared/audiodsp_port.h"
#include "shared/audiodsp_vorbis.h"
#include "shared/audiodsp_tremor_names.h"

// misc.h picks its 64-bit multiply's word order by comparing BYTE_ORDER with
// LITTLE_ENDIAN, and a toolchain whose headers define neither (MSVC, MinGW)
// would take both branches. Every target audiodsp builds for is
// little-endian; a big-endian one says so through the compiler's own macro.
#if !defined(BYTE_ORDER) || !defined(LITTLE_ENDIAN) || !defined(BIG_ENDIAN)
#undef BYTE_ORDER
#undef LITTLE_ENDIAN
#undef BIG_ENDIAN
#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN 4321
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define BYTE_ORDER BIG_ENDIAN
#else
#define BYTE_ORDER LITTLE_ENDIAN
#endif
#endif

// --- the allocator ---------------------------------------------------------
//
// Tremor allocates through _ogg_malloc and friends, which its os_types.h
// points at the C library. They're pointed at these instead, which ask the
// bound driver first (shared/audiodsp_port.h, "memory for a decoder"): on a
// board with PSRAM that is where the decoder's state goes.

static bool vorbis_port_mem(const audiodsp_port_ops_t **port) {
    const audiodsp_port_ops_t *p = audiodsp_port();
    *port = p;
    return p->mem_alloc != NULL && p->mem_realloc != NULL && p->mem_free != NULL;
}

static void *vorbis_raw_malloc(size_t n) {
    const audiodsp_port_ops_t *p;
    return vorbis_port_mem(&p) ? p->mem_alloc(n) : malloc(n);
}

static void *vorbis_raw_realloc(void *ptr, size_t n) {
    const audiodsp_port_ops_t *p;
    return vorbis_port_mem(&p) ? p->mem_realloc(ptr, n) : realloc(ptr, n);
}

static void vorbis_raw_free(void *ptr) {
    const audiodsp_port_ops_t *p;
    if (vorbis_port_mem(&p)) {
        p->mem_free(ptr);
    } else {
        free(ptr);
    }
}

#ifdef AUDIODSP_VORBIS_HEAP_STATS
// A size word ahead of every block, so frees can be counted. A measuring
// build only; it costs a word a block and a lock it doesn't take.
typedef union {
    size_t size;
    long double align_ld;
    void *align_p;
    long long align_ll;
} vorbis_heap_header_t;

static size_t vorbis_heap_live;
static size_t vorbis_heap_peak;

static void *vorbis_heap_note(vorbis_heap_header_t *h, size_t n) {
    if (h == NULL) {
        return NULL;
    }
    h->size = n;
    vorbis_heap_live += n;
    if (vorbis_heap_live > vorbis_heap_peak) {
        vorbis_heap_peak = vorbis_heap_live;
    }
    return h + 1;
}

static void *vorbis_malloc(size_t n) {
    return vorbis_heap_note(vorbis_raw_malloc(sizeof(vorbis_heap_header_t) + n), n);
}

static void *vorbis_calloc(size_t count, size_t size) {
    size_t n = count * size;
    void *p = vorbis_malloc(n);
    if (p != NULL) {
        memset(p, 0, n);
    }
    return p;
}

static void vorbis_free(void *p) {
    if (p != NULL) {
        vorbis_heap_header_t *h = (vorbis_heap_header_t *)p - 1;
        vorbis_heap_live -= h->size;
        vorbis_raw_free(h);
    }
}

static void *vorbis_realloc(void *p, size_t n) {
    if (p == NULL) {
        return vorbis_malloc(n);
    }
    vorbis_heap_header_t *h = (vorbis_heap_header_t *)p - 1;
    size_t old = h->size;
    vorbis_heap_header_t *moved = vorbis_raw_realloc(h, sizeof(*h) + n);
    if (moved == NULL) {
        return NULL;
    }
    vorbis_heap_live -= old;
    return vorbis_heap_note(moved, n);
}

void audiodsp_vorbis_heap_stats(size_t *live, size_t *peak) {
    *live = vorbis_heap_live;
    *peak = vorbis_heap_peak;
}

void audiodsp_vorbis_heap_stats_reset(void) {
    vorbis_heap_peak = vorbis_heap_live;
}
#else
static void *vorbis_malloc(size_t n) {
    return vorbis_raw_malloc(n);
}

static void *vorbis_calloc(size_t count, size_t size) {
    if (size != 0 && count > (size_t)-1 / size) {
        return NULL;
    }
    void *p = vorbis_raw_malloc(count * size);
    if (p != NULL) {
        memset(p, 0, count * size);
    }
    return p;
}

static void *vorbis_realloc(void *p, size_t n) {
    return vorbis_raw_realloc(p, n);
}

static void vorbis_free(void *p) {
    vorbis_raw_free(p);
}

void audiodsp_vorbis_heap_stats(size_t *live, size_t *peak) {
    *live = 0;
    *peak = 0;
}

void audiodsp_vorbis_heap_stats_reset(void) {
}
#endif

// --- Tremor ----------------------------------------------------------------

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-label"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wuninitialized"
#pragma GCC diagnostic ignored "-Wchar-subscripts"
#pragma GCC diagnostic ignored "-Wshift-negative-value"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wparentheses"
#pragma GCC diagnostic ignored "-Wpointer-arith"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wold-style-declaration"
#pragma GCC diagnostic ignored "-Wold-style-definition"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wmissing-declarations"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wfloat-conversion"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wfloat-equal"
#pragma GCC diagnostic ignored "-Wempty-body"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

// os_types.h first, so its allocator names can be pointed at ours before any
// file uses them. Its include guard keeps the other files from undoing this.
#include "../../third_party/tremor/os_types.h"
#undef _ogg_malloc
#undef _ogg_calloc
#undef _ogg_realloc
#undef _ogg_free
#define _ogg_malloc vorbis_malloc
#define _ogg_calloc vorbis_calloc
#define _ogg_realloc vorbis_realloc
#define _ogg_free vorbis_free

// Four files each have their own static ilog(); one translation unit can only
// hold one of each name. misc.c is left out: it is Tremor's debugging
// allocator, which nothing calls unless _VDBG is defined.
#define ilog vorbis_ilog_bitwise
#include "../../third_party/tremor/bitwise.c"
#undef ilog
#include "../../third_party/tremor/codebook.c"
#define ilog vorbis_ilog_dsp
#include "../../third_party/tremor/dsp.c"
#undef ilog
#include "../../third_party/tremor/floor0.c"
#define ilog vorbis_ilog_floor1
#include "../../third_party/tremor/floor1.c"
#undef ilog
#include "../../third_party/tremor/floor_lookup.c"
#include "../../third_party/tremor/framing.c"
#include "../../third_party/tremor/info.c"
#define ilog vorbis_ilog_mapping0
#include "../../third_party/tremor/mapping0.c"
#undef ilog
#include "../../third_party/tremor/mdct.c"
#include "../../third_party/tremor/res012.c"
#include "../../third_party/tremor/vorbisfile.c"

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// --- the API ---------------------------------------------------------------

struct audiodsp_vorbis {
    OggVorbis_File vf;
    const audiodsp_vorbis_io_t *io;
    void *stream;
    int channels;
    long rate;
    // The link the last read came from, and whether a later link changed
    // format and so ended the stream (audiodsp_vorbis.h, read()).
    int link;
    int ended;
};

static size_t vorbis_cb_read(void *ptr, size_t size, size_t nmemb, void *ds) {
    audiodsp_vorbis_t *v = ds;
    size_t want = size * nmemb;
    if (want > LONG_MAX) {
        want = LONG_MAX;
    }
    long got = v->io->read(v->stream, ptr, want);
    if (got < 0) {
        // Tremor tells a failed read from the end of the stream by errno.
        errno = EIO;
        return 0;
    }
    return (size_t)got / (size ? size : 1);
}

static int vorbis_cb_seek(void *ds, ogg_int64_t offset, int whence) {
    audiodsp_vorbis_t *v = ds;
    if (v->io->seek == NULL) {
        return -1;
    }
    return v->io->seek(v->stream, (int64_t)offset, whence) == 0 ? 0 : -1;
}

static int vorbis_cb_close(void *ds) {
    // The stream is the caller's.
    (void)ds;
    return 0;
}

static long vorbis_cb_tell(void *ds) {
    audiodsp_vorbis_t *v = ds;
    return v->io->tell != NULL ? v->io->tell(v->stream) : -1;
}

static int vorbis_error(int ov) {
    switch (ov) {
        case OV_EREAD:
            return AUDIODSP_VORBIS_EREAD;
        case OV_ENOTVORBIS:
        case OV_ENOTAUDIO:
        case OV_EOF:
        case OV_FALSE:
            return AUDIODSP_VORBIS_ENOTVORBIS;
        case OV_EBADHEADER:
        case OV_EBADLINK:
        case OV_EBADPACKET:
            return AUDIODSP_VORBIS_EBADHEADER;
        case OV_EVERSION:
            return AUDIODSP_VORBIS_EVERSION;
        case OV_ENOSEEK:
            return AUDIODSP_VORBIS_ENOSEEK;
        case OV_EINVAL:
            return AUDIODSP_VORBIS_EINVAL;
        default:
            return AUDIODSP_VORBIS_EFAULT;
    }
}

audiodsp_vorbis_t *audiodsp_vorbis_open(const audiodsp_vorbis_io_t *io,
    void *stream, int *error) {
    int err = 0;
    audiodsp_vorbis_t *v = NULL;
    if (io == NULL || io->read == NULL || (io->seek != NULL && io->tell == NULL)) {
        err = AUDIODSP_VORBIS_EINVAL;
        goto out;
    }
    v = vorbis_calloc(1, sizeof(*v));
    if (v == NULL) {
        err = AUDIODSP_VORBIS_ENOMEM;
        goto out;
    }
    v->io = io;
    v->stream = stream;
    static const ov_callbacks callbacks = {
        vorbis_cb_read, vorbis_cb_seek, vorbis_cb_close, vorbis_cb_tell,
    };
    int ov = ov_open_callbacks(v, &v->vf, NULL, 0, callbacks);
    if (ov < 0) {
        // Tremor has already cleared vf on every failing path.
        vorbis_free(v);
        v = NULL;
        err = vorbis_error(ov);
        goto out;
    }
    vorbis_info *vi = ov_info(&v->vf, -1);
    if (vi == NULL || vi->channels < 1 || vi->rate < 1) {
        ov_clear(&v->vf);
        vorbis_free(v);
        v = NULL;
        err = AUDIODSP_VORBIS_EBADHEADER;
        goto out;
    }
    v->channels = vi->channels;
    v->rate = vi->rate;
    v->link = v->vf.current_link;
out:
    if (error != NULL) {
        *error = err;
    }
    return v;
}

void audiodsp_vorbis_close(audiodsp_vorbis_t *v) {
    if (v != NULL) {
        ov_clear(&v->vf);
        vorbis_free(v);
    }
}

int audiodsp_vorbis_channels(const audiodsp_vorbis_t *v) {
    return v->channels;
}

long audiodsp_vorbis_sample_rate(const audiodsp_vorbis_t *v) {
    return v->rate;
}

int64_t audiodsp_vorbis_frames(audiodsp_vorbis_t *v) {
    if (!ov_seekable(&v->vf)) {
        return -1;
    }
    ogg_int64_t total = ov_pcm_total(&v->vf, -1);
    return total < 0 ? -1 : (int64_t)total;
}

int64_t audiodsp_vorbis_tell(audiodsp_vorbis_t *v) {
    ogg_int64_t at = ov_pcm_tell(&v->vf);
    return at < 0 ? 0 : (int64_t)at;
}

long audiodsp_vorbis_read(audiodsp_vorbis_t *v, int16_t *pcm, size_t frames) {
    if (v == NULL || pcm == NULL) {
        return AUDIODSP_VORBIS_EINVAL;
    }
    if (v->ended || frames == 0) {
        return 0;
    }
    size_t bytes = frames * (size_t)v->channels * sizeof(int16_t);
    if (bytes > INT_MAX) {
        bytes = INT_MAX - (INT_MAX % ((size_t)v->channels * sizeof(int16_t)));
    }
    for (;;) {
        int link = v->link;
        long got = ov_read(&v->vf, pcm, (int)bytes, &link);
        if (got == OV_HOLE) {
            // A lost or damaged page: Tremor has resynchronised past it.
            continue;
        }
        if (got < 0) {
            return vorbis_error((int)got);
        }
        if (got == 0) {
            return 0;
        }
        if (link != v->link) {
            vorbis_info *vi = ov_info(&v->vf, -1);
            if (vi == NULL || vi->channels != v->channels || vi->rate != v->rate) {
                v->ended = 1;
                return 0;
            }
            v->link = link;
        }
        return got / (long)((size_t)v->channels * sizeof(int16_t));
    }
}

int audiodsp_vorbis_seek(audiodsp_vorbis_t *v, int64_t frame) {
    if (v == NULL || frame < 0) {
        return AUDIODSP_VORBIS_EINVAL;
    }
    if (!ov_seekable(&v->vf)) {
        return AUDIODSP_VORBIS_ENOSEEK;
    }
    int ov = ov_pcm_seek(&v->vf, (ogg_int64_t)frame);
    if (ov < 0) {
        return vorbis_error(ov);
    }
    v->ended = 0;
    v->link = v->vf.current_link;
    return 0;
}

const char *audiodsp_vorbis_strerror(int error) {
    switch (error) {
        case 0:
            return "no error";
        case AUDIODSP_VORBIS_EREAD:
            return "reading the stream failed";
        case AUDIODSP_VORBIS_ENOTVORBIS:
            return "not an Ogg Vorbis stream";
        case AUDIODSP_VORBIS_EBADHEADER:
            return "the Vorbis headers are damaged";
        case AUDIODSP_VORBIS_EVERSION:
            return "a Vorbis version this decoder can't read";
        case AUDIODSP_VORBIS_ENOMEM:
            return "out of memory";
        case AUDIODSP_VORBIS_ENOSEEK:
            return "the stream can't seek";
        case AUDIODSP_VORBIS_EINVAL:
            return "invalid argument";
        default:
            return "the Vorbis decoder failed";
    }
}
