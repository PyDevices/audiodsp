// Ported from CircuitPython's shared-bindings/audiomp3/MP3Decoder.c and
// shared-module/audiomp3/MP3Decoder.c (upstream repo:
// https://github.com/adafruit/circuitpython, MIT), merged into one file --
// this port doesn't keep CP's shared-bindings/shared-module split.
//
// `shared/runtime/context_manager_helpers.h` -> `cp_compat/context_manager_helpers.h`;
// `py/objproperty.h` -> `cp_compat/objproperty.h`; `shared-bindings/util.h`'s
// `mp_arg_validate_*` -> `cp_compat/argcheck.h`; `supervisor/background_callback.h`
// -> `cp_compat/background_callback.h` (a stub, not a port -- see that
// header). `lib/mp3/src/{mp3common,coder}.h` -> the vendored `mp3/src`
// clone in the parent workspace (see docs/porting-plan.md "Tier 5 audiomp3" and
// docs/upstream-diff.md for the license and the one local patch it needed).
// `MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS` keeps upstream's flag; this port adds
// `attr, cp_compat_attr` for property invocation on a native type, same as
// every other tier -- see docs/upstream-diff.md, "Property invocation needs
// an explicit attr slot."
//
// Deviation from upstream: `mp_obj_malloc_with_finaliser` here is mainline's
// own macro (py/obj.h), confirmed identical in shape to CP's
// (`mp_obj_malloc_with_finaliser_helper`-backed on both); no shim needed.
//
// Deviation from upstream: explicit `#include <errno.h>` added for `EINVAL`
// (used bare, not `MP_EINVAL`, matching upstream exactly). glibc and
// mingw-w64's headers happen to expose it transitively through
// `<sys/types.h>`/`<unistd.h>` already, which is why this was never needed
// on unix or windows; emscripten's libc does not, and the wasm build
// (phase 8d) failed with "use of undeclared identifier 'EINVAL'" without it.
//
// Deviation from upstream, 2026-09-21: `<unistd.h>` replaced by `<stdio.h>`.
// Nothing in here calls a POSIX function -- the reads and seeks all go
// through `py/stream.h` -- and what the header was actually supplying is
// `SEEK_SET`/`SEEK_CUR`, which are ISO C and live in `<stdio.h>`. `off_t`
// and `ssize_t` come from `<sys/types.h>`, which is still here.
// tools/check_portable.py fails on `<unistd.h>` anywhere in src/, and this
// was the one place in the repo that had it.
//
// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft
// SPDX-FileCopyrightText: Copyright (c) 2019 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>

#include "py/builtin.h"
#include "py/mperrno.h"
#include "py/runtime.h"
#include "py/stream.h"

#include "audiocore/__init__.h"
#include "audiomp3/MP3Decoder.h"
#include "cp_compat/background_callback.h"
#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/objproperty.h"
#include "shared/audioif_pump_lock.h"

#include "coder.h"
#include "mp3common.h"

#define MAX_BUFFER_LEN (MAX_NSAMP * MAX_NGRAN * MAX_NCHAN * sizeof(int16_t))

#define DO_DEBUG (0)

// Deviation from upstream, this port's own windows target only (see
// docs/upstream-diff.md, "Tier 5 audiomp3: MP_STREAM_POLL is unavailable on
// win32 for real files"): mainline's own extmod/vfs_posix_file.c raises
// NotImplementedError from MP_STREAM_POLL on _WIN32 for regular POSIX-style
// file objects (not sockets) rather than returning a value -- so this
// port's unix build takes the code below unchanged, but on windows the poll
// call itself would raise before ever returning "not readable". Skip it
// there and treat the stream as always-possibly-readable, exactly the same
// fallback already used a few lines down for a stream with no ioctl slot at
// all -- mp3file_update_inbuf_always()'s own non-blocking read handling is
// what actually avoids stalling either way.
static bool stream_readable(void *stream) {
    #ifdef _WIN32
    return true;
    #else
    int errcode = 0;
    mp_obj_base_t *o = MP_OBJ_TO_PTR(stream);
    const mp_stream_p_t *stream_p = MP_OBJ_TYPE_GET_SLOT(o->type, protocol);
    if (!stream_p->ioctl) {
        return true;
    }

    mp_int_t ret = stream_p->ioctl(stream, MP_STREAM_POLL, MP_STREAM_POLL_RD | MP_STREAM_POLL_ERR | MP_STREAM_POLL_HUP, &errcode);
    if (DO_DEBUG) {
        mp_printf(&mp_plat_print, "stream_readable ioctl() -> %d [errcode=%d]\n", ret, errcode);
    }
    return ret != 0;
    #endif
}

