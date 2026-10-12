// audiovorbis.VorbisDecoder: an Ogg Vorbis file as an audio sample.
//
// Modelled on CircuitPython's audiomp3.MP3Decoder (this repo's
// src/audiomp3/MP3Decoder.c, from shared-bindings/audiomp3/MP3Decoder.c and
// shared-module/audiomp3/MP3Decoder.c, https://github.com/adafruit/circuitpython,
// MIT): the same constructor, properties and methods, and the same answers to
// the audiosample protocol, so either decoder plays wherever the other does.
// CircuitPython itself has no Vorbis module; this one is PyDevices'.
//
// What differs from MP3Decoder, and why:
//
// - The decoding is shared/audiodsp_vorbis.h (Xiph's Tremor), which pulls
//   bytes through callbacks rather than from an input buffer this object
//   fills, so there is no input buffer and the optional `buffer` holds only
//   the decoded audio.
// - A rewind (reset_buffer) goes back to the start whenever anything has
//   been played, not only after the end. MP3Decoder skips the rewind before
//   its end, so a decoder played halfway and played again carries on.
// - A stream that raises inside a read during playback ends the playback
//   with GET_BUFFER_ERROR instead of raising: the pull runs under the pump
//   lock, and nothing may raise while it is held (shared/audiodsp_pump_lock.h).
//   Opening a file still raises the stream's own exception.
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2019 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "py/builtin.h"
#include "py/mperrno.h"
#include "py/nlr.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "py/stream.h"

#include "audiocore/__init__.h"
#include "audiovorbis/VorbisDecoder.h"
#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/objproperty.h"
#include "shared/audiodsp_pump_lock.h"

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
        self->pcm_buffer[0] = m_malloc(bytes);
        self->pcm_buffer[1] = m_malloc(bytes);
        self->buffer_frames = AUDIOVORBIS_FRAMES;
    }
    common_hal_audiovorbis_vorbisdecoder_set_file(self, stream);
}

void common_hal_audiovorbis_vorbisdecoder_set_file(audiovorbis_vorbisdecoder_obj_t *self,
    mp_obj_t stream) {
    // Open the new stream before touching anything a pull reads: opening
    // reads the stream, allocates and can raise, none of which may happen
    // under the lock.
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

    audiodsp_pump_lock_acquire();
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
    audiodsp_pump_lock_release();

    // The old decoder read the old stream through `self`; it is closed here,
    // after the swap, and closing reads nothing.
    audiodsp_vorbis_close(old);
}

void common_hal_audiovorbis_vorbisdecoder_deinit(audiovorbis_vorbisdecoder_obj_t *self) {
    // The whole body under the lock, as for MP3Decoder: a pull let in by the
    // funnel's guard must not find the decoder half torn down. Closing frees
    // with the C library's allocator, never the GC, so it may run here.
    audiodsp_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    audiodsp_vorbis_close(self->decoder);
    self->decoder = NULL;
    self->pcm_buffer[0] = NULL;
    self->pcm_buffer[1] = NULL;
    self->stream = mp_const_none;
    self->stream_error = MP_OBJ_NULL;
    self->samples_decoded = 0;
    audiodsp_pump_lock_release();
}

// --- the audiosample protocol ------------------------------------------------
//
// Like audiomp3, this reads its stream from inside the pull, so it is refused
// on the pump's thread: an Ogg file belongs behind a buffer the interpreter
// fills.

void audiovorbis_vorbisdecoder_reset_buffer(audiovorbis_vorbisdecoder_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    if (audiodsp_pump_on_pump_thread()) {
        audiodsp_pump_fault_set(AUDIODSP_PUMP_FAULT_UNPUMPABLE);
        return;
    }
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
    if (audiodsp_pump_on_pump_thread()) {
        audiodsp_pump_fault_set(AUDIODSP_PUMP_FAULT_UNPUMPABLE);
        *bufptr = NULL;
        *buffer_length = 0;
        return GET_BUFFER_ERROR;
    }
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

// --- Python bindings ---------------------------------------------------------

static mp_obj_t vorbis_check_stream(mp_obj_t stream) {
    if (mp_obj_is_str(stream)) {
        stream = mp_call_function_2(MP_OBJ_FROM_PTR(&mp_builtin_open_obj), stream, MP_ROM_QSTR(MP_QSTR_rb));
    }
    const mp_stream_p_t *stream_p = mp_get_stream_raise(stream, MP_STREAM_OP_READ);
    if (stream_p->is_text) {
        mp_raise_TypeError(MP_ERROR_TEXT("file must be a file opened in byte mode"));
    }
    return stream;
}

static mp_obj_t audiovorbis_vorbisdecoder_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 2, false);
    mp_obj_t stream = vorbis_check_stream(args[0]);

    uint8_t *buffer = NULL;
    size_t buffer_size = 0;
    if (n_args >= 2) {
        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise(args[1], &bufinfo, MP_BUFFER_WRITE);
        buffer = bufinfo.buf;
        buffer_size = bufinfo.len;
    }

    audiovorbis_vorbisdecoder_obj_t *self = mp_obj_malloc_with_finaliser(audiovorbis_vorbisdecoder_obj_t, &audiovorbis_vorbisdecoder_type);
    // mp_obj_malloc_with_finaliser doesn't zero what follows the base.
    memset((uint8_t *)self + sizeof(mp_obj_base_t), 0, sizeof(*self) - sizeof(mp_obj_base_t));
    common_hal_audiovorbis_vorbisdecoder_construct(self, stream, buffer, buffer_size);

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audiovorbis_vorbisdecoder_deinit(mp_obj_t self_in) {
    audiovorbis_vorbisdecoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audiovorbis_vorbisdecoder_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiovorbis_vorbisdecoder_deinit_obj, audiovorbis_vorbisdecoder_deinit);

