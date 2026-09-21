// Ported from CircuitPython's shared-module/audiocore/__init__.c (upstream
// repo: https://github.com/adafruit/circuitpython, MIT). Unused includes
// dropped (RawSample.h/WaveFile.h/audiomixer/Mixer.h/audioio/__init__.h --
// none of their symbols are referenced in this file upstream either;
// checked with a grep across the whole file before dropping them, to avoid
// a premature tier-3 build dependency here in tier 1).
//
// The deinit-guard helpers and the three common property accessors
// (sample_rate/bits_per_sample/channel_count) below are ported from
// CircuitPython's shared-bindings/audiocore/__init__.c, minus its
// CIRCUITPY_AUDIOCORE_DEBUG-gated get_buffer/reset_buffer/get_structure
// functions (docs-hidden debug helpers, not part of the real audiosample
// surface, and gated off in CP's own default config) and minus the module
// table + MP_REGISTER_MODULE (moved to module.c once RawSample/WaveFile
// exist, so this file doesn't have to forward-declare their types).
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Dan Halbert for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#include "audiocore/__init__.h"
#include "audiospeed/Resampler.h"
#include "cp_compat/argcheck.h"
#include "cp_compat/objproperty.h"
#include "cp_compat/util.h"
#include "shared/audiodsp_pump_lock.h"
#include "shared/audiodsp_sample.h"

#include "py/obj.h"
#include "py/runtime.h"

typedef struct {
    mp_obj_t object;
    const audiosample_p_t *protocol;
} micropython_sample_adapter_t;

static audiodsp_status_t micropython_sample_reset(void *context,
    bool single_channel_output, uint8_t audio_channel) {
    micropython_sample_adapter_t *adapter = context;
    adapter->protocol->reset_buffer(MP_OBJ_TO_PTR(adapter->object),
        single_channel_output, audio_channel);
    return AUDIODSP_STATUS_OK;
}

static audiodsp_status_t micropython_sample_get(void *context,
    bool single_channel_output, uint8_t audio_channel,
    const uint8_t **buffer, uint32_t *buffer_length,
    audiodsp_buffer_result_t *result) {
    micropython_sample_adapter_t *adapter = context;
    uint8_t *runtime_buffer = NULL;
    audioio_get_buffer_result_t runtime_result = adapter->protocol->get_buffer(
        MP_OBJ_TO_PTR(adapter->object), single_channel_output, audio_channel,
        &runtime_buffer, buffer_length);
    *buffer = runtime_buffer;
    *result = (audiodsp_buffer_result_t)runtime_result;
    return AUDIODSP_STATUS_OK;
}

static const audiodsp_sample_ops_t micropython_sample_ops = {
    .reset_buffer = micropython_sample_reset,
    .get_buffer = micropython_sample_get,
};

// Non-raising. This is the half of the funnel that used to call
// mp_proto_get_or_throw, and the reason it may not is that the raise dies one
// frame EARLIER than the longjmp -- inside gc_alloc, building the exception
// object, on a thread with no interpreter state to allocate from. The spike
// has the stack (docs/spikes/live-audio-path-notes.md, "a raise on the pump
// thread dies allocating the exception"). So it returns false and leaves a
// code in the fault register; the pull stops and the next control call
// reports it, and the Python-facing entry points below raise from the fault
// exactly as they always did.
static bool micropython_sample_source(mp_obj_t sample_obj,
    micropython_sample_adapter_t *adapter, audiodsp_sample_source_t *source) {
    const audiosample_p_t *protocol = mp_proto_get(
        MP_QSTR_protocol_audiosample, sample_obj);
    if (protocol == NULL) {
        // Pump thread only: the fault register is what the pump stops on, and
        // a control-path pull must not stop the audio. See the deinit guards
        // below.
        if (audiodsp_pump_on_pump_thread()) {
            audiodsp_pump_fault_set(AUDIODSP_PUMP_FAULT_NO_PROTOCOL);
        }
        return false;
    }
    adapter->object = sample_obj;
    adapter->protocol = protocol;
    source->ops = &micropython_sample_ops;
    source->context = adapter;
    source->info = NULL;
    return true;
}