// This is a near copy of mp_stream_posix_read, but avoiding use of global
// errno value & with added prints for debugging purposes. (circuitpython doesn't
// enable mp_stream_posix_read anyway)
static mp_int_t stream_read(void *stream, void *buf, size_t len) {
    int errcode;
    mp_obj_base_t *o = MP_OBJ_TO_PTR(stream);
    const mp_stream_p_t *stream_p = MP_OBJ_TYPE_GET_SLOT(o->type, protocol);
    if (!stream_p->read) {
        return -EINVAL;
    }
    mp_uint_t out_sz = stream_p->read(MP_OBJ_FROM_PTR(stream), buf, len, &errcode);
    if (DO_DEBUG) {
        mp_printf(&mp_plat_print, "stream_read(%d) -> %d\n", (int)len, (int)out_sz);
    }
    if (out_sz == MP_STREAM_ERROR) {
        if (DO_DEBUG) {
            mp_printf(&mp_plat_print, "errcode=%d\n", errcode);
        }
        return -errcode; // CIRCUITPY-CHANGE: returns negative errcode value
    } else {
        return out_sz;
    }
}

// This is a near copy of mp_stream_posix_lseek, but avoiding use of global
// errno value (circuitpython doesn't enable posix stream routines anyway)
static off_t stream_lseek(void *stream, off_t offset, int whence) {
    int errcode;
    const mp_obj_base_t *o = stream;
    const mp_stream_p_t *stream_p = MP_OBJ_TYPE_GET_SLOT(o->type, protocol);
    if (!stream_p->ioctl) {
        return -EINVAL;
    }
    struct mp_stream_seek_t seek_s;
    seek_s.offset = offset;
    seek_s.whence = whence;
    mp_uint_t res = stream_p->ioctl(MP_OBJ_FROM_PTR(stream), MP_STREAM_SEEK, (mp_uint_t)(uintptr_t)&seek_s, &errcode);
    if (res == MP_STREAM_ERROR) {
        return -errcode;
    }
    return seek_s.offset;
}

#define INPUT_BUFFER_AVAILABLE(i) ((i).write_off - (i).read_off)
#define INPUT_BUFFER_SPACE(i) ((i).size - INPUT_BUFFER_AVAILABLE(i))
#define INPUT_BUFFER_READ_PTR(i) ((i).buf + (i).read_off)
#define INPUT_BUFFER_CONSUME(i, n) ((i).read_off += (n))
#define INPUT_BUFFER_CLEAR(i) ((i).read_off = (i).write_off = 0)

static void stream_set_blocking(audiomp3_mp3file_obj_t *self, bool block_ok) {
    if (!self->settimeout_args[0]) {
        return;
    }
    if (block_ok == self->block_ok) {
        return;
    }
    self->block_ok = block_ok;
    self->settimeout_args[2] = block_ok ? mp_const_none : mp_obj_new_int(0);
    mp_call_method_n_kw(1, 0, self->settimeout_args);
}

/** Fill the input buffer unconditionally.
 *
 * Returns true if the input buffer contains any useful data,
 * false otherwise.  (The input buffer will be padded to the end with
 * 0 bytes, which do not interfere with MP3 decoding)
 *
 * Raises OSError if stream_read fails.
 *
 * Sets self->eof if any read of the file returns 0 bytes
 */
