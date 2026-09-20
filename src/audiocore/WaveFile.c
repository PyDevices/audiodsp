// Adapted from CircuitPython's shared-bindings/audiocore/WaveFile.c and
// shared-module/audiocore/WaveFile.c (upstream repo:
// https://github.com/adafruit/circuitpython, MIT). See WaveFile.h for why
// file I/O goes through MicroPython's generic stream protocol here instead
// of CP's direct FatFS calls -- everything else (WAV chunk parsing,
// double-buffered refill/get_buffer state machine, end-of-file padding) is
// an unchanged, mechanical port. Docstrings (`//|` lines) dropped.
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <string.h>

#include "audiocore/WaveFile.h"
#include "audiocore/__init__.h"
#include "cp_compat/argcheck.h"
#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/objproperty.h"
#include "shared/audioif_pump_lock.h"

#include "py/builtin.h"
#include "py/mperrno.h"
#include "py/runtime.h"
#include "py/stream.h"

// --- portable stream helpers (replace CP's f_read/f_lseek/f_tell) --------

static void wav_read_exactly(audioio_wavefile_obj_t *self, void *buf, size_t size) {
    int errcode = 0;
    mp_uint_t n = mp_stream_read_exactly(self->file, buf, size, &errcode);
    if (errcode != 0) {
        mp_raise_OSError(errcode);
    }
    if (n != size) {
        mp_raise_OSError(MP_EIO);
    }
}

static mp_off_t wav_seek(audioio_wavefile_obj_t *self, mp_off_t offset, int whence) {
    struct mp_stream_seek_t seek_s = { .offset = offset, .whence = whence };
    int errcode = 0;
    mp_uint_t res = self->file_stream_p->ioctl(self->file, MP_STREAM_SEEK, (uintptr_t)&seek_s, &errcode);
    if (res == MP_STREAM_ERROR) {
        mp_raise_OSError(errcode);
    }
    return seek_s.offset;
}

// --- from shared-module/audiocore/WaveFile.c ------------------------------

struct wave_format_chunk {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint16_t extra_params;
    uint16_t valid_bits_per_sample;
    uint32_t channel_mask;
    uint16_t extended_audio_format;
    uint8_t extended_guid[14];
};

void common_hal_audioio_wavefile_construct(audioio_wavefile_obj_t *self,
    mp_obj_t file,
    uint8_t *buffer,
    size_t buffer_size) {
    // Load the wave
    self->file = file;
    self->file_stream_p = mp_get_stream_raise(file, MP_STREAM_OP_READ | MP_STREAM_OP_IOCTL);
    uint8_t chunk_header[16];
    wav_seek(self, 0, MP_SEEK_SET);
    wav_read_exactly(self, chunk_header, 16);
    if (memcmp(chunk_header, "RIFF", 4) != 0 ||
        memcmp(chunk_header + 8, "WAVEfmt ", 8) != 0) {
        mp_arg_error_invalid(MP_QSTR_file);
    }
    uint32_t format_size;
    wav_read_exactly(self, &format_size, 4);
    if (format_size > sizeof(struct wave_format_chunk)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Invalid format chunk size"));
    }
    struct wave_format_chunk format;
    wav_read_exactly(self, &format, format_size);

    if ((format_size != 40 && format.audio_format != 1) ||
        format.num_channels > 2 ||
        format.bits_per_sample > 16 ||
        (format_size == 18 && format.extra_params != 0) ||
        (format_size == 40 &&
         (format.audio_format != 0xfffe ||
          format.extended_audio_format != 1 ||
          format.valid_bits_per_sample != format.bits_per_sample))) {
        mp_raise_ValueError(MP_ERROR_TEXT("Format not supported"));
    }
    // Get the sample_rate
    self->base.sample_rate = format.sample_rate;
    self->base.channel_count = format.num_channels;
    self->base.bits_per_sample = format.bits_per_sample;
    self->base.samples_signed = format.bits_per_sample > 8;
    self->base.max_buffer_length = 512;
    self->base.single_buffer = false;

    uint8_t chunk_tag[4];
    uint32_t chunk_length;
    bool found_data_chunk = false;

    while (!found_data_chunk) {
        wav_read_exactly(self, chunk_tag, 4);
        if (memcmp((uint8_t *)chunk_tag, "data", 4) == 0) {
            found_data_chunk = true;
        }

        wav_read_exactly(self, &chunk_length, 4);

        if (!found_data_chunk) {
            wav_seek(self, chunk_length, MP_SEEK_CUR);
        }
    }

    self->file_length = chunk_length;
    self->data_start = wav_seek(self, 0, MP_SEEK_CUR);

    // Try to allocate two buffers, one will be loaded from file and the other
    // DMAed to DAC.
    if (buffer_size) {
        self->len = buffer_size / 2;
        self->buffer = buffer;
        self->second_buffer = buffer + self->len;
    } else {
        self->len = 256;
        self->buffer = m_malloc(self->len);
        if (self->buffer == NULL) {
            common_hal_audioio_wavefile_deinit(self);
            m_malloc_fail(self->len);
        }

        self->second_buffer = m_malloc(self->len);
        if (self->second_buffer == NULL) {
            common_hal_audioio_wavefile_deinit(self);
            m_malloc_fail(self->len);
        }
    }
}