// The deinitialised guard sits on these two functions and not only on each
// node's Python methods, because these two are the funnel: every pull and
// every rewind in the whole palette arrives here, whether it came from
// `audiocore.get_buffer()`, from an audio output's pump, or from one node
// fetching from the node behind it. Guarding the Python methods alone left
// the C protocol entry point open, which is why a released `audiomixer.Mixer`
// segfaulted on a board while raising cleanly on the CPython shim
// (audiodsp#59): `Mixer.deinit()` frees its voice buffers and
// `audiomixer_mixer_get_buffer` read them.
//
// `micropython_sample_source` has already thrown unless the object
// implements the audiosample protocol, and every type that does begins with
// an `audiosample_base_t` -- synthio's two through `synthio_synth_t` -- so
// the cast below is sound and needs no second protocol lookup.
//
// Neither of these raises any more. Both used to, on every pull of every node
// in the palette -- the protocol lookup and the deinit check were the two
// funnel sites the exception audit found, and "cannot fire on a stable graph"
// is a property, not a guarantee. A torn graph fired both. Now a failure here
// is a fault code and a GET_BUFFER_ERROR, which every node in the palette
// already handles by producing silence, so a bug in the handoff is a quiet
// block instead of a core dump.
//
// The promise to Python is kept where it was made: `audiocore.get_buffer()`
// and `audiocore.reset_buffer()` in module.c raise from the fault register.
void audiosample_reset_buffer(mp_obj_t sample_obj, bool single_channel_output, uint8_t audio_channel) {
    micropython_sample_adapter_t adapter;
    audiodsp_sample_source_t source;
    if (!micropython_sample_source(sample_obj, &adapter, &source)) {
        return;
    }
    if (audiosample_deinited(MP_OBJ_TO_PTR(sample_obj))) {
        // ONLY from the pump's own thread. The fault register is the pump's
        // "why did I stop", and the pump stops on it -- so a control-thread
        // pull of a released node would take the audio down with it, and the
        // control path does pull released nodes legitimately: a Rack's
        // deinit() stops each child in turn, and a stop() resets its source's
        // buffer. On the board that killed the audio on the first patch
        // change in rack_gui, err=5 fault=deinited, with nothing at all wrong
        // with the graph the pump was playing. The caller still gets its
        // GET_BUFFER_ERROR here, and audiocore.get_buffer() still raises,
        // because module.c does its own check.
        if (audiodsp_pump_on_pump_thread()) {
            audiodsp_pump_fault_set(AUDIODSP_PUMP_FAULT_DEINITED);
        }
        return;
    }
    audiodsp_pump_lock_acquire_nested();
    (void)audiodsp_sample_reset(&source, single_channel_output, audio_channel);
    audiodsp_pump_lock_release_nested();
}

audioio_get_buffer_result_t audiosample_get_buffer(mp_obj_t sample_obj,
    bool single_channel_output,
    uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    micropython_sample_adapter_t adapter;
    audiodsp_sample_source_t source;
    *buffer = NULL;
    *buffer_length = 0;
    if (!micropython_sample_source(sample_obj, &adapter, &source)) {
        return GET_BUFFER_ERROR;
    }
    if (audiosample_deinited(MP_OBJ_TO_PTR(sample_obj))) {
        // This is audiodsp#59's case: a released Mixer whose voice buffers are
        // freed, read by audiomixer_mixer_get_buffer. The guard still stops
        // the read; what changes is that it now stops it without allocating.
        // Published to the pump's fault register only from the pump's own
        // thread -- see reset_buffer above for what a control-thread deinit
        // did to the audio before that distinction existed.
        if (audiodsp_pump_on_pump_thread()) {
            audiodsp_pump_fault_set(AUDIODSP_PUMP_FAULT_DEINITED);
        }
        return GET_BUFFER_ERROR;
    }
    const uint8_t *shared_buffer = NULL;
    audiodsp_buffer_result_t result = AUDIODSP_BUFFER_ERROR;
    audiodsp_pump_lock_acquire_nested();
    audiodsp_status_t status = audiodsp_sample_get(&source, single_channel_output,
        channel, &shared_buffer, buffer_length, &result);
    audiodsp_pump_lock_release_nested();
    if (status != AUDIODSP_STATUS_OK) {
        *buffer = NULL;
        *buffer_length = 0;
        return GET_BUFFER_ERROR;
    }
    *buffer = (uint8_t *)shared_buffer;
    return (audioio_get_buffer_result_t)result;
}

void audiosample_convert_u8m_s16s(int16_t *buffer_out, const uint8_t *buffer_in, size_t nframes) {
    for (; nframes--;) {
        int16_t sample = (*buffer_in++ - 0x80) << 8;
        *buffer_out++ = sample;
        *buffer_out++ = sample;
    }
}

void audiosample_convert_u8s_s16s(int16_t *buffer_out, const uint8_t *buffer_in, size_t nframes) {
    size_t nsamples = 2 * nframes;
    for (; nsamples--;) {
        int16_t sample = (*buffer_in++ - 0x80) << 8;
        *buffer_out++ = sample;
    }
}

void audiosample_convert_s8m_s16s(int16_t *buffer_out, const int8_t *buffer_in, size_t nframes) {
    for (; nframes--;) {
        int16_t sample = (*buffer_in++) << 8;
        *buffer_out++ = sample;
        *buffer_out++ = sample;
    }
}

