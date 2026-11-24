// audiocore module table. New code (not a port): CircuitPython wires its
// module globals through its own build-time manifest system instead of a
// single registration file like this one.
//
// get_buffer/reset_buffer/get_structure are ported from CircuitPython's
// shared-bindings/audiocore/__init__.c, where they're gated behind
// CIRCUITPY_AUDIOCORE_DEBUG (docs-hidden, off in CP's default config).
// Unconditional here: this is exactly the primitive
// the oracle-diffing test strategy needs -- a Python-level
// way to pull raw PCM out of *any* audiosample (RawSample, WaveFile, and
// every other audiosample-shaped type) without needing a real
// output device or a Mixer wired up yet.
//
// SPDX-FileCopyrightText: Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include <string.h>

#include "audiocore/RawSample.h"
#include "audiocore/WaveFile.h"
#include "audiocore/__init__.h"
#include "cp_compat/audiodsp_build.h"

#include "py/obj.h"
#include "py/runtime.h"

// The funnel below these two stopped raising: a pull may not, because the
// raise dies inside gc_alloc on a thread with no interpreter to allocate
// from. So the two checks it used to do happen HERE instead, where raising is
// free, and these two keep the exceptions they have always promised.
static void audiocore_check(mp_obj_t sample_in) {
    audiosample_check_for_deinit(audiosample_check(sample_in));
}

// The two optional arguments are the ones a host passes down the graph:
// whether it wants one channel of a stereo sample, and which. The CPython
// twin takes them too, so a node written for one runs on the other.
static bool audiocore_single(size_t n_args, const mp_obj_t *args) {
    return n_args > 1 && mp_obj_is_true(args[1]);
}

static uint8_t audiocore_channel(size_t n_args, const mp_obj_t *args) {
    return n_args > 2 ? (uint8_t)mp_obj_get_int(args[2]) : 0;
}

static mp_obj_t audiocore_get_buffer(size_t n_args, const mp_obj_t *args) {
    mp_obj_t sample_in = args[0];
    audiocore_check(sample_in);
    uint8_t *buffer = NULL;
    uint32_t buffer_length = 0;
    audioio_get_buffer_result_t gbr = audiosample_get_buffer(sample_in,
        audiocore_single(n_args, args), audiocore_channel(n_args, args), &buffer, &buffer_length);

    mp_obj_t result[2] = { mp_obj_new_int_from_uint(gbr), mp_const_none };

    if (gbr != GET_BUFFER_ERROR) {
        // copies the data because the gc semantics of get_buffer are unclear
        void *result_buf = m_malloc(buffer_length);
        memcpy(result_buf, buffer, buffer_length);
        // Always a byte view: len(buf) is the buffer length in BYTES, matching
        // the C protocol (audiosample_get_buffer's buffer_length is bytes).
        // This used to be a typed view ('h'/'H' for 16-bit samples), whose
        // len() was the SAMPLE count -- consumers doing byte math on len(buf)
        // (audiodev's AudioOut pump) then under-counted 16-bit audio by 2x and
        // overproduced PCM at twice realtime. One length unit, same as
        // CircuitPython's own convention, ends that class of bug; anything
        // wanting typed access can cast or struct.unpack the bytes itself.
        result[1] = mp_obj_new_memoryview('B', buffer_length, result_buf);
    }

    return mp_obj_new_tuple(2, result);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiocore_get_buffer_obj, 1, 3, audiocore_get_buffer);

static mp_obj_t audiocore_get_structure(size_t n_args, const mp_obj_t *args) {
    bool single_buffer, samples_signed;
    uint32_t max_buffer_length;
    uint8_t spacing;

    audiosample_get_buffer_structure_checked(args[0], audiocore_single(n_args, args), &single_buffer, &samples_signed, &max_buffer_length, &spacing);
    mp_obj_t result[4] = {
        mp_obj_new_int_from_uint(single_buffer),
        mp_obj_new_int_from_uint(samples_signed),
        mp_obj_new_int_from_uint(max_buffer_length),
        mp_obj_new_int_from_uint(spacing),
    };
    return mp_obj_new_tuple(4, result);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiocore_get_structure_obj, 1, 2, audiocore_get_structure);

static mp_obj_t audiocore_reset_buffer(size_t n_args, const mp_obj_t *args) {
    audiocore_check(args[0]);
    audiosample_reset_buffer(args[0], audiocore_single(n_args, args), audiocore_channel(n_args, args));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiocore_reset_buffer_obj, 1, 3, audiocore_reset_buffer);

static const mp_rom_map_elem_t audiocore_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiocore) },
    AUDIODSP_BUILD_GLOBALS,
    { MP_ROM_QSTR(MP_QSTR_RawSample), MP_ROM_PTR(&audioio_rawsample_type) },
    { MP_ROM_QSTR(MP_QSTR_WaveFile), MP_ROM_PTR(&audioio_wavefile_type) },
    { MP_ROM_QSTR(MP_QSTR_get_buffer), MP_ROM_PTR(&audiocore_get_buffer_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset_buffer), MP_ROM_PTR(&audiocore_reset_buffer_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_structure), MP_ROM_PTR(&audiocore_get_structure_obj) },
};
static MP_DEFINE_CONST_DICT(audiocore_module_globals, audiocore_module_globals_table);

const mp_obj_module_t audiocore_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audiocore_module_globals,
};
MP_REGISTER_MODULE(MP_QSTR_audiocore, audiocore_module);
