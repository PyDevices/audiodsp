// audiovorbis.VorbisDecoder for CircuitPython: the decoding and the
// audiosample protocol. The MicroPython twin is audiodsp's
// src/audiovorbis/VorbisDecoder.c, and the two differ only where the
// interpreters do (no pump lock here, CircuitPython's non-scanned allocator
// for the audio buffers). Modelled on CircuitPython's
// shared-module/audiomp3/MP3Decoder.c (https://github.com/adafruit/circuitpython,
// MIT). CircuitPython itself has no Vorbis module; this one is PyDevices'.
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2019 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "py/mperrno.h"
#include "py/nlr.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "py/stream.h"

#include "shared-bindings/audiocore/__init__.h"
#include "shared-bindings/audiovorbis/VorbisDecoder.h"

// The optional buffer is used when each half holds at least this much.
#define AUDIOVORBIS_MIN_HALF_BYTES (256)

// The C stack a call into Tremor may take below the caller, with room to
// spare: about 1 KB of fixed frames on RISC-V, up to 2 KB of scratch a packet
// sizes on the stack, and the stream's own read underneath (docs/vorbis.md).
// Tremor doesn't check its depth, so this checks first: a Python program deep
// enough to run the stack out gets an error instead of a crashed board.
#define AUDIOVORBIS_STACK_NEEDED (6 * 1024)

static bool vorbis_stack_ok(void) {
    #if MICROPY_STACK_CHECK
    return mp_stack_usage() + AUDIOVORBIS_STACK_NEEDED <= MP_STATE_THREAD(stack_limit);
    #else
    return true;
    #endif
}

// --- the stream, as audiodsp_vorbis_io_t ------------------------------------
//
// Tremor reads and seeks through these from deep inside a decode. A stream
// whose read raises (a Python-level stream, a socket that times out) must not
// longjmp through Tremor, so each call runs under its own nlr frame and an
// exception becomes a failed read, kept in stream_error (or stream_errcode,
// for a stream that reports an errno instead of raising).

static long vorbis_stream_read(void *self_in, void *buf, size_t len) {
    audiovorbis_vorbisdecoder_obj_t *self = self_in;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        int errcode = 0;
        const mp_stream_p_t *stream_p = mp_get_stream(self->stream);
        mp_uint_t got = stream_p->read(self->stream, buf, len, &errcode);
        nlr_pop();
        if (got == MP_STREAM_ERROR) {
            if (mp_is_nonblocking_error(errcode)) {
                return 0;
            }
            // An errno, not an exception object: this can run inside a
            // pull, where nothing may allocate.
            self->stream_errcode = errcode;
            return -1;
        }
        return (long)got;
    }
    self->stream_error = MP_OBJ_FROM_PTR(nlr.ret_val);
    return -1;
}

static int vorbis_stream_seek(void *self_in, int64_t offset, int whence) {
    audiovorbis_vorbisdecoder_obj_t *self = self_in;
    const mp_stream_p_t *stream_p = mp_get_stream(self->stream);
    if (stream_p->ioctl == NULL) {
        return -1;
    }
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        int errcode = 0;
        struct mp_stream_seek_t seek_s;
        seek_s.offset = (mp_off_t)offset;
        seek_s.whence = whence;
        mp_uint_t res = stream_p->ioctl(self->stream, MP_STREAM_SEEK,
            (mp_uint_t)(uintptr_t)&seek_s, &errcode);
        nlr_pop();
        return res == MP_STREAM_ERROR ? -1 : 0;
    }
    return -1;
}

static long vorbis_stream_tell(void *self_in) {
    audiovorbis_vorbisdecoder_obj_t *self = self_in;
    const mp_stream_p_t *stream_p = mp_get_stream(self->stream);
    if (stream_p->ioctl == NULL) {
        return -1;
    }
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        int errcode = 0;
        struct mp_stream_seek_t seek_s;
        seek_s.offset = 0;
        seek_s.whence = SEEK_CUR;
        mp_uint_t res = stream_p->ioctl(self->stream, MP_STREAM_SEEK,
            (mp_uint_t)(uintptr_t)&seek_s, &errcode);
        nlr_pop();
        return res == MP_STREAM_ERROR ? -1 : (long)seek_s.offset;
    }
    return -1;
}