void audiosample_convert_s8s_s16s(int16_t *buffer_out, const int8_t *buffer_in, size_t nframes) {
    size_t nsamples = 2 * nframes;
    for (; nsamples--;) {
        int16_t sample = (*buffer_in++) << 8;
        *buffer_out++ = sample;
    }
}

void audiosample_convert_u16m_s16s(int16_t *buffer_out, const uint16_t *buffer_in, size_t nframes) {
    for (; nframes--;) {
        int16_t sample = *buffer_in++ - 0x8000;
        *buffer_out++ = sample;
        *buffer_out++ = sample;
    }
}

void audiosample_convert_u16s_s16s(int16_t *buffer_out, const uint16_t *buffer_in, size_t nframes) {
    size_t nsamples = 2 * nframes;
    for (; nsamples--;) {
        int16_t sample = *buffer_in++ - 0x8000;
        *buffer_out++ = sample;
    }
}

void audiosample_convert_s16m_s16s(int16_t *buffer_out, const int16_t *buffer_in, size_t nframes) {
    for (; nframes--;) {
        int16_t sample = *buffer_in++;
        *buffer_out++ = sample;
        *buffer_out++ = sample;
    }
}


void audiosample_convert_u8s_u8m(uint8_t *buffer_out, const uint8_t *buffer_in, size_t nframes) {
    for (; nframes--;) {
        uint8_t sample = *buffer_in++ + 0x80;
        *buffer_out++ = sample;
        buffer_in++;
    }
}

void audiosample_convert_s8m_u8m(uint8_t *buffer_out, const int8_t *buffer_in, size_t nframes) {
    for (; nframes--;) {
        uint8_t sample = *buffer_in++ + 0x80;
        *buffer_out++ = sample;
    }
}

void audiosample_convert_s8s_u8m(uint8_t *buffer_out, const int8_t *buffer_in, size_t nframes) {
    for (; nframes--;) {
        uint8_t sample = *buffer_in++ + 0x80;
        *buffer_out++ = sample;
        buffer_in++;
    }
}

void audiosample_convert_u16m_u8m(uint8_t *buffer_out, const uint16_t *buffer_in, size_t nframes) {
    for (; nframes--;) {
        uint8_t sample = (*buffer_in++) >> 8;
        *buffer_out++ = sample;
    }
}

void audiosample_convert_u16s_u8m(uint8_t *buffer_out, const uint16_t *buffer_in, size_t nframes) {
    for (; nframes--;) {
        uint8_t sample = (*buffer_in++) >> 8;
        *buffer_out++ = sample;
        buffer_in++;
    }
}

void audiosample_convert_s16m_u8m(uint8_t *buffer_out, const int16_t *buffer_in, size_t nframes) {
    for (; nframes--;) {
        uint8_t sample = (*buffer_in++ + 0x8000) >> 8;
        *buffer_out++ = sample;
    }
}

void audiosample_convert_s16s_u8m(uint8_t *buffer_out, const int16_t *buffer_in, size_t nframes) {
    for (; nframes--;) {
        uint8_t sample = (*buffer_in++ + 0x8000) >> 8;
        *buffer_out++ = sample;
        buffer_in++;
    }
}


void audiosample_convert_u8m_u8s(uint8_t *buffer_out, const uint8_t *buffer_in, size_t nframes) {
    for (; nframes--;) {
        uint8_t sample = *buffer_in++;
        *buffer_out++ = sample;
        *buffer_out++ = sample;
    }
}

void audiosample_convert_s8m_u8s(uint8_t *buffer_out, const int8_t *buffer_in, size_t nframes) {
    for (; nframes--;) {
        uint8_t sample = *buffer_in++ + 0x80;
        *buffer_out++ = sample;
        *buffer_out++ = sample;
    }
}

void audiosample_convert_s8s_u8s(uint8_t *buffer_out, const int8_t *buffer_in, size_t nframes) {
    size_t nsamples = 2 * nframes;
    for (; nsamples--;) {
        uint8_t sample = *buffer_in++ + 0x80;
        *buffer_out++ = sample;
    }
}

void audiosample_convert_u16m_u8s(uint8_t *buffer_out, const uint16_t *buffer_in, size_t nframes) {
    for (; nframes--;) {
        uint8_t sample = (*buffer_in++) >> 8;
        *buffer_out++ = sample;
        *buffer_out++ = sample;
    }
}

void audiosample_convert_u16s_u8s(uint8_t *buffer_out, const uint16_t *buffer_in, size_t nframes) {
    size_t nsamples = 2 * nframes;
    for (; nsamples--;) {
        uint8_t sample = (*buffer_in++) >> 8;
        *buffer_out++ = sample;
    }
}