static bool mp3file_update_inbuf_always(audiomp3_mp3file_obj_t *self, bool block_ok) {
    if (self->eof || INPUT_BUFFER_SPACE(self->inbuf) == 0) {
        return INPUT_BUFFER_AVAILABLE(self->inbuf) > 0;
    }

    stream_set_blocking(self, block_ok);

    // We didn't previously reach EOF and we have input buffer space available

    // Move the unconsumed portion of the buffer to the start
    if (self->inbuf.read_off) {
        memmove(self->inbuf.buf, INPUT_BUFFER_READ_PTR(self->inbuf), INPUT_BUFFER_AVAILABLE(self->inbuf));
        self->inbuf.write_off -= self->inbuf.read_off;
        self->inbuf.read_off = 0;
    }

    for (size_t to_read; !self->eof && (to_read = INPUT_BUFFER_SPACE(self->inbuf)) > 0;) {
        uint8_t *write_ptr = self->inbuf.buf + self->inbuf.write_off;
        ssize_t n_read = stream_read(self->stream, write_ptr, to_read);

        if (n_read < 0) {
            int errcode = -n_read;
            if (mp_is_nonblocking_error(errcode) || errcode == MP_ETIMEDOUT) {
                break;
            }
            self->eof = true;
            mp_raise_OSError(errcode);
        }

        if (n_read == 0) {
            self->eof = true;
        }

        self->inbuf.write_off += n_read;
    }

    if (DO_DEBUG) {
        mp_printf(&mp_plat_print, "new avail=%d eof=%d\n", (int)INPUT_BUFFER_AVAILABLE(self->inbuf), self->eof);
    }

    // Return true iff there are at least some useful bytes in the buffer
    return INPUT_BUFFER_AVAILABLE(self->inbuf) > 0;
}

/** Update the inbuf from a background callback.
 *
 * Re-queue if there's still buffer space available to read stream data
 */
// Upstream re-queues itself here under `#if !defined(MICROPY_UNIX_COVERAGE)`
// (`background_callback_add(&self->inbuf_fill_cb, mp3file_update_inbuf_cb,
// self)`) so a real background scheduler keeps topping up the input buffer
// between get_buffer() calls. Omitted here, matching what that #if already
// compiles down to on CircuitPython's own unix coverage build (our parity
// oracle): this port's background_callback_add (cp_compat/background_callback.h)
// always calls its function immediately and synchronously, so re-queueing
// from inside the callback itself would recurse forever rather than
// deferring -- there is no "later" for it to defer to yet.
static void mp3file_update_inbuf_cb(void *self_in) {
    audiomp3_mp3file_obj_t *self = self_in;
    if (audiosample_deinited(&self->base)) {
        return;
    }
    if (!self->eof && stream_readable(self->stream)) {
        mp3file_update_inbuf_always(self, false);
    }
}

/** Fill the input buffer if it is less than half full.
 *
 * Returns the same as mp3file_update_inbuf_always.
 */
static bool mp3file_update_inbuf_half(audiomp3_mp3file_obj_t *self, bool block_ok) {
    // If buffer is over half full, do nothing
    if (INPUT_BUFFER_SPACE(self->inbuf) < self->inbuf.size / 2) {
        return true;
    }

    return mp3file_update_inbuf_always(self, block_ok);
}

#define READ_PTR(self) (INPUT_BUFFER_READ_PTR(self->inbuf))
#define BYTES_LEFT(self) (INPUT_BUFFER_AVAILABLE(self->inbuf))
#define CONSUME(self, n) (INPUT_BUFFER_CONSUME(self->inbuf, n))