static const audiodsp_vorbis_io_t vorbis_stream_io = {
    vorbis_stream_read, vorbis_stream_seek, vorbis_stream_tell,
};

// --- construction ------------------------------------------------------------

static void vorbis_raise_open_error(audiovorbis_vorbisdecoder_obj_t *self, int err) {
    mp_obj_t pending = self->stream_error;
    int errcode = self->stream_errcode;
    self->stream_error = MP_OBJ_NULL;
    self->stream_errcode = 0;
    if (pending != MP_OBJ_NULL) {
        nlr_raise(pending);
    }
    if (errcode != 0) {
        mp_raise_OSError(errcode);
    }
    if (err == AUDIODSP_VORBIS_ENOMEM) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("Couldn't allocate decoder"));
    }
    mp_raise_msg_varg(&mp_type_RuntimeError,
        MP_ERROR_TEXT("Failed to parse Vorbis file: %s"), audiodsp_vorbis_strerror(err));
}

void common_hal_audiovorbis_vorbisdecoder_construct(audiovorbis_vorbisdecoder_obj_t *self,
    mp_obj_t stream, uint8_t *buffer, size_t buffer_size) {
    self->stream = mp_const_none;
    self->stream_error = MP_OBJ_NULL;
    if ((intptr_t)buffer & 1) {
        buffer += 1;
        buffer_size -= 1;
    }
    // Whole stereo frames in each half, so a mono file and a stereo one can
    // share the same buffer.
    size_t half = (buffer_size / 2) & ~(size_t)3;
    if (buffer != NULL && half >= AUDIOVORBIS_MIN_HALF_BYTES) {
        self->pcm_buffer[0] = (int16_t *)(void *)buffer;
        self->pcm_buffer[1] = (int16_t *)(void *)(buffer + half);
        self->buffer_frames = half / (2 * sizeof(int16_t));
    } else {
        // Room for a stereo stream; a mono one uses the first half of each.
        size_t bytes = AUDIOVORBIS_FRAMES * 2 * sizeof(int16_t);
        self->pcm_buffer[0] = m_malloc_without_collect(bytes);
        self->pcm_buffer[1] = m_malloc_without_collect(bytes);
        self->buffer_frames = AUDIOVORBIS_FRAMES;
    }
    common_hal_audiovorbis_vorbisdecoder_set_file(self, stream);
}

void common_hal_audiovorbis_vorbisdecoder_set_file(audiovorbis_vorbisdecoder_obj_t *self,
    mp_obj_t stream) {
    // Open the new stream before touching anything a pull reads: opening
    // reads the stream, allocates and can raise.
    if (!vorbis_stack_ok()) {
        mp_raise_msg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("not enough stack to open a Vorbis file here"));
    }
    mp_obj_t old_stream = self->stream;
    self->stream = stream;
    self->stream_error = MP_OBJ_NULL;
    self->stream_errcode = 0;
    int err = 0;
    audiodsp_vorbis_t *decoder = audiodsp_vorbis_open(&vorbis_stream_io, self, &err);
    if (decoder == NULL) {
        self->stream = old_stream;
        vorbis_raise_open_error(self, err);
    }
    int channels = audiodsp_vorbis_channels(decoder);
    long rate = audiodsp_vorbis_sample_rate(decoder);
    if (channels > 2) {
        audiodsp_vorbis_close(decoder);
        self->stream = old_stream;
        mp_raise_ValueError(MP_ERROR_TEXT("only mono and stereo Vorbis files can play"));
    }

    audiodsp_vorbis_t *old = self->decoder;
    self->decoder = decoder;
    self->base.sample_rate = (uint32_t)rate;
    self->base.channel_count = (uint8_t)channels;
    self->base.single_buffer = false;
    self->base.bits_per_sample = 16;
    self->base.samples_signed = true;
    self->base.max_buffer_length = self->buffer_frames * channels * sizeof(int16_t);
    self->buffer_bytes[0] = self->buffer_bytes[1] = 0;
    self->buffer_index = 0;
    self->eof = false;
    self->started = false;
    self->other_channel = -1;
    self->samples_decoded = 0;

    // The old decoder read the old stream through `self`; it is closed here,
    // after the swap, and closing reads nothing.
    audiodsp_vorbis_close(old);
}