void audiosample_convert_s16m_u8s(uint8_t *buffer_out, const int16_t *buffer_in, size_t nframes) {
    for (; nframes--;) {
        uint8_t sample = (*buffer_in++ + 0x8000) >> 8;
        *buffer_out++ = sample;
        *buffer_out++ = sample;
    }
}

void audiosample_convert_s16s_u8s(uint8_t *buffer_out, const int16_t *buffer_in, size_t nframes) {
    size_t nsamples = 2 * nframes;
    for (; nsamples--;) {
        uint8_t sample = (*buffer_in++ + 0x8000) >> 8;
        *buffer_out++ = sample;
    }
}

void audiosample_must_match(audiosample_base_t *self, mp_obj_t other_in, bool allow_mono_to_stereo) {
    const audiosample_base_t *other = audiosample_check(other_in);
    audiosample_check_for_deinit(other);
    // A Resampler is exempt from the rate check, because a rate it does not
    // match is the entire reason to use one: it is handed the destination's
    // rate at the bottom of this function and resamples to it. Upstream does
    // exactly this, gated on CIRCUITPY_AUDIOSPEED; audiodsp always builds
    // audiospeed, so there is nothing to gate on.
    if (other->sample_rate != self->sample_rate &&
        !mp_obj_is_type(other_in, &audiospeed_resampler_type)) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("The sample's %q does not match"), MP_QSTR_sample_rate);
    }
    if ((!allow_mono_to_stereo || (allow_mono_to_stereo && self->channel_count != 2)) && other->channel_count != self->channel_count) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("The sample's %q does not match"), MP_QSTR_channel_count);
    }
    if (other->bits_per_sample != self->bits_per_sample) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("The sample's %q does not match"), MP_QSTR_bits_per_sample);
    }
    if (other->samples_signed != self->samples_signed) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("The sample's %q does not match"), MP_QSTR_signedness);
    }

    // Bind the destination rate onto a Resampler. This is the one place it
    // happens, and it covers the whole palette because every node's `play()`
    // comes through here -- which is why upstream put it here too, rather
    // than in each node.
    if (mp_obj_is_type(other_in, &audiospeed_resampler_type)) {
        audiospeed_resampler_set_sample_rate(MP_OBJ_TO_PTR(other_in),
            self->sample_rate);
    }
}

bool audiosample_deinited(const audiosample_base_t *self) {
    return self->channel_count == 0;
}

void audiosample_check_for_deinit(const audiosample_base_t *self) {
    if (audiosample_deinited(self)) {
        raise_deinited_error();
    }
}

// One aligned word, so it cannot tear and it takes no lock. The damage in a
// deinit() is never this line -- it is the pointer nulling that follows it,
// after the funnel's guard has already let a pull in. Each deinit() body
// holds the lock over the lot.
void audiosample_mark_deinit(audiosample_base_t *self) {
    self->channel_count = 0;
}

// common implementation of channel_count property for audio samples
static mp_obj_t audiosample_obj_get_channel_count(mp_obj_t self_in) {
    audiosample_base_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(self);
    return MP_OBJ_NEW_SMALL_INT(audiosample_get_channel_count(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiosample_get_channel_count_obj, audiosample_obj_get_channel_count);

MP_PROPERTY_GETTER(audiosample_channel_count_obj,
    (mp_obj_t)&audiosample_get_channel_count_obj);


// common implementation of bits_per_sample property for audio samples
static mp_obj_t audiosample_obj_get_bits_per_sample(mp_obj_t self_in) {
    audiosample_base_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(self);
    return MP_OBJ_NEW_SMALL_INT(audiosample_get_bits_per_sample(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiosample_get_bits_per_sample_obj, audiosample_obj_get_bits_per_sample);

MP_PROPERTY_GETTER(audiosample_bits_per_sample_obj,
    (mp_obj_t)&audiosample_get_bits_per_sample_obj);

// common implementation of sample_rate property for audio samples
static mp_obj_t audiosample_obj_get_sample_rate(mp_obj_t self_in) {
    audiosample_base_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(self);
    return MP_OBJ_NEW_SMALL_INT(audiosample_get_sample_rate(audiosample_check(self_in)));
}
MP_DEFINE_CONST_FUN_OBJ_1(audiosample_get_sample_rate_obj, audiosample_obj_get_sample_rate);

static mp_obj_t audiosample_obj_set_sample_rate(mp_obj_t self_in, mp_obj_t sample_rate) {
    audiosample_base_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(self);
    audiosample_set_sample_rate(audiosample_check(self_in), mp_obj_get_int(sample_rate));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(audiosample_set_sample_rate_obj, audiosample_obj_set_sample_rate);

MP_PROPERTY_GETSET(audiosample_sample_rate_obj,
    (mp_obj_t)&audiosample_get_sample_rate_obj,
    (mp_obj_t)&audiosample_set_sample_rate_obj);