// http://id3.org/id3v2.3.0
static void mp3file_skip_id3v2(audiomp3_mp3file_obj_t *self, bool block_ok) {
    mp3file_update_inbuf_half(self, block_ok);
    if (BYTES_LEFT(self) < 10) {
        return;
    }
    uint8_t *data = READ_PTR(self);
    if (!(
        data[0] == 'I' &&
        data[1] == 'D' &&
        data[2] == '3' &&
        data[3] != 0xff &&
        data[4] != 0xff &&
        (data[5] & 0x1f) == 0 &&
        (data[6] & 0x80) == 0 &&
        (data[7] & 0x80) == 0 &&
        (data[8] & 0x80) == 0 &&
        (data[9] & 0x80) == 0)) {
        return;
    }
    int32_t size = (data[6] << 21) | (data[7] << 14) | (data[8] << 7) | (data[9]);
    size += 10; // size excludes the "header" (but not the "extended header")
    // First, deduct from size whatever is left in buffer
    if (DO_DEBUG) {
        mp_printf(&mp_plat_print, "%s:%d id3 size %d\n", __FILE__, __LINE__, size);
    }
    uint32_t to_consume = MIN(size, BYTES_LEFT(self));
    CONSUME(self, to_consume);
    size -= to_consume;

    // Next, seek in the file after the header
    if (stream_lseek(self->stream, SEEK_CUR, size) == 0) {
        return;
    }

    // Couldn't seek (might be a socket), so need to actually read and discard all that data
    while (size > 0 && !self->eof) {
        mp3file_update_inbuf_always(self, true);
        to_consume = MIN(size, BYTES_LEFT(self));
        CONSUME(self, to_consume);
        size -= to_consume;
    }
}

/* If a sync word can be found, advance to it and return true.  Otherwise,
 * return false.
 */
static bool mp3file_find_sync_word(audiomp3_mp3file_obj_t *self, bool block_ok) {
    do {
        mp3file_update_inbuf_half(self, block_ok);
        int offset = MP3FindSyncWord(READ_PTR(self), BYTES_LEFT(self));
        if (offset >= 0) {
            CONSUME(self, offset);
            mp3file_update_inbuf_half(self, block_ok);
            return true;
        }
        CONSUME(self, MAX(0, BYTES_LEFT(self) - 16));
    } while (!self->eof);
    return false;
}

static bool mp3file_get_next_frame_info(audiomp3_mp3file_obj_t *self, MP3FrameInfo *fi, bool block_ok) {
    int err;
    do {
        err = MP3GetNextFrameInfo(self->decoder, fi, READ_PTR(self));
        if (err == ERR_MP3_NONE) {
            break;
        }
        CONSUME(self, 1);
        mp3file_find_sync_word(self, block_ok);
    } while (!self->eof);
    return err == ERR_MP3_NONE;
}

#define DEFAULT_INPUT_BUFFER_SIZE (2048)
#define MIN_USER_BUFFER_SIZE (DEFAULT_INPUT_BUFFER_SIZE + 2 * MAX_BUFFER_LEN)

void common_hal_audiomp3_mp3file_construct(audiomp3_mp3file_obj_t *self,
    mp_obj_t stream,
    uint8_t *buffer,
    size_t buffer_size) {
    // Note: Adafruit_MP3 uses a 2kB input buffer and two 4kB output pcm_buffer.
    // for a whopping total of 10kB pcm_buffer (+mp3 decoder state and frame buffer)
    // At 44kHz, that's 23ms of output audio data.
    //
    // We will choose a slightly different allocation strategy for the output:
    // Make sure the pcm_buffer are sized exactly to match (a multiple of) the
    // frame size; this is typically 2304 * 2 bytes, so a little bit bigger
    // than the two 4kB output pcm_buffer, except that the alignment allows to
    // never allocate that extra frame buffer.

    if ((intptr_t)buffer & 1) {
        buffer += 1;
        buffer_size -= 1;
    }
    if (buffer && buffer_size > MIN_USER_BUFFER_SIZE) {
        self->pcm_buffer[0] = (int16_t *)(void *)buffer;
        self->pcm_buffer[1] = (int16_t *)(void *)(buffer + MAX_BUFFER_LEN);
        self->inbuf.buf = buffer + 2 * MAX_BUFFER_LEN;
        self->inbuf.size = buffer_size - 2 * MAX_BUFFER_LEN;
    } else {
        self->inbuf.size = DEFAULT_INPUT_BUFFER_SIZE;
        // Deviation from upstream: m_malloc_without_collect -> m_malloc, same
        // established fix as every other tier (see docs/upstream-diff.md) --
        // mainline has no non-collecting allocator.
        self->inbuf.buf = m_malloc(DEFAULT_INPUT_BUFFER_SIZE);
        if (self->inbuf.buf == NULL) {
            common_hal_audiomp3_mp3file_deinit(self);
            m_malloc_fail(DEFAULT_INPUT_BUFFER_SIZE);
        }

        if (buffer_size >= 2 * MAX_BUFFER_LEN) {
            self->pcm_buffer[0] = (int16_t *)(void *)buffer;
            self->pcm_buffer[1] = (int16_t *)(void *)(buffer + MAX_BUFFER_LEN);
        } else {
            self->pcm_buffer[0] = m_malloc(MAX_BUFFER_LEN);
            if (self->pcm_buffer[0] == NULL) {
                common_hal_audiomp3_mp3file_deinit(self);
                m_malloc_fail(MAX_BUFFER_LEN);
            }

            self->pcm_buffer[1] = m_malloc(MAX_BUFFER_LEN);
            if (self->pcm_buffer[1] == NULL) {
                common_hal_audiomp3_mp3file_deinit(self);
                m_malloc_fail(MAX_BUFFER_LEN);
            }
        }
    }
    self->inbuf.read_off = self->inbuf.write_off = 0;

    self->decoder = MP3InitDecoder();
    if (self->decoder == NULL) {
        common_hal_audiomp3_mp3file_deinit(self);
        mp_raise_msg(&mp_type_MemoryError,
            MP_ERROR_TEXT("Couldn't allocate decoder"));
    }

    common_hal_audiomp3_mp3file_set_file(self, stream);
}

