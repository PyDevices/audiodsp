// audiomp3's decoder for CPython: Helix, driven as src/audiomp3/MP3Decoder.c
// drives it.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft
// SPDX-FileCopyrightText: Copyright (c) 2019 Jeff Epler for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// Each function below carries the name of the one it follows in
// src/audiomp3/MP3Decoder.c (itself ported from CircuitPython's
// shared-module/audiomp3/MP3Decoder.c). Keep the two in step: a change to
// how that file buffers, skips or resets changes the bytes a board decodes,
// and tests/test_cpython_audiomp3.py holds this side to the same hash.
//
// What is left out is what has no meaning here: the pump-thread guard, the
// pump lock, background callbacks (which on these builds run at once, inline,
// where this calls the same refill directly), and settimeout on the stream.

#include <math.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audiomp3/mp3_alloc.h"
#include "cpython/audiodsp_mp3.h"

#include "coder.h"
#include "mp3common.h"

#define MAX_BUFFER_LEN (MAX_NSAMP * MAX_NGRAN * MAX_NCHAN * sizeof(int16_t))

// buffers.c (compiled through src/audiomp3/mp3_buffers.c) takes the decoder's
// state from these. calloc rather than malloc: MicroPython's heap hands out
// cleared memory, and the decoder must start from the same state.
void *mp3_alloc(size_t sz) {
    return calloc(1, sz);
}

void mp3_free(void *ptr) {
    free(ptr);
}

struct audiodsp_mp3 {
    const audiodsp_mp3_io_t *io;
    void *stream;
    HMP3Decoder decoder;
    struct {
        uint8_t *buf;
        // Signed, as MP3Decoder.h's mp_int_t fields are: the arithmetic
        // below depends on it.
        ptrdiff_t size;
        ptrdiff_t read_off;
        ptrdiff_t write_off;
    } inbuf;
    int16_t *pcm_buffer[2];
    int buffer_index;
    bool eof;
    bool read_failed;
    int sample_rate;
    int channel_count;
    size_t max_buffer_length;
    uint32_t samples_decoded;
};

#define INPUT_BUFFER_AVAILABLE(i) ((i).write_off - (i).read_off)
#define INPUT_BUFFER_SPACE(i) ((i).size - INPUT_BUFFER_AVAILABLE(i))
#define INPUT_BUFFER_READ_PTR(i) ((i).buf + (i).read_off)
#define INPUT_BUFFER_CONSUME(i, n) ((i).read_off += (n))
#define INPUT_BUFFER_CLEAR(i) ((i).read_off = (i).write_off = 0)

#define READ_PTR(self) (INPUT_BUFFER_READ_PTR(self->inbuf))
#define BYTES_LEFT(self) (INPUT_BUFFER_AVAILABLE(self->inbuf))
#define CONSUME(self, n) (INPUT_BUFFER_CONSUME(self->inbuf, n))

// mp3file_update_inbuf_always. A failed read ends the stream, as the OSError
// MP3Decoder raises does; the caller reports it once the decoder is back.
static bool mp3_update_inbuf_always(audiodsp_mp3_t *self) {
    if (self->eof || INPUT_BUFFER_SPACE(self->inbuf) == 0) {
        return INPUT_BUFFER_AVAILABLE(self->inbuf) > 0;
    }
    if (self->inbuf.read_off) {
        memmove(self->inbuf.buf, INPUT_BUFFER_READ_PTR(self->inbuf), INPUT_BUFFER_AVAILABLE(self->inbuf));
        self->inbuf.write_off -= self->inbuf.read_off;
        self->inbuf.read_off = 0;
    }
    for (ptrdiff_t to_read; !self->eof && (to_read = INPUT_BUFFER_SPACE(self->inbuf)) > 0;) {
        long n_read = self->io->read(self->stream, self->inbuf.buf + self->inbuf.write_off, (size_t)to_read);
        if (n_read == AUDIODSP_MP3_EAGAIN) {
            break;
        }
        if (n_read < 0) {
            self->eof = true;
            self->read_failed = true;
            break;
        }
        if (n_read == 0) {
            self->eof = true;
        }
        self->inbuf.write_off += n_read;
    }
    return INPUT_BUFFER_AVAILABLE(self->inbuf) > 0;
}

// mp3file_update_inbuf_half
static bool mp3_update_inbuf_half(audiodsp_mp3_t *self) {
    if (INPUT_BUFFER_SPACE(self->inbuf) < self->inbuf.size / 2) {
        return true;
    }
    return mp3_update_inbuf_always(self);
}