void common_hal_audioio_wavefile_deinit(audioio_wavefile_obj_t *self) {
    self->buffer = NULL;
    self->second_buffer = NULL;
    audiosample_mark_deinit(&self->base);
}

// A file-backed source cannot be pulled by a pump and never will be: it
// reads through the VFS from inside the pull, which re-enters the interpreter,
// and it raises OSError on a seek that fails (wav_seek above, the only raise
// the exception audit found at a node boundary). Both are fatal on a thread
// with no interpreter state to raise or allocate from.
//
// So it refuses, rather than being trusted not to be wired up. The design
// that replaces this is a prefetcher: the interpreter fills a ring and the
// pump drains it. See docs/spikes/live-audio-path-done.md, "prefetching
// sources".
void audioio_wavefile_reset_buffer(audioio_wavefile_obj_t *self,
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
    self->bytes_remaining = self->file_length;
    wav_seek(self, self->data_start, MP_SEEK_SET);
    self->read_count = 0;
    self->left_read_count = 0;
    self->right_read_count = 0;
}

audioio_get_buffer_result_t audioio_wavefile_get_buffer(audioio_wavefile_obj_t *self,
    bool single_channel_output,
    uint8_t channel,
    uint8_t **buffer,
    uint32_t *buffer_length) {
    if (audioif_pump_on_pump_thread()) {
        audioif_pump_fault_set(AUDIOIF_PUMP_FAULT_UNPUMPABLE);
        *buffer = NULL;
        *buffer_length = 0;
        return GET_BUFFER_ERROR;
    }
    if (!single_channel_output) {
        channel = 0;
    }

    uint32_t channel_read_count = self->left_read_count;
    if (channel == 1) {
        channel_read_count = self->right_read_count;
    }

    bool need_more_data = self->read_count == channel_read_count;

    if (self->bytes_remaining == 0 && need_more_data) {
        *buffer = NULL;
        *buffer_length = 0;
        return GET_BUFFER_DONE;
    }

    if (need_more_data) {
        uint32_t num_bytes_to_load = self->len;
        if (num_bytes_to_load > self->bytes_remaining) {
            num_bytes_to_load = self->bytes_remaining;
        }
        if (self->buffer_index % 2 == 1) {
            *buffer = self->second_buffer;
        } else {
            *buffer = self->buffer;
        }
        int errcode = 0;
        mp_uint_t length_read = mp_stream_read_exactly(self->file, *buffer, num_bytes_to_load, &errcode);
        if (errcode != 0 || length_read != num_bytes_to_load) {
            return GET_BUFFER_ERROR;
        }
        self->bytes_remaining -= length_read;
        // Pad the last buffer to word align it.
        if (self->bytes_remaining == 0 && length_read % sizeof(uint32_t) != 0) {
            uint32_t pad = length_read % sizeof(uint32_t);
            length_read += pad;
            if (self->base.bits_per_sample == 8) {
                for (uint32_t i = 0; i < pad; i++) {
                    ((uint8_t *)(*buffer))[length_read / sizeof(uint8_t) - i - 1] = 0x80;
                }
            } else if (self->base.bits_per_sample == 16) {
                // We know the buffer is aligned because we allocated it onto the heap ourselves.
                #pragma GCC diagnostic push
                #pragma GCC diagnostic ignored "-Wcast-align"
                ((int16_t *)(*buffer))[length_read / sizeof(int16_t) - 1] = 0;
                #pragma GCC diagnostic pop
            }
        }
        *buffer_length = length_read;
        if (self->buffer_index % 2 == 1) {
            self->second_buffer_length = length_read;
        } else {
            self->buffer_length = length_read;
        }
        self->buffer_index += 1;
        self->read_count += 1;
    }

    uint32_t buffers_back = self->read_count - 1 - channel_read_count;
    if ((self->buffer_index - buffers_back) % 2 == 0) {
        *buffer = self->second_buffer;
        *buffer_length = self->second_buffer_length;
    } else {
        *buffer = self->buffer;
        *buffer_length = self->buffer_length;
    }

    if (channel == 0) {
        self->left_read_count += 1;
    } else if (channel == 1) {
        self->right_read_count += 1;
        *buffer = *buffer + self->base.bits_per_sample / 8;
    }

    return self->bytes_remaining == 0 ? GET_BUFFER_DONE : GET_BUFFER_MORE_DATA;
}