void common_hal_audiomp3_mp3file_set_file(audiomp3_mp3file_obj_t *self, mp_obj_t stream) {
    background_callback_prevent();

    self->stream = stream;
    mp_load_method_maybe(stream, MP_QSTR_settimeout, self->settimeout_args);

    INPUT_BUFFER_CLEAR(self->inbuf);
    self->eof = 0;

    self->block_ok = false;
    stream_set_blocking(self, true);

    self->other_channel = -1;
    mp3file_update_inbuf_half(self, true);
    mp3file_find_sync_word(self, true);
    // It **SHOULD** not be necessary to do this; the buffer should be filled
    // with fresh content before it is returned by get_buffer().  The fact that
    // this is necessary to avoid a glitch at the start of playback of a second
    // track using the same decoder object means there's still a bug in
    // get_buffer() that I didn't understand.
    memset(self->pcm_buffer[0], 0, MAX_BUFFER_LEN);
    memset(self->pcm_buffer[1], 0, MAX_BUFFER_LEN);

    /* important to do this - DSP primitives assume a bunch of state variables are 0 on first use */
    struct _MP3DecInfo *decoder = self->decoder;
    memset(decoder->FrameHeaderPS, 0, sizeof(FrameHeader));
    memset(decoder->SideInfoPS, 0, sizeof(SideInfo));
    memset(decoder->ScaleFactorInfoPS, 0, sizeof(ScaleFactorInfo));
    memset(decoder->HuffmanInfoPS, 0, sizeof(HuffmanInfo));
    memset(decoder->DequantInfoPS, 0, sizeof(DequantInfo));
    memset(decoder->IMDCTInfoPS, 0, sizeof(IMDCTInfo));
    memset(decoder->SubbandInfoPS, 0, sizeof(SubbandInfo));

    MP3FrameInfo fi;
    bool result = mp3file_get_next_frame_info(self, &fi, true);
    background_callback_allow();
    if (!result) {
        mp_raise_msg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("Failed to parse MP3 file"));
    }

    self->base.sample_rate = fi.samprate;
    self->base.channel_count = fi.nChans;
    self->base.single_buffer = false;
    self->base.bits_per_sample = 16;
    self->base.samples_signed = true;
    self->base.max_buffer_length = fi.outputSamps * sizeof(int16_t);
    self->len = 2 * self->base.max_buffer_length;
    self->samples_decoded = 0;
}

void common_hal_audiomp3_mp3file_deinit(audiomp3_mp3file_obj_t *self) {
    // The whole body. mark_deinit is not the damage; the pointer
    // nulling AFTER it is -- the funnel's guard has already let a
    // pull in by then, and the pull writes into a buffer that has
    // just become NULL. Detach under the lock, free afterwards.
    audioif_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    if (self->decoder) {
        MP3FreeDecoder(self->decoder);
    }
    self->decoder = NULL;
    self->inbuf.buf = NULL;
    self->pcm_buffer[0] = NULL;
    self->pcm_buffer[1] = NULL;
    self->stream = mp_const_none;
    self->settimeout_args[0] = MP_OBJ_NULL;
    self->samples_decoded = 0;
    audioif_pump_lock_release();
}