static int64_t mp3_seek(audiodsp_mp3_t *self, int64_t offset, int whence) {
    return self->io->seek ? self->io->seek(self->stream, offset, whence) : -1;
}

// mp3file_skip_id3v2
static void mp3_skip_id3v2(audiodsp_mp3_t *self) {
    mp3_update_inbuf_half(self);
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
    size += 10;
    uint32_t to_consume = MIN(size, BYTES_LEFT(self));
    CONSUME(self, to_consume);
    size -= to_consume;

    int64_t before = mp3_seek(self, 0, SEEK_CUR);
    if (before >= 0 && mp3_seek(self, size, SEEK_CUR) == before + size) {
        return;
    }
    while (size > 0 && !self->eof) {
        mp3_update_inbuf_always(self);
        to_consume = MIN(size, BYTES_LEFT(self));
        CONSUME(self, to_consume);
        size -= to_consume;
    }
}

// mp3file_find_sync_word
static bool mp3_find_sync_word(audiodsp_mp3_t *self) {
    do {
        mp3_update_inbuf_half(self);
        int offset = MP3FindSyncWord(READ_PTR(self), (int)BYTES_LEFT(self));
        if (offset >= 0) {
            CONSUME(self, offset);
            mp3_update_inbuf_half(self);
            return true;
        }
        CONSUME(self, MAX(0, BYTES_LEFT(self) - 16));
    } while (!self->eof);
    return false;
}

// mp3file_get_next_frame_info
static bool mp3_get_next_frame_info(audiodsp_mp3_t *self, MP3FrameInfo *fi) {
    int err;
    do {
        err = MP3GetNextFrameInfo(self->decoder, fi, READ_PTR(self));
        if (err == ERR_MP3_NONE) {
            break;
        }
        CONSUME(self, 1);
        mp3_find_sync_word(self);
    } while (!self->eof);
    return err == ERR_MP3_NONE;
}

// mp3file_reset_decoder
static void mp3_reset_decoder(audiodsp_mp3_t *self) {
    memset(self->pcm_buffer[0], 0, MAX_BUFFER_LEN);
    memset(self->pcm_buffer[1], 0, MAX_BUFFER_LEN);
    struct _MP3DecInfo *decoder = self->decoder;
    memset(decoder->FrameHeaderPS, 0, sizeof(FrameHeader));
    memset(decoder->SideInfoPS, 0, sizeof(SideInfo));
    memset(decoder->ScaleFactorInfoPS, 0, sizeof(ScaleFactorInfo));
    memset(decoder->HuffmanInfoPS, 0, sizeof(HuffmanInfo));
    memset(decoder->DequantInfoPS, 0, sizeof(DequantInfo));
    memset(decoder->IMDCTInfoPS, 0, sizeof(IMDCTInfo));
    memset(decoder->SubbandInfoPS, 0, sizeof(SubbandInfo));
}

// common_hal_audiomp3_mp3file_construct and _set_file, with the default
// buffers. A caller-supplied buffer there only changes where the memory
// lives, and the input buffer's size, which `inbuf_size` stands for.
audiodsp_mp3_t *audiodsp_mp3_open(const audiodsp_mp3_io_t *io, void *stream,
    size_t inbuf_size, int *error) {
    int err = AUDIODSP_MP3_ENOMEM;
    audiodsp_mp3_t *self = calloc(1, sizeof(*self));
    if (self == NULL) {
        goto fail;
    }
    self->io = io;
    self->stream = stream;
    self->inbuf.size = (ptrdiff_t)inbuf_size;
    self->inbuf.buf = malloc(inbuf_size);
    self->pcm_buffer[0] = malloc(MAX_BUFFER_LEN);
    self->pcm_buffer[1] = malloc(MAX_BUFFER_LEN);
    if (!self->inbuf.buf || !self->pcm_buffer[0] || !self->pcm_buffer[1]) {
        goto fail;
    }
    self->decoder = MP3InitDecoder();
    if (self->decoder == NULL) {
        goto fail;
    }

    mp3_update_inbuf_half(self);
    mp3_find_sync_word(self);
    mp3_reset_decoder(self);
    MP3FrameInfo fi;
    bool result = mp3_get_next_frame_info(self, &fi);
    if (self->read_failed) {
        err = AUDIODSP_MP3_EREAD;
        goto fail;
    }
    if (!result) {
        err = AUDIODSP_MP3_EPARSE;
        goto fail;
    }
    self->sample_rate = fi.samprate;
    self->channel_count = fi.nChans;
    self->max_buffer_length = (size_t)fi.outputSamps * sizeof(int16_t);
    self->samples_decoded = 0;
    return self;

fail:
    audiodsp_mp3_close(self);
    if (error) {
        *error = err;
    }
    return NULL;
}