static void check_for_deinit(audiovorbis_vorbisdecoder_obj_t *self) {
    // One word read under the lock and the raise outside it, as in
    // MP3Decoder: nothing that can longjmp runs while the lock is held.
    audiodsp_pump_lock_acquire();
    const bool released = audiosample_deinited(&self->base);
    audiodsp_pump_lock_release();
    if (released) {
        audiosample_check_for_deinit(&self->base);
    }
}

static mp_obj_t audiovorbis_vorbisdecoder_obj_get_file(mp_obj_t self_in) {
    audiovorbis_vorbisdecoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return self->stream;
}
MP_DEFINE_CONST_FUN_OBJ_1(audiovorbis_vorbisdecoder_get_file_obj, audiovorbis_vorbisdecoder_obj_get_file);

static mp_obj_t audiovorbis_vorbisdecoder_obj_set_file(mp_obj_t self_in, mp_obj_t stream) {
    audiovorbis_vorbisdecoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    const mp_stream_p_t *stream_p = mp_get_stream_raise(stream, MP_STREAM_OP_READ);
    if (stream_p->is_text) {
        mp_raise_TypeError(MP_ERROR_TEXT("file must be a file opened in byte mode"));
    }
    common_hal_audiovorbis_vorbisdecoder_set_file(self, stream);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiovorbis_vorbisdecoder_set_file_obj, audiovorbis_vorbisdecoder_obj_set_file);

static mp_obj_t audiovorbis_vorbisdecoder_obj_open(mp_obj_t self_in, mp_obj_t path) {
    audiovorbis_vorbisdecoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    mp_obj_t file = mp_call_function_2(MP_OBJ_FROM_PTR(&mp_builtin_open_obj), path, MP_ROM_QSTR(MP_QSTR_rb));
    common_hal_audiovorbis_vorbisdecoder_set_file(self, file);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiovorbis_vorbisdecoder_open_obj, audiovorbis_vorbisdecoder_obj_open);

MP_PROPERTY_GETSET(audiovorbis_vorbisdecoder_file_obj,
    (mp_obj_t)&audiovorbis_vorbisdecoder_get_file_obj,
    (mp_obj_t)&audiovorbis_vorbisdecoder_set_file_obj);

static mp_obj_t audiovorbis_vorbisdecoder_obj_get_rms_level(mp_obj_t self_in) {
    audiovorbis_vorbisdecoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_float(common_hal_audiovorbis_vorbisdecoder_get_rms_level(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiovorbis_vorbisdecoder_get_rms_level_obj, audiovorbis_vorbisdecoder_obj_get_rms_level);

MP_PROPERTY_GETTER(audiovorbis_vorbisdecoder_rms_level_obj,
    (mp_obj_t)&audiovorbis_vorbisdecoder_get_rms_level_obj);

static mp_obj_t audiovorbis_vorbisdecoder_obj_get_samples_decoded(mp_obj_t self_in) {
    audiovorbis_vorbisdecoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_int_from_uint(common_hal_audiovorbis_vorbisdecoder_get_samples_decoded(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiovorbis_vorbisdecoder_get_samples_decoded_obj, audiovorbis_vorbisdecoder_obj_get_samples_decoded);

MP_PROPERTY_GETTER(audiovorbis_vorbisdecoder_samples_decoded_obj,
    (mp_obj_t)&audiovorbis_vorbisdecoder_get_samples_decoded_obj);

static const mp_rom_map_elem_t audiovorbis_vorbisdecoder_locals_dict_table[] = {
    // Methods
    { MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&audiovorbis_vorbisdecoder_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiovorbis_vorbisdecoder_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&audiovorbis_vorbisdecoder_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },

    // Properties
    { MP_ROM_QSTR(MP_QSTR_file), MP_ROM_PTR(&audiovorbis_vorbisdecoder_file_obj) },
    { MP_ROM_QSTR(MP_QSTR_rms_level), MP_ROM_PTR(&audiovorbis_vorbisdecoder_rms_level_obj) },
    { MP_ROM_QSTR(MP_QSTR_samples_decoded), MP_ROM_PTR(&audiovorbis_vorbisdecoder_samples_decoded_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audiovorbis_vorbisdecoder_locals_dict, audiovorbis_vorbisdecoder_locals_dict_table);

static const audiosample_p_t audiovorbis_vorbisdecoder_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audiovorbis_vorbisdecoder_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audiovorbis_vorbisdecoder_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiovorbis_vorbisdecoder_type,
    MP_QSTR_VorbisDecoder,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiovorbis_vorbisdecoder_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiovorbis_vorbisdecoder_locals_dict,
    protocol, &audiovorbis_vorbisdecoder_proto
    );