// audiomp3 is the one module in the palette that cannot be made pump-safe,
// and should not be. It reads its stream from inside get_buffer, raises
// OSError there (mp3file_update_inbuf_always), and calls a Python method on
// the stream object to set its timeout (stream_set_blocking -> mp_call_method
// _n_kw). Re-entering the interpreter from the pump thread is not a thing that
// can be guarded; it is a thing that must not be reached. An MP3 source
// belongs behind a buffer the interpreter fills.
void audiomp3_mp3file_reset_buffer(audiomp3_mp3file_obj_t *self,
    bool single_channel_output,
    uint8_t channel) {
    if (audioif_pump_on_pump_thread()) {
        audioif_pump_fault_set(AUDIOIF_PUMP_FAULT_UNPUMPABLE);
        return;
    }
    if (single_channel_output && channel == 1) {
        return;
    }
    // We don't reset the buffer index in case we're looping and we have an odd number of buffer
    // loads
    background_callback_prevent();
    if (self->eof && stream_lseek(self->stream, SEEK_SET, 0) == 0) {
        INPUT_BUFFER_CLEAR(self->inbuf);
        self->eof = 0;
        self->samples_decoded = 0;
        self->other_channel = -1;
        mp3file_skip_id3v2(self, false);
        mp3file_find_sync_word(self, false);
    }
    background_callback_allow();
}

audioio_get_buffer_result_t audiomp3_mp3file_get_buffer(audiomp3_mp3file_obj_t *self,
    bool single_channel_output,
    uint8_t channel,
    uint8_t **bufptr,
    uint32_t *buffer_length) {
    if (audioif_pump_on_pump_thread()) {
        audioif_pump_fault_set(AUDIOIF_PUMP_FAULT_UNPUMPABLE);
        *bufptr = NULL;
        *buffer_length = 0;
        return GET_BUFFER_ERROR;
    }
    if (!self->inbuf.buf) {
        *buffer_length = 0;
        if (DO_DEBUG) {
            mp_printf(&mp_plat_print, "%s:%d\n", __FILE__, __LINE__);
        }
        return GET_BUFFER_ERROR;
    }
    if (!single_channel_output) {
        channel = 0;
    }

    size_t frame_buffer_size_bytes = self->base.max_buffer_length;
    *buffer_length = frame_buffer_size_bytes;

    if (channel == self->other_channel) {
        *bufptr = (uint8_t *)(self->pcm_buffer[self->other_buffer_index] + channel);
        self->other_channel = -1;
        self->samples_decoded += *buffer_length / sizeof(int16_t);
        if (DO_DEBUG) {
            mp_printf(&mp_plat_print, "%s:%d\n", __FILE__, __LINE__);
        }
        return GET_BUFFER_MORE_DATA;
    }


    self->buffer_index = !self->buffer_index;
    self->other_channel = 1 - channel;
    self->other_buffer_index = self->buffer_index;
    int16_t *buffer = (int16_t *)(void *)self->pcm_buffer[self->buffer_index];
    *bufptr = (uint8_t *)buffer;

    mp3file_skip_id3v2(self, false);
    if (!mp3file_find_sync_word(self, false)) {
        memset(buffer, 0, self->base.max_buffer_length);
        *buffer_length = 0;
        return self->eof ? GET_BUFFER_DONE : GET_BUFFER_ERROR;
    }
    int bytes_left = BYTES_LEFT(self);
    uint8_t *inbuf = READ_PTR(self);
    int err = MP3Decode(self->decoder, &inbuf, &bytes_left, buffer, 0);
    if (err != ERR_MP3_INDATA_UNDERFLOW) {
        CONSUME(self, BYTES_LEFT(self) - bytes_left);
    }
    if (err) {
        memset(buffer, 0, frame_buffer_size_bytes);
        if (DO_DEBUG) {
            mp_printf(&mp_plat_print, "%s:%d err=%d\n", __FILE__, __LINE__, err);
        }
        if (self->eof || (err != ERR_MP3_INDATA_UNDERFLOW && err != ERR_MP3_MAINDATA_UNDERFLOW)) {
            memset(buffer, 0, self->base.max_buffer_length);
            *buffer_length = 0;
            self->eof = true;
            return GET_BUFFER_ERROR;
        }
    }

    self->samples_decoded += frame_buffer_size_bytes / sizeof(int16_t);

    mp3file_skip_id3v2(self, false);
    int result = mp3file_find_sync_word(self, false) ? GET_BUFFER_MORE_DATA : GET_BUFFER_DONE;

    if (DO_DEBUG) {
        mp_printf(&mp_plat_print, "%s:%d result=%d\n", __FILE__, __LINE__, result);
    }
    if (INPUT_BUFFER_SPACE(self->inbuf) > 512) {
        background_callback_add(
            &self->inbuf_fill_cb,
            mp3file_update_inbuf_cb,
            self);
    }

    if (DO_DEBUG) {
        mp_printf(&mp_plat_print, "post-decode avail=%d eof=%d\n", (int)INPUT_BUFFER_AVAILABLE(self->inbuf), self->eof);
    }
    return result;
}