void audiodsp_mp3_close(audiodsp_mp3_t *self) {
    if (self == NULL) {
        return;
    }
    if (self->decoder) {
        MP3FreeDecoder(self->decoder);
    }
    free(self->inbuf.buf);
    free(self->pcm_buffer[0]);
    free(self->pcm_buffer[1]);
    free(self);
}

int audiodsp_mp3_sample_rate(const audiodsp_mp3_t *self) {
    return self->sample_rate;
}

int audiodsp_mp3_channels(const audiodsp_mp3_t *self) {
    return self->channel_count;
}

size_t audiodsp_mp3_buffer_bytes(const audiodsp_mp3_t *self) {
    return self->max_buffer_length;
}

// audiomp3_mp3file_reset_buffer
int audiodsp_mp3_rewind(audiodsp_mp3_t *self) {
    if (self->eof && mp3_seek(self, 0, SEEK_SET) == 0) {
        INPUT_BUFFER_CLEAR(self->inbuf);
        self->eof = false;
        self->read_failed = false;
        self->samples_decoded = 0;
        mp3_reset_decoder(self);
        mp3_skip_id3v2(self);
        mp3_find_sync_word(self);
    }
    return self->read_failed ? AUDIODSP_MP3_EREAD : 0;
}

// audiomp3_mp3file_get_buffer, for interleaved output.
static int mp3_decode(audiodsp_mp3_t *self, const int16_t **pcm, size_t *nbytes) {
    size_t frame_buffer_size_bytes = self->max_buffer_length;
    *nbytes = frame_buffer_size_bytes;

    self->buffer_index = !self->buffer_index;
    int16_t *buffer = self->pcm_buffer[self->buffer_index];
    *pcm = buffer;

    mp3_skip_id3v2(self);
    if (!mp3_find_sync_word(self)) {
        memset(buffer, 0, self->max_buffer_length);
        *nbytes = 0;
        return self->eof ? AUDIODSP_MP3_DONE : AUDIODSP_MP3_ERROR;
    }
    int bytes_left = (int)BYTES_LEFT(self);
    uint8_t *inbuf = READ_PTR(self);
    int err = MP3Decode(self->decoder, &inbuf, &bytes_left, buffer, 0);
    if (err != ERR_MP3_INDATA_UNDERFLOW) {
        CONSUME(self, BYTES_LEFT(self) - bytes_left);
    }
    if (err) {
        memset(buffer, 0, frame_buffer_size_bytes);
        if (self->eof || (err != ERR_MP3_INDATA_UNDERFLOW && err != ERR_MP3_MAINDATA_UNDERFLOW)) {
            *nbytes = 0;
            bool underflow = (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW);
            self->eof = true;
            return underflow ? AUDIODSP_MP3_DONE : AUDIODSP_MP3_ERROR;
        }
    }

    self->samples_decoded += (uint32_t)(frame_buffer_size_bytes / sizeof(int16_t));

    mp3_skip_id3v2(self);
    int result = mp3_find_sync_word(self) ? AUDIODSP_MP3_MORE : AUDIODSP_MP3_DONE;

    // The background refill MP3Decoder queues here runs at once on the
    // builds this is held to, so it is a plain call.
    if (INPUT_BUFFER_SPACE(self->inbuf) > 512 && !self->eof) {
        mp3_update_inbuf_always(self);
    }
    return result;
}

int audiodsp_mp3_decode(audiodsp_mp3_t *self, const int16_t **pcm, size_t *nbytes) {
    int result = mp3_decode(self, pcm, nbytes);
    return self->read_failed ? AUDIODSP_MP3_EREAD : result;
}

// common_hal_audiomp3_mp3file_get_rms_level
double audiodsp_mp3_rms_level(const audiodsp_mp3_t *self) {
    double sumsq = 0.0;
    const int16_t *buffer = self->pcm_buffer[self->buffer_index];
    for (size_t i = 0; i < self->max_buffer_length / sizeof(int16_t); i++) {
        sumsq += (double)buffer[i] * buffer[i];
    }
    return sqrt(sumsq) / (double)(self->max_buffer_length / sizeof(int16_t));
}

uint32_t audiodsp_mp3_samples_decoded(const audiodsp_mp3_t *self) {
    return self->samples_decoded;
}