// --- from shared-bindings/audiocore/WaveFile.c ----------------------------

static mp_obj_t audioio_wavefile_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 2, false);
    mp_obj_t arg = args[0];

    if (mp_obj_is_str(arg)) {
        arg = mp_call_function_2(MP_OBJ_FROM_PTR(&mp_builtin_open_obj), arg, MP_ROM_QSTR(MP_QSTR_rb));
    }

    // Deviation from upstream: CP requires mp_type_vfs_fat_fileio
    // specifically; this port accepts anything implementing the stream
    // protocol (see WaveFile.h), which mp_get_stream_raise enforces in
    // common_hal_audioio_wavefile_construct.
    uint8_t *buffer = NULL;
    size_t buffer_size = 0;
    if (n_args >= 2) {
        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise(args[1], &bufinfo, MP_BUFFER_WRITE);
        buffer = bufinfo.buf;
        buffer_size = mp_arg_validate_length_range(bufinfo.len, 8, 1024, MP_QSTR_buffer);
    }

    audioio_wavefile_obj_t *self = mp_obj_malloc(audioio_wavefile_obj_t, &audioio_wavefile_type);
    common_hal_audioio_wavefile_construct(self, arg, buffer, buffer_size);

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audioio_wavefile_deinit(mp_obj_t self_in) {
    audioio_wavefile_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_audioio_wavefile_deinit(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioio_wavefile_deinit_obj, audioio_wavefile_deinit);

static const mp_rom_map_elem_t audioio_wavefile_locals_dict_table[] = {
    // Methods
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audioio_wavefile_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },

    // Properties
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audioio_wavefile_locals_dict, audioio_wavefile_locals_dict_table);

static const audiosample_p_t audioio_wavefile_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audioio_wavefile_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audioio_wavefile_get_buffer,
};

// Deviation from upstream: adds `attr, cp_compat_attr` -- see
// cp_compat/objproperty.c for why a ported type's MP_PROPERTY_GETTER/GETSET
// locals_dict entries need this to actually be invoked on this port.
MP_DEFINE_CONST_OBJ_TYPE(
    audioio_wavefile_type,
    MP_QSTR_WaveFile,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audioio_wavefile_make_new,
    attr, cp_compat_attr,
    locals_dict, &audioio_wavefile_locals_dict,
    protocol, &audioio_wavefile_proto
    );