float common_hal_audiomp3_mp3file_get_rms_level(audiomp3_mp3file_obj_t *self) {
    float sumsq = 0.f;
    // Assumes no DC component to the audio.  Is that a safe assumption?
    int16_t *buffer = (int16_t *)(void *)self->pcm_buffer[self->buffer_index];
    for (size_t i = 0; i < self->base.max_buffer_length / sizeof(int16_t); i++) {
        sumsq += (float)buffer[i] * buffer[i];
    }
    return sqrtf(sumsq) / (self->base.max_buffer_length / sizeof(int16_t));
}

uint32_t common_hal_audiomp3_mp3file_get_samples_decoded(audiomp3_mp3file_obj_t *self) {
    return self->samples_decoded;
}

// --- Python bindings (shared-bindings/audiomp3/MP3Decoder.c) ---

static mp_obj_t audiomp3_mp3file_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 2, false);
    mp_obj_t stream = args[0];

    if (mp_obj_is_str(stream)) {
        stream = mp_call_function_2(MP_OBJ_FROM_PTR(&mp_builtin_open_obj), stream, MP_ROM_QSTR(MP_QSTR_rb));
    }

    const mp_stream_p_t *stream_p = mp_get_stream_raise(stream, MP_STREAM_OP_READ);

    if (stream_p->is_text) {
        mp_raise_TypeError(MP_ERROR_TEXT("file must be a file opened in byte mode"));
    }
    uint8_t *buffer = NULL;
    size_t buffer_size = 0;
    if (n_args >= 2) {
        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise(args[1], &bufinfo, MP_BUFFER_WRITE);
        buffer = bufinfo.buf;
        buffer_size = bufinfo.len;
    }

    audiomp3_mp3file_obj_t *self = mp_obj_malloc_with_finaliser(audiomp3_mp3file_obj_t, &audiomp3_mp3file_type);
    common_hal_audiomp3_mp3file_construct(self, stream, buffer, buffer_size);

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audiomp3_mp3file_deinit(mp_obj_t self_in) {
    audiomp3_mp3file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiomp3_mp3file_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiomp3_mp3file_deinit_obj, audiomp3_mp3file_deinit);

static void check_for_deinit(audiomp3_mp3file_obj_t *self) {
    // One word read under the lock, and the RAISE OUTSIDE IT. The lock's
    // contract is that nothing which can longjmp runs while it is held
    // (shared/audioif_pump_lock.h): a raise from in here never reaches the
    // release, so the mutex is left owned by a thread that has gone back to
    // the interpreter, and the pump blocks on it for ever. This is the guard
    // on every Python-facing method of this class, so it is the most reached
    // statement in the file.
    audioif_pump_lock_acquire();
    const bool released = audiosample_deinited(&self->base);
    audioif_pump_lock_release();
    if (released) {
        audiosample_check_for_deinit(&self->base);
    }
}