void common_hal_audiovorbis_vorbisdecoder_deinit(audiovorbis_vorbisdecoder_obj_t *self) {
    audiosample_mark_deinit(&self->base);
    audiodsp_vorbis_close(self->decoder);
    self->decoder = NULL;
    self->pcm_buffer[0] = NULL;
    self->pcm_buffer[1] = NULL;
    self->stream = mp_const_none;
    self->stream_error = MP_OBJ_NULL;
    self->samples_decoded = 0;
}

// --- the audiosample protocol ------------------------------------------------

void audiovorbis_vorbisdecoder_reset_buffer(audiovorbis_vorbisdecoder_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    if (single_channel_output && channel == 1) {
        return;
    }
    if (self->decoder == NULL || !self->started || !vorbis_stack_ok()) {
        return;
    }
    if (audiodsp_vorbis_seek(self->decoder, 0) == 0) {
        self->eof = false;
        self->started = false;
        self->samples_decoded = 0;
        self->other_channel = -1;
    }
}

audioio_get_buffer_result_t audiovorbis_vorbisdecoder_get_buffer(audiovorbis_vorbisdecoder_obj_t *self,
    bool single_channel_output, uint8_t channel,
    uint8_t **bufptr, uint32_t *buffer_length) {
    if (self->decoder == NULL || !vorbis_stack_ok()) {
        *bufptr = NULL;
        *buffer_length = 0;
        return GET_BUFFER_ERROR;
    }
    if (!single_channel_output) {
        channel = 0;
    }

    // The second channel of a stereo buffer handed out one channel at a time.
    if (channel == self->other_channel) {
        *bufptr = (uint8_t *)(self->pcm_buffer[self->other_buffer_index] + channel);
        *buffer_length = self->buffer_bytes[self->other_buffer_index];
        self->other_channel = -1;
        self->samples_decoded += *buffer_length / sizeof(int16_t);
        return self->eof ? GET_BUFFER_DONE : GET_BUFFER_MORE_DATA;
    }

    self->buffer_index = !self->buffer_index;
    self->other_channel = 1 - channel;
    self->other_buffer_index = self->buffer_index;
    int16_t *buffer = self->pcm_buffer[self->buffer_index];
    *bufptr = (uint8_t *)buffer;

    const uint32_t channels = self->base.channel_count;
    uint32_t frames = 0;
    bool failed = false;
    while (!self->eof && frames < self->buffer_frames) {
        long got = audiodsp_vorbis_read(self->decoder, buffer + frames * channels,
            self->buffer_frames - frames);
        if (got > 0) {
            frames += (uint32_t)got;
            self->started = true;
        } else {
            // 0 is the end; an error ends the playback too, with what was
            // decoded before it.
            self->eof = true;
            failed = got < 0;
        }
    }
    uint32_t bytes = frames * channels * sizeof(int16_t);
    self->buffer_bytes[self->buffer_index] = bytes;
    *buffer_length = bytes;
    self->samples_decoded += bytes / sizeof(int16_t);
    if (failed && frames == 0) {
        return GET_BUFFER_ERROR;
    }
    return self->eof ? GET_BUFFER_DONE : GET_BUFFER_MORE_DATA;
}

mp_float_t common_hal_audiovorbis_vorbisdecoder_get_rms_level(audiovorbis_vorbisdecoder_obj_t *self) {
    mp_float_t sumsq = MICROPY_FLOAT_CONST(0.0);
    int16_t *buffer = self->pcm_buffer[self->buffer_index];
    size_t samples = self->buffer_bytes[self->buffer_index] / sizeof(int16_t);
    if (samples == 0) {
        return MICROPY_FLOAT_CONST(0.0);
    }
    for (size_t i = 0; i < samples; i++) {
        sumsq += (mp_float_t)buffer[i] * (mp_float_t)buffer[i];
    }
    return MICROPY_FLOAT_C_FUN(sqrt)(sumsq) / (mp_float_t)samples;
}

uint32_t common_hal_audiovorbis_vorbisdecoder_get_samples_decoded(audiovorbis_vorbisdecoder_obj_t *self) {
    return self->samples_decoded;
}