static mp_obj_t audiomp3_mp3file_obj_get_file(mp_obj_t self_in) {
    audiomp3_mp3file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return self->stream;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiomp3_mp3file_get_file_obj, audiomp3_mp3file_obj_get_file);

static mp_obj_t audiomp3_mp3file_obj_set_file(mp_obj_t self_in, mp_obj_t stream) {
    audiomp3_mp3file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    const mp_stream_p_t *stream_p = mp_get_stream_raise(stream, MP_STREAM_OP_READ);

    if (stream_p->is_text) {
        mp_raise_TypeError(MP_ERROR_TEXT("file must be a file opened in byte mode"));
    }
    common_hal_audiomp3_mp3file_set_file(self, stream);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiomp3_mp3file_set_file_obj, audiomp3_mp3file_obj_set_file);

static mp_obj_t audiomp3_mp3file_obj_open(mp_obj_t self_in, mp_obj_t path) {
    audiomp3_mp3file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);

    mp_obj_t file = mp_call_function_2(MP_OBJ_FROM_PTR(&mp_builtin_open_obj), path, MP_ROM_QSTR(MP_QSTR_rb));

    common_hal_audiomp3_mp3file_set_file(self, file);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiomp3_mp3file_open_obj, audiomp3_mp3file_obj_open);

MP_PROPERTY_GETSET(audiomp3_mp3file_file_obj,
    (mp_obj_t)&audiomp3_mp3file_get_file_obj,
    (mp_obj_t)&audiomp3_mp3file_set_file_obj);

static mp_obj_t audiomp3_mp3file_obj_get_rms_level(mp_obj_t self_in) {
    audiomp3_mp3file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    // Deviation from upstream: explicit cast. common_hal_..._get_rms_level
    // returns C `float`; mp_obj_new_float takes mp_float_t (double on every
    // build in this workspace, see docs/porting-plan.md's mp_float_t note),
    // so the implicit widening trips -Wdouble-promotion -Werror under
    // emscripten (phase 8d) though not unix/windows.
    return mp_obj_new_float((mp_float_t)common_hal_audiomp3_mp3file_get_rms_level(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiomp3_mp3file_get_rms_level_obj, audiomp3_mp3file_obj_get_rms_level);

MP_PROPERTY_GETTER(audiomp3_mp3file_rms_level_obj,
    (mp_obj_t)&audiomp3_mp3file_get_rms_level_obj);

static mp_obj_t audiomp3_mp3file_obj_get_samples_decoded(mp_obj_t self_in) {
    audiomp3_mp3file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return MP_OBJ_NEW_SMALL_INT(common_hal_audiomp3_mp3file_get_samples_decoded(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiomp3_mp3file_get_samples_decoded_obj, audiomp3_mp3file_obj_get_samples_decoded);

MP_PROPERTY_GETTER(audiomp3_mp3file_samples_decoded_obj,
    (mp_obj_t)&audiomp3_mp3file_get_samples_decoded_obj);

static const mp_rom_map_elem_t audiomp3_mp3file_locals_dict_table[] = {
    // Methods
    { MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&audiomp3_mp3file_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiomp3_mp3file_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&audiomp3_mp3file_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },

    // Properties
    { MP_ROM_QSTR(MP_QSTR_file), MP_ROM_PTR(&audiomp3_mp3file_file_obj) },
    { MP_ROM_QSTR(MP_QSTR_rms_level), MP_ROM_PTR(&audiomp3_mp3file_rms_level_obj) },
    { MP_ROM_QSTR(MP_QSTR_samples_decoded), MP_ROM_PTR(&audiomp3_mp3file_samples_decoded_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiomp3_mp3file_locals_dict, audiomp3_mp3file_locals_dict_table);

static const audiosample_p_t audiomp3_mp3file_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audiomp3_mp3file_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audiomp3_mp3file_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiomp3_mp3file_type,
    MP_QSTR_MP3Decoder,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiomp3_mp3file_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiomp3_mp3file_locals_dict,
    protocol, &audiomp3_mp3file_proto
    );
