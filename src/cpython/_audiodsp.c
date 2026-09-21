// SPDX-License-Identifier: MIT
// Small CPython runtime core used by the public compatibility modules.
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

#include "shared/audiodsp_rawsample.h"
#include "shared/audiodsp_synth_dsp.h"
#include "shared/audiodsp_envelope.h"
#include "shared/audiodsp_distortion.h"
#include "shared/audiodsp_biquad.h"
#include "shared/audiodsp_echo.h"
#include "shared/audiodsp_phaser.h"
#include "shared/audiodsp_chorus.h"
#include "shared/audiodsp_multitap.h"
#include "shared/audiodsp_pitchshift.h"
#include "shared/audiodsp_freeverb.h"
#include "shared/audiodsp_dynamics.h"
#include "shared/audiodsp_splitter.h"
#include "shared/audiodsp_midside.h"
#include "shared/audiodsp_remix.h"
#include "shared/audiodsp_multiply.h"
#include "shared/audiodsp_suboctave.h"
#include "shared/audiodsp_convolve.h"
#include "shared/audiodsp_feedback_delay.h"
#include "shared/audiodsp_shaper.h"
#include "shared/audiodsp_samplehold.h"
#include "shared/audiodsp_ladder.h"
#include "shared/audiodsp_tank.h"
#include "shared/audiodsp_modal.h"
#include "shared/audiodsp_filter_f32.h"

// setup.py defines this from the VERSION file; the fallback is only for
// someone compiling this source by hand.
#ifndef AUDIODSP_VERSION
#define AUDIODSP_VERSION "0.0.0+unknown"
#endif
#ifndef AUDIODSP_REVISION
#define AUDIODSP_REVISION "unknown"
#endif

typedef struct {
    PyObject *error;
    PyObject *buffer_owner_type;
    PyObject *rawsample_type;
    PyObject *envelope_state_type;
    PyObject *biquad_state_type;
    PyObject *dynamics_state_type;
    PyObject *splitter_ring_type;
    PyObject *feedback_delay_state_type;
    PyObject *waveshaper_state_type;
    PyObject *samplehold_state_type;
    PyObject *ladder_state_type;
    PyObject *biquad_f32_state_type;
    PyObject *allpass_f32_state_type;
    PyObject *suboctave_state_type;
    PyObject *convolver_state_type;
    PyObject *tank_state_type;
    PyObject *modal_state_type;
} audiodsp_state_t;

typedef struct {
    PyObject_HEAD
    Py_buffer view;
    int acquired;
} audiodsp_buffer_owner_t;

static int buffer_owner_init(audiodsp_buffer_owner_t *self, PyObject *args, PyObject *kwargs) {
    PyObject *exporter;
    static char *keywords[] = {"exporter", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O:BufferOwner", keywords, &exporter)) return -1;
    if (self->acquired) {
        PyBuffer_Release(&self->view);
        self->acquired = 0;
    }
    if (PyObject_GetBuffer(exporter, &self->view, PyBUF_FORMAT | PyBUF_C_CONTIGUOUS) < 0) return -1;
    self->acquired = 1;
    return 0;
}

static int buffer_owner_traverse(audiodsp_buffer_owner_t *self, visitproc visit, void *arg) {
    if (self->acquired) Py_VISIT(self->view.obj);
    return 0;
}

static int buffer_owner_clear(audiodsp_buffer_owner_t *self) {
    if (self->acquired) {
        PyBuffer_Release(&self->view);
        self->acquired = 0;
    }
    return 0;
}

static void buffer_owner_dealloc(audiodsp_buffer_owner_t *self) {
    PyObject_GC_UnTrack(self);
    buffer_owner_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *buffer_owner_release(audiodsp_buffer_owner_t *self, PyObject *unused) {
    buffer_owner_clear(self);
    Py_RETURN_NONE;
}

static PyObject *buffer_owner_bytes(audiodsp_buffer_owner_t *self, PyObject *unused) {
    if (!self->acquired) {
        PyErr_SetString(PyExc_RuntimeError, "buffer has been released");
        return NULL;
    }
    return PyBytes_FromStringAndSize((const char *)self->view.buf, self->view.len);
}

static PyObject *buffer_owner_format(audiodsp_buffer_owner_t *self, void *closure) {
    if (!self->acquired) Py_RETURN_NONE;
    return PyUnicode_FromString(self->view.format == NULL ? "B" : self->view.format);
}

static PyObject *buffer_owner_nbytes(audiodsp_buffer_owner_t *self, void *closure) {
    return PyLong_FromSsize_t(self->acquired ? self->view.len : 0);
}

static PyObject *buffer_owner_itemsize(audiodsp_buffer_owner_t *self, void *closure) {
    return PyLong_FromSsize_t(self->acquired ? self->view.itemsize : 0);
}

static PyMethodDef buffer_owner_methods[] = {
    {"release", (PyCFunction)buffer_owner_release, METH_NOARGS, NULL},
    {"bytes", (PyCFunction)buffer_owner_bytes, METH_NOARGS, NULL},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef buffer_owner_getset[] = {
    {"format", (getter)buffer_owner_format, NULL, NULL, NULL},
    {"nbytes", (getter)buffer_owner_nbytes, NULL, NULL, NULL},
    {"itemsize", (getter)buffer_owner_itemsize, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyType_Slot buffer_owner_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, buffer_owner_init},
    {Py_tp_dealloc, buffer_owner_dealloc},
    {Py_tp_traverse, buffer_owner_traverse},
    {Py_tp_clear, buffer_owner_clear},
    {Py_tp_methods, buffer_owner_methods},
    {Py_tp_getset, buffer_owner_getset},
    {0, NULL},
};

static PyType_Spec buffer_owner_spec = {
    .name = "_audiodsp.BufferOwner",
    .basicsize = sizeof(audiodsp_buffer_owner_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_HAVE_GC,
    .slots = buffer_owner_slots,
};

typedef struct {
    PyObject_HEAD
    Py_buffer exporter;
    int acquired;
    audiodsp_sample_info_t info;
    audiodsp_rawsample_state_t state;
} audiodsp_rawsample_object_t;

static int rawsample_raise_status(audiodsp_status_t status) {
    if (status == AUDIODSP_STATUS_DEINITIALIZED) {
        // ValueError, not RuntimeError: this is CircuitPython's own exception
        // for a released object, straight out of shared-bindings/util.c, and
        // cp_compat/util.c is a verbatim port of it. The exception type is the
        // one thing about a released node that portable user code can catch, so
        // a `try/except ValueError` around a teardown path has to work the same
        // on a board and on this target. audiodsp#73; the message matches the
        // native builds' too, trailing sentence included.
        PyErr_SetString(PyExc_ValueError,
            "Object has been deinitialized and can no longer be used. "
            "Create a new object.");
    } else {
        PyErr_SetString(PyExc_RuntimeError, "audio sample operation failed");
    }
    return -1;
}

static int rawsample_init(audiodsp_rawsample_object_t *self,
    PyObject *args, PyObject *kwargs) {
    PyObject *exporter;
    int channel_count = 1;
    unsigned int sample_rate = 8000;
    int single_buffer = 1;
    static char *keywords[] = {
        "buffer", "channel_count", "sample_rate", "single_buffer", NULL
    };
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|$iIp:RawSample",
        keywords, &exporter, &channel_count, &sample_rate, &single_buffer)) {
        return -1;
    }
    if (channel_count != 1 && channel_count != 2) {
        PyErr_SetString(PyExc_ValueError, "channel_count must be 1 or 2");
        return -1;
    }
    if (sample_rate < 1) {
        PyErr_SetString(PyExc_ValueError, "sample_rate must be at least 1");
        return -1;
    }

    Py_buffer view = {0};
    if (PyObject_GetBuffer(exporter, &view,
        PyBUF_FORMAT | PyBUF_C_CONTIGUOUS) < 0) {
        return -1;
    }
    const char *format = view.format == NULL ? "B" : view.format;
    bool samples_signed = format[0] == 'b' || format[0] == 'h';
    if (!((format[0] == 'b' || format[0] == 'B') && format[1] == '\0') &&
        !((format[0] == 'h' || format[0] == 'H') && format[1] == '\0')) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError,
            "buffer must be a bytearray or array of type 'h', 'H', 'b', or 'B'");
        return -1;
    }
    uint8_t bytes_per_sample = (format[0] == 'h' || format[0] == 'H') ? 2 : 1;
    if (!single_buffer &&
        view.len % (bytes_per_sample * channel_count * 2) != 0) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError,
            "Length of buffer must be an even multiple of channel_count * type_size");
        return -1;
    }
    if (view.len > UINT32_MAX) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_OverflowError, "buffer is too large");
        return -1;
    }

    if (self->acquired) {
        audiodsp_rawsample_deinit(&self->state);
        PyBuffer_Release(&self->exporter);
    }
    self->exporter = view;
    self->acquired = 1;
    audiodsp_rawsample_construct(&self->state, &self->info,
        (uint8_t *)view.buf, (uint32_t)view.len, bytes_per_sample,
        samples_signed, (uint8_t)channel_count, sample_rate, single_buffer);
    return 0;
}

static int rawsample_traverse(audiodsp_rawsample_object_t *self,
    visitproc visit, void *arg) {
    if (self->acquired) Py_VISIT(self->exporter.obj);
    return 0;
}

static int rawsample_clear(audiodsp_rawsample_object_t *self) {
    audiodsp_rawsample_deinit(&self->state);
    if (self->acquired) {
        PyBuffer_Release(&self->exporter);
        self->acquired = 0;
    }
    return 0;
}

static void rawsample_dealloc(audiodsp_rawsample_object_t *self) {
    PyObject_GC_UnTrack(self);
    rawsample_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *rawsample_deinit(audiodsp_rawsample_object_t *self,
    PyObject *unused) {
    rawsample_clear(self);
    Py_RETURN_NONE;
}

static PyObject *rawsample_reset_buffer(audiodsp_rawsample_object_t *self,
    PyObject *args, PyObject *kwargs) {
    int single_channel_output = 0;
    unsigned int audio_channel = 0;
    static char *keywords[] = {
        "single_channel_output", "audio_channel", NULL
    };
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|pI:_reset_buffer",
        keywords, &single_channel_output, &audio_channel)) return NULL;
    audiodsp_sample_source_t source = audiodsp_rawsample_source(&self->state);
    audiodsp_status_t status = audiodsp_sample_reset(&source,
        single_channel_output, (uint8_t)audio_channel);
    if (status != AUDIODSP_STATUS_OK) {
        rawsample_raise_status(status);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *rawsample_get_buffer(audiodsp_rawsample_object_t *self,
    PyObject *args, PyObject *kwargs) {
    int single_channel_output = 0;
    unsigned int audio_channel = 0;
    static char *keywords[] = {
        "single_channel_output", "audio_channel", NULL
    };
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|pI:_get_buffer",
        keywords, &single_channel_output, &audio_channel)) return NULL;

    audiodsp_sample_source_t source = audiodsp_rawsample_source(&self->state);
    const uint8_t *buffer = NULL;
    uint32_t buffer_length = 0;
    audiodsp_buffer_result_t result = AUDIODSP_BUFFER_ERROR;
    audiodsp_status_t status = audiodsp_sample_get(&source,
        single_channel_output, (uint8_t)audio_channel,
        &buffer, &buffer_length, &result);
    if (status != AUDIODSP_STATUS_OK) {
        rawsample_raise_status(status);
        return NULL;
    }

    PyObject *data;
    if (single_channel_output && self->info.channel_count > 1) {
        const uint32_t width = self->info.bits_per_sample / 8;
        const uint32_t frame_width = width * self->info.channel_count;
        const uint32_t frames = buffer_length / frame_width;
        data = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)frames * width);
        if (data != NULL) {
            uint8_t *destination = (uint8_t *)PyBytes_AS_STRING(data);
            for (uint32_t frame = 0; frame < frames; frame++) {
                memcpy(destination + frame * width,
                    buffer + frame * frame_width, width);
            }
        }
    } else {
        data = PyBytes_FromStringAndSize((const char *)buffer, buffer_length);
    }
    if (data == NULL) return NULL;
    PyObject *tuple = Py_BuildValue("(iN)", (int)result, data);
    return tuple;
}

static PyObject *rawsample_enter(audiodsp_rawsample_object_t *self,
    PyObject *unused) {
    if (self->state.deinited) {
        rawsample_raise_status(AUDIODSP_STATUS_DEINITIALIZED);
        return NULL;
    }
    return Py_NewRef((PyObject *)self);
}

static PyObject *rawsample_exit(audiodsp_rawsample_object_t *self,
    PyObject *args) {
    rawsample_clear(self);
    Py_RETURN_NONE;
}

static PyObject *rawsample_get_sample_rate(audiodsp_rawsample_object_t *self,
    void *closure) {
    if (self->state.deinited) {
        rawsample_raise_status(AUDIODSP_STATUS_DEINITIALIZED);
        return NULL;
    }
    return PyLong_FromUnsignedLong(self->info.sample_rate);
}

static int rawsample_set_sample_rate(audiodsp_rawsample_object_t *self,
    PyObject *value, void *closure) {
    if (self->state.deinited) return rawsample_raise_status(AUDIODSP_STATUS_DEINITIALIZED);
    unsigned long rate = PyLong_AsUnsignedLong(value);
    if (PyErr_Occurred()) return -1;
    if (rate < 1 || rate > UINT32_MAX) {
        PyErr_SetString(PyExc_ValueError, "sample_rate must be at least 1");
        return -1;
    }
    self->info.sample_rate = (uint32_t)rate;
    return 0;
}

#define RAWSAMPLE_UINT8_GETTER(name, field) \
    static PyObject *rawsample_get_##name(audiodsp_rawsample_object_t *self, void *closure) { \
        if (self->state.deinited) { \
            rawsample_raise_status(AUDIODSP_STATUS_DEINITIALIZED); \
            return NULL; \
        } \
        return PyLong_FromUnsignedLong(self->info.field); \
    }

RAWSAMPLE_UINT8_GETTER(bits_per_sample, bits_per_sample)
RAWSAMPLE_UINT8_GETTER(channel_count, channel_count)

static PyObject *rawsample_get_samples_signed(audiodsp_rawsample_object_t *self,
    void *closure) {
    return PyBool_FromLong(self->info.samples_signed);
}

static PyObject *rawsample_get_single_buffer(audiodsp_rawsample_object_t *self,
    void *closure) {
    return PyBool_FromLong(self->info.single_buffer);
}

static PyMethodDef rawsample_methods[] = {
    {"deinit", (PyCFunction)rawsample_deinit, METH_NOARGS, NULL},
    {"_reset_buffer", (PyCFunction)rawsample_reset_buffer, METH_VARARGS | METH_KEYWORDS, NULL},
    {"_get_buffer", (PyCFunction)rawsample_get_buffer, METH_VARARGS | METH_KEYWORDS, NULL},
    {"__enter__", (PyCFunction)rawsample_enter, METH_NOARGS, NULL},
    {"__exit__", (PyCFunction)rawsample_exit, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef rawsample_getset[] = {
    {"sample_rate", (getter)rawsample_get_sample_rate, (setter)rawsample_set_sample_rate, NULL, NULL},
    {"bits_per_sample", (getter)rawsample_get_bits_per_sample, NULL, NULL, NULL},
    {"channel_count", (getter)rawsample_get_channel_count, NULL, NULL, NULL},
    {"samples_signed", (getter)rawsample_get_samples_signed, NULL, NULL, NULL},
    {"single_buffer", (getter)rawsample_get_single_buffer, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyType_Slot rawsample_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, rawsample_init},
    {Py_tp_dealloc, rawsample_dealloc},
    {Py_tp_traverse, rawsample_traverse},
    {Py_tp_clear, rawsample_clear},
    {Py_tp_methods, rawsample_methods},
    {Py_tp_getset, rawsample_getset},
    {0, NULL},
};

static PyType_Spec rawsample_spec = {
    .name = "audiocore.RawSample",
    .basicsize = sizeof(audiodsp_rawsample_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_HAVE_GC,
    .slots = rawsample_slots,
};

typedef struct {
    PyObject_HEAD
    audiodsp_envelope_definition_t definition;
    audiodsp_envelope_state_t state;
} audiodsp_envelope_state_object_t;

static int envelope_state_init(audiodsp_envelope_state_object_t *self,
    PyObject *args, PyObject *kwargs) {
    unsigned int sample_rate;
    int enabled;
    double attack_time = 0, decay_time = 0, release_time = 0;
    double attack_level = 1, sustain_level = 1;
    static char *keywords[] = {
        "sample_rate", "enabled", "attack_time", "decay_time",
        "release_time", "attack_level", "sustain_level", NULL
    };
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Ip|ddddd:EnvelopeState",
        keywords, &sample_rate, &enabled, &attack_time, &decay_time,
        &release_time, &attack_level, &sustain_level)) return -1;
    audiodsp_envelope_definition_init(&self->definition, sample_rate, enabled,
        attack_time, decay_time, release_time, attack_level, sustain_level);
    audiodsp_envelope_state_init(&self->state, &self->definition);
    return 0;
}

static PyObject *envelope_state_step(audiodsp_envelope_state_object_t *self,
    PyObject *argument) {
    size_t count = PyLong_AsSize_t(argument);
    if (PyErr_Occurred()) return NULL;
    audiodsp_envelope_state_step(&self->state, &self->definition, count);
    Py_RETURN_NONE;
}

static PyObject *envelope_state_release(audiodsp_envelope_state_object_t *self,
    PyObject *unused) {
    audiodsp_envelope_state_release(&self->state);
    Py_RETURN_NONE;
}

static PyObject *envelope_state_reattack(audiodsp_envelope_state_object_t *self,
    PyObject *unused) {
    audiodsp_envelope_state_reattack(&self->state);
    Py_RETURN_NONE;
}

// Replace the envelope PARAMETERS while leaving the running state (level,
// substep, phase) untouched. The MicroPython and CircuitPython builds re-read
// a note's envelope on every render block (synthio_synth_get_note_envelope,
// called from the render loop), so reassigning `note.envelope` takes effect
// immediately there. This target caches the definition inside the state object
// at press time, so it needs this to stay faithful. See docs/upstream-diff.md.
static PyObject *envelope_state_set_definition(
    audiodsp_envelope_state_object_t *self, PyObject *args, PyObject *kwargs) {
    unsigned int sample_rate;
    int enabled;
    double attack_time = 0, decay_time = 0, release_time = 0;
    double attack_level = 1, sustain_level = 1;
    static char *keywords[] = {
        "sample_rate", "enabled", "attack_time", "decay_time",
        "release_time", "attack_level", "sustain_level", NULL
    };
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Ip|ddddd:set_definition",
        keywords, &sample_rate, &enabled, &attack_time, &decay_time,
        &release_time, &attack_level, &sustain_level)) return NULL;
    audiodsp_envelope_definition_init(&self->definition, sample_rate, enabled,
        attack_time, decay_time, release_time, attack_level, sustain_level);
    Py_RETURN_NONE;
}

static PyObject *envelope_state_level(audiodsp_envelope_state_object_t *self,
    void *closure) {
    return PyLong_FromLong(self->state.level);
}

static PyObject *envelope_state_kind(audiodsp_envelope_state_object_t *self,
    void *closure) {
    return PyLong_FromLong(self->state.state);
}

static PyMethodDef envelope_state_methods[] = {
    {"step", (PyCFunction)envelope_state_step, METH_O, NULL},
    {"release", (PyCFunction)envelope_state_release, METH_NOARGS, NULL},
    {"reattack", (PyCFunction)envelope_state_reattack, METH_NOARGS, NULL},
    {"set_definition", (PyCFunction)envelope_state_set_definition,
     METH_VARARGS | METH_KEYWORDS, NULL},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef envelope_state_getset[] = {
    {"level", (getter)envelope_state_level, NULL, NULL, NULL},
    {"state", (getter)envelope_state_kind, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyType_Slot envelope_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, envelope_state_init},
    {Py_tp_methods, envelope_state_methods},
    {Py_tp_getset, envelope_state_getset},
    {0, NULL},
};

static PyType_Spec envelope_state_spec = {
    .name = "_audiodsp.EnvelopeState",
    .basicsize = sizeof(audiodsp_envelope_state_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = envelope_state_slots,
};

// One filter state per channel. A stereo stream filtered through a single
// state makes each channel's filter memory the other channel's history; see
// docs/upstream-diff.md. CircuitPython caps channel_count at 2.
#define AUDIODSP_BIQUAD_MAX_CHANNELS 2

typedef struct {
    PyObject_HEAD
    audiodsp_biquad_state_t state[AUDIODSP_BIQUAD_MAX_CHANNELS];
} audiodsp_biquad_state_object_t;

static int biquad_state_init(audiodsp_biquad_state_object_t *self,
    PyObject *args, PyObject *kwargs) {
    if (!PyArg_ParseTuple(args, ":BiquadState")) return -1;
    memset(self->state, 0, sizeof(self->state));
    return 0;
}

static PyObject *biquad_state_reset(audiodsp_biquad_state_object_t *self,
    PyObject *unused) {
    for (int c = 0; c < AUDIODSP_BIQUAD_MAX_CHANNELS; c++) {
        audiodsp_biquad_reset(&self->state[c]);
    }
    Py_RETURN_NONE;
}

static PyObject *biquad_state_process(audiodsp_biquad_state_object_t *self,
    PyObject *args) {
    Py_buffer input = {0};
    int mode;
    double frequency, Q, A;
    unsigned int sample_rate;
    if (!PyArg_ParseTuple(args, "y*idddI:process_i32", &input, &mode,
        &frequency, &Q, &A, &sample_rate)) return NULL;
    if (input.len % sizeof(int32_t) || mode < 0 || mode > 6 ||
        sample_rate < 1) {
        PyBuffer_Release(&input);
        PyErr_SetString(PyExc_ValueError, "invalid biquad parameters");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize((const char *)input.buf,
        input.len);
    PyBuffer_Release(&input);
    if (result == NULL) return NULL;
    // CircuitPython's Q15 kernel, not the widened one beside it: this backs
    // synthio.Biquad and audiofilters.Filter, which CircuitPython also has, so
    // they render CircuitPython's bytes. audiodsp#77, Brad 2026-09-09.
    audiodsp_biquad_cp_coefficients_t coefficients;
    audiodsp_biquad_cp_configure(&coefficients, mode,
        audiodsp_biquad_cp_w0(frequency, sample_rate), Q, A);
    // Mono path (synthio's per-note filters): one channel, state[0].
    audiodsp_biquad_cp_process(&coefficients, &self->state[0],
        (int32_t *)PyBytes_AS_STRING(result),
        PyBytes_GET_SIZE(result) / sizeof(int32_t));
    return result;
}

static PyObject *biquad_state_process_s16(audiodsp_biquad_state_object_t *self,
    PyObject *args) {
    Py_buffer input = {0};
    int mode, channels;
    double frequency, Q, A, mix;
    unsigned int sample_rate;
    if (!PyArg_ParseTuple(args, "y*idddIdi:process_s16", &input, &mode,
        &frequency, &Q, &A, &sample_rate, &mix, &channels)) return NULL;
    if (input.len % sizeof(int16_t) || mode < 0 || mode > 6 ||
        sample_rate < 1 || channels < 1 ||
        channels > AUDIODSP_BIQUAD_MAX_CHANNELS ||
        (input.len / sizeof(int16_t)) % (size_t)channels) {
        PyBuffer_Release(&input);
        PyErr_SetString(PyExc_ValueError, "invalid biquad parameters");
        return NULL;
    }
    Py_ssize_t count = input.len / sizeof(int16_t);
    Py_ssize_t frames = count / channels;
    int32_t *working = PyMem_Malloc((size_t)count * sizeof(int32_t));
    if (working == NULL) {
        PyBuffer_Release(&input);
        return PyErr_NoMemory();
    }
    const int16_t *source = input.buf;
    audiodsp_biquad_cp_coefficients_t coefficients;
    audiodsp_biquad_cp_configure(&coefficients, mode,
        audiodsp_biquad_cp_w0(frequency, sample_rate), Q, A);
    // Deinterleave each channel into its own contiguous span of `working`,
    // filter it with that channel's state, and read it back interleaved.
    for (int c = 0; c < channels; c++) {
        int32_t *segment = working + (Py_ssize_t)c * frames;
        for (Py_ssize_t k = 0; k < frames; k++) {
            segment[k] = source[k * channels + c];
        }
        audiodsp_biquad_cp_process(&coefficients, &self->state[c], segment,
            frames);
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, input.len);
    if (result != NULL) {
        int16_t *destination = (int16_t *)PyBytes_AS_STRING(result);
        int32_t scale = 0x0fffffff / (32768 * 2 - 28000);
        for (Py_ssize_t k = 0; k < frames; k++) {
            for (int c = 0; c < channels; c++) {
                Py_ssize_t index = k * channels + c;
                int32_t filtered = working[(Py_ssize_t)c * frames + k];
                int32_t combined = (int32_t)(source[index] * (1.0 - mix) +
                    filtered * mix);
                destination[index] = audiodsp_mix_down_sample(combined, scale,
                    -28000, 28000);
            }
        }
    }
    PyMem_Free(working);
    PyBuffer_Release(&input);
    return result;
}

static PyMethodDef biquad_state_methods[] = {
    {"reset", (PyCFunction)biquad_state_reset, METH_NOARGS, NULL},
    {"process_i32", (PyCFunction)biquad_state_process, METH_VARARGS, NULL},
    {"process_s16", (PyCFunction)biquad_state_process_s16, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL},
};

static PyType_Slot biquad_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, biquad_state_init},
    {Py_tp_methods, biquad_state_methods},
    {0, NULL},
};

static PyType_Spec biquad_state_spec = {
    .name = "_audiodsp.BiquadState",
    .basicsize = sizeof(audiodsp_biquad_state_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = biquad_state_slots,
};

// --- audiodynamics / audioroute ---------------------------------------
//
// Unlike the CircuitPython-derived effects above, these two nodes come from
// micropython-vst3's `vstaudio` usermod. The wrappers that drive them live in
// audiodynamics.py and audioroute.py; what is exposed here is the state each
// keeps between blocks, so the arithmetic stays in the same C the MicroPython
// build compiles.

typedef struct {
    PyObject_HEAD
    audiodsp_dynamics_config_t config;
    audiodsp_dynamics_state_t state;
    int16_t *lookahead;
} audiodsp_dynamics_object_t;

// Allocated only once someone asks for lookahead, and only ever grown:
// `set(lookahead_ms=...)` mid-stream is a live gesture, and shrinking would
// mean freeing memory the DSP is reading out of.
static int dynamics_ensure_lookahead(audiodsp_dynamics_object_t *self) {
    const uint32_t wanted = audiodsp_dynamics_lookahead_frames(&self->config);
    if (wanted == 0 || wanted <= self->state.lookahead_capacity) {
        return 0;
    }
    int16_t *buffer = PyMem_Calloc((size_t)wanted *
        self->config.channel_count, sizeof(int16_t));
    if (buffer == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    PyMem_Free(self->lookahead);
    self->lookahead = buffer;
    audiodsp_dynamics_set_lookahead(&self->state, buffer, wanted);
    return 0;
}

static void dynamics_state_dealloc(audiodsp_dynamics_object_t *self) {
    PyTypeObject *type = Py_TYPE(self);
    PyMem_Free(self->lookahead);
    self->lookahead = NULL;
    type->tp_free((PyObject *)self);
    Py_DECREF(type);
}

static int dynamics_state_init(audiodsp_dynamics_object_t *self,
    PyObject *args, PyObject *kwargs) {
    int mode = AUDIODSP_DYNAMICS_COMPRESS;
    unsigned int sample_rate = 48000;
    unsigned int channel_count = 2;
    static char *keywords[] = {"mode", "sample_rate", "channel_count", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|iII:DynamicsState",
        keywords, &mode, &sample_rate, &channel_count)) return -1;
    if (sample_rate < 1) {
        PyErr_SetString(PyExc_ValueError, "sample_rate must be at least 1");
        return -1;
    }
    if (channel_count < 1 || channel_count > 2) {
        PyErr_SetString(PyExc_ValueError, "channel_count must be 1 or 2");
        return -1;
    }
    audiodsp_dynamics_config_init(&self->config, mode, sample_rate);
    audiodsp_dynamics_state_init(&self->state);
    audiodsp_dynamics_set_channel_count(&self->config, &self->state,
        channel_count);
    PyMem_Free(self->lookahead);
    self->lookahead = NULL;
    return 0;
}

static PyObject *dynamics_state_set_sample_rate(
    audiodsp_dynamics_object_t *self, PyObject *argument) {
    unsigned long rate = PyLong_AsUnsignedLong(argument);
    if (PyErr_Occurred()) return NULL;
    if (rate < 1 || rate > UINT32_MAX) {
        PyErr_SetString(PyExc_ValueError, "sample_rate must be at least 1");
        return NULL;
    }
    self->config.sample_rate = (uint32_t)rate;
    Py_RETURN_NONE;
}

static PyObject *dynamics_state_configure(audiodsp_dynamics_object_t *self,
    PyObject *args) {
    int option;
    double value;
    if (!PyArg_ParseTuple(args, "id:configure", &option, &value)) return NULL;
    if (option < AUDIODSP_DYNAMICS_OPT_THRESHOLD_DB ||
        option >= AUDIODSP_DYNAMICS_OPT_COUNT) {
        PyErr_SetString(PyExc_ValueError, "unknown dynamics option");
        return NULL;
    }
    audiodsp_dynamics_configure(&self->config,
        (audiodsp_dynamics_option_t)option, (float)value);
    if (dynamics_ensure_lookahead(self) < 0) return NULL;
    Py_RETURN_NONE;
}

static PyObject *dynamics_state_finish(audiodsp_dynamics_object_t *self,
    PyObject *unused) {
    audiodsp_dynamics_config_finish(&self->config);
    Py_RETURN_NONE;
}

static PyObject *dynamics_state_reset(audiodsp_dynamics_object_t *self,
    PyObject *unused) {
    audiodsp_dynamics_reset(&self->state);
    Py_RETURN_NONE;
}

// `process(audio)` is the original. `process(audio, key)` hands the detector
// a different stream from the one the gain lands on, which is what an external
// key input is; the key has to carry at least as many frames as the audio,
// because the wrapper is the one that decides how short a run may be.
static PyObject *dynamics_state_process(audiodsp_dynamics_object_t *self,
    PyObject *args) {
    PyObject *audio = NULL;
    PyObject *key_object = NULL;
    if (!PyArg_ParseTuple(args, "O|O:process", &audio, &key_object)) {
        return NULL;
    }
    Py_buffer input = {0};
    if (PyObject_GetBuffer(audio, &input, PyBUF_SIMPLE) < 0) return NULL;
    const Py_ssize_t width = 2 * (Py_ssize_t)self->config.channel_count;
    if (input.len % width) {
        PyBuffer_Release(&input);
        PyErr_SetString(PyExc_ValueError,
            "input must be whole 16-bit frames for the configured channel count");
        return NULL;
    }
    Py_buffer key = {0};
    int have_key = 0;
    if (key_object != NULL && key_object != Py_None) {
        if (PyObject_GetBuffer(key_object, &key, PyBUF_SIMPLE) < 0) {
            PyBuffer_Release(&input);
            return NULL;
        }
        if (key.len < input.len) {
            PyBuffer_Release(&key);
            PyBuffer_Release(&input);
            PyErr_SetString(PyExc_ValueError,
                "the key must carry at least as many frames as the input");
            return NULL;
        }
        have_key = 1;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, input.len);
    if (result != NULL) {
        audiodsp_dynamics_process_s16_key(&self->config, &self->state,
            (int16_t *)PyBytes_AS_STRING(result), (const int16_t *)input.buf,
            have_key ? (const int16_t *)key.buf : NULL,
            (uint32_t)(input.len / width));
    }
    if (have_key) {
        PyBuffer_Release(&key);
    }
    PyBuffer_Release(&input);
    return result;
}

static PyObject *dynamics_state_gain_reduction_db(
    audiodsp_dynamics_object_t *self, void *closure) {
    return PyFloat_FromDouble((double)self->state.gain_reduction_db);
}

static PyObject *dynamics_state_get_sample_rate(
    audiodsp_dynamics_object_t *self, void *closure) {
    return PyLong_FromUnsignedLong(self->config.sample_rate);
}

static PyMethodDef dynamics_state_methods[] = {
    {"set_sample_rate", (PyCFunction)dynamics_state_set_sample_rate, METH_O, NULL},
    {"configure", (PyCFunction)dynamics_state_configure, METH_VARARGS, NULL},
    {"finish", (PyCFunction)dynamics_state_finish, METH_NOARGS, NULL},
    {"reset", (PyCFunction)dynamics_state_reset, METH_NOARGS, NULL},
    {"process", (PyCFunction)dynamics_state_process, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef dynamics_state_getset[] = {
    {"gain_reduction_db", (getter)dynamics_state_gain_reduction_db, NULL, NULL, NULL},
    {"sample_rate", (getter)dynamics_state_get_sample_rate, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyType_Slot dynamics_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, dynamics_state_init},
    {Py_tp_dealloc, dynamics_state_dealloc},
    {Py_tp_methods, dynamics_state_methods},
    {Py_tp_getset, dynamics_state_getset},
    {0, NULL},
};

static PyType_Spec dynamics_state_spec = {
    .name = "_audiodsp.DynamicsState",
    .basicsize = sizeof(audiodsp_dynamics_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = dynamics_state_slots,
};

typedef struct {
    PyObject_HEAD
    audiodsp_splitter_state_t state;
} audiodsp_splitter_object_t;

static int splitter_ring_init(audiodsp_splitter_object_t *self,
    PyObject *args, PyObject *kwargs) {
    int taps = 2;
    int channel_count = 2;
    static char *keywords[] = {"taps", "channel_count", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|ii:SplitterRing",
        keywords, &taps, &channel_count)) return -1;
    if (taps < 1 || taps > (int)AUDIODSP_SPLITTER_MAX_TAPS) {
        PyErr_SetString(PyExc_ValueError, "taps must be 1..4");
        return -1;
    }
    audiodsp_splitter_init(&self->state, (uint32_t)taps);
    if (channel_count < 1 || channel_count > 2) {
        PyErr_SetString(PyExc_ValueError, "channel_count must be 1 or 2");
        return -1;
    }
    audiodsp_splitter_set_channel_count(&self->state,
        (uint32_t)channel_count);
    return 0;
}

static int splitter_ring_check_tap(audiodsp_splitter_object_t *self, int tap) {
    if (tap < 0 || (uint32_t)tap >= self->state.tap_count) {
        PyErr_SetString(PyExc_ValueError, "tap index out of range");
        return -1;
    }
    return 0;
}

static PyObject *splitter_ring_write(audiodsp_splitter_object_t *self,
    PyObject *argument) {
    Py_buffer input = {0};
    if (PyObject_GetBuffer(argument, &input, PyBUF_SIMPLE) < 0) return NULL;
    const Py_ssize_t width = 2 * (Py_ssize_t)self->state.channel_count;
    if (input.len % width) {
        PyBuffer_Release(&input);
        PyErr_SetString(PyExc_ValueError,
            "input must be whole frames for the configured channel count");
        return NULL;
    }
    const uint32_t taken = audiodsp_splitter_write(&self->state,
        (const int16_t *)input.buf, (uint32_t)(input.len / width));
    PyBuffer_Release(&input);
    // The frame count actually appended, which is one ring at most. The caller
    // keeps the rest and offers it next time rather than letting it be written
    // over unread. audiodsp#87.
    return PyLong_FromUnsignedLong((unsigned long)taken);
}

static PyObject *splitter_ring_starved(audiodsp_splitter_object_t *self,
    PyObject *argument) {
    int tap = (int)PyLong_AsLong(argument);
    if (PyErr_Occurred() || splitter_ring_check_tap(self, tap) < 0) return NULL;
    return PyBool_FromLong(audiodsp_splitter_starved(&self->state,
        (uint32_t)tap));
}

static PyObject *splitter_ring_take(audiodsp_splitter_object_t *self,
    PyObject *argument) {
    int tap = (int)PyLong_AsLong(argument);
    if (PyErr_Occurred() || splitter_ring_check_tap(self, tap) < 0) return NULL;
    uint32_t start = 0;
    const uint32_t run = audiodsp_splitter_take(&self->state, (uint32_t)tap,
        &start);
    if (self->state.channel_count == 2u) {
        return PyBytes_FromStringAndSize(
            (const char *)&self->state.ring[start * 2u],
            (Py_ssize_t)run * 4);
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)run * 2);
    if (result == NULL) return NULL;
    int16_t *out = (int16_t *)PyBytes_AS_STRING(result);
    for (uint32_t frame = 0; frame < run; ++frame) {
        out[frame] = self->state.ring[(start + frame) %
            AUDIODSP_SPLITTER_RING_FRAMES * 2u];
    }
    return result;
}

static PyMethodDef splitter_ring_methods[] = {
    {"write", (PyCFunction)splitter_ring_write, METH_O, NULL},
    {"starved", (PyCFunction)splitter_ring_starved, METH_O, NULL},
    {"take", (PyCFunction)splitter_ring_take, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyType_Slot splitter_ring_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, splitter_ring_init},
    {Py_tp_methods, splitter_ring_methods},
    {0, NULL},
};

static PyType_Spec splitter_ring_spec = {
    .name = "_audiodsp.SplitterRing",
    .basicsize = sizeof(audiodsp_splitter_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = splitter_ring_slots,
};

// audioecho.FeedbackDelay's line and filters. Unlike multiply_s16() below --
// which is a plain function, because the multiply carries no state at all and
// the Python side already holds its one setting -- this one is all state: the
// delay line dwarfs everything else in the object. So it is a type, the way
// DynamicsState is.

typedef struct {
    PyObject_HEAD
    audiodsp_feedback_delay_config_t config;
    audiodsp_feedback_delay_state_t state;
    int16_t *line;
    // The `wow_shape` table, held open for as long as the config points at
    // it: the DSP borrows the samples and would otherwise read whatever the
    // allocator put there next.
    Py_buffer wow_shape;
    int wow_shape_held;
} audiodsp_feedback_delay_object_t;

static void feedback_delay_release_shape(
    audiodsp_feedback_delay_object_t *self) {
    if (self->wow_shape_held) {
        PyBuffer_Release(&self->wow_shape);
        self->wow_shape_held = 0;
    }
}

static int feedback_delay_state_init(audiodsp_feedback_delay_object_t *self,
    PyObject *args, PyObject *kwargs) {
    unsigned int sample_rate = 48000;
    unsigned int channel_count = 2;
    double max_delay_ms = 250.0;
    static char *keywords[] = {"sample_rate", "max_delay_ms",
                               "channel_count", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|IdI:FeedbackDelayState",
        keywords, &sample_rate, &max_delay_ms, &channel_count)) return -1;
    if (sample_rate < 1) {
        PyErr_SetString(PyExc_ValueError, "sample_rate must be at least 1");
        return -1;
    }
    if (max_delay_ms <= 0.0) {
        PyErr_SetString(PyExc_ValueError, "max_delay_ms must be positive");
        return -1;
    }
    if (channel_count < 1 || channel_count > 2) {
        PyErr_SetString(PyExc_ValueError, "channel_count must be 1 or 2");
        return -1;
    }
    uint32_t frames = (uint32_t)((double)sample_rate * max_delay_ms / 1000.0);
    if (frames < 2) frames = 2;
    int16_t *line = PyMem_Calloc((size_t)frames * 2u, sizeof(int16_t));
    if (line == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    PyMem_Free(self->line);
    self->line = line;
    // config_init clears the shape pointer, so the view has to go with it.
    feedback_delay_release_shape(self);
    audiodsp_feedback_delay_config_init(&self->config, sample_rate, frames);
    audiodsp_feedback_delay_set_channel_count(&self->config, channel_count);
    audiodsp_feedback_delay_state_init(&self->state, line);
    audiodsp_feedback_delay_configure(&self->config,
        AUDIODSP_FEEDBACK_DELAY_OPT_DELAY_MS, (float)max_delay_ms * 0.5f);
    return 0;
}

static void feedback_delay_state_dealloc(
    audiodsp_feedback_delay_object_t *self) {
    PyTypeObject *type = Py_TYPE(self);
    feedback_delay_release_shape(self);
    PyMem_Free(self->line);
    self->line = NULL;
    type->tp_free((PyObject *)self);
    Py_DECREF(type);
}

static PyObject *feedback_delay_state_configure(
    audiodsp_feedback_delay_object_t *self, PyObject *args) {
    int option;
    double value;
    if (!PyArg_ParseTuple(args, "id:configure", &option, &value)) return NULL;
    if (option < AUDIODSP_FEEDBACK_DELAY_OPT_DELAY_MS ||
        option > AUDIODSP_FEEDBACK_DELAY_OPT_LOOP_WINDOW_MS) {
        PyErr_SetString(PyExc_ValueError, "unknown feedback delay option");
        return NULL;
    }
    audiodsp_feedback_delay_configure(&self->config,
        (audiodsp_feedback_delay_option_t)option, (float)value);
    Py_RETURN_NONE;
}

// `wow_shape` is a buffer, not a number, so it does not go through
// configure(): None puts the built-in sine back, anything else is one period
// of int16 Q15 whose length is a power of two.
static PyObject *feedback_delay_state_wow_shape(
    audiodsp_feedback_delay_object_t *self, PyObject *argument) {
    if (argument == Py_None) {
        audiodsp_feedback_delay_set_wow_shape(&self->config, NULL, 0);
        feedback_delay_release_shape(self);
        Py_RETURN_NONE;
    }
    Py_buffer view;
    if (PyObject_GetBuffer(argument, &view, PyBUF_SIMPLE) < 0) return NULL;
    const Py_ssize_t width = (Py_ssize_t)sizeof(int16_t);
    if (view.len % width != 0 ||
        !audiodsp_feedback_delay_set_wow_shape(&self->config,
            (const int16_t *)view.buf, (uint32_t)(view.len / width))) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError,
            "wow_shape must be 2 to 4096 int16 samples, a power of two");
        return NULL;
    }
    feedback_delay_release_shape(self);
    self->wow_shape = view;
    self->wow_shape_held = 1;
    Py_RETURN_NONE;
}

static PyObject *feedback_delay_state_finish(
    audiodsp_feedback_delay_object_t *self, PyObject *unused) {
    audiodsp_feedback_delay_config_finish(&self->config);
    Py_RETURN_NONE;
}

static PyObject *feedback_delay_state_reset(
    audiodsp_feedback_delay_object_t *self, PyObject *unused) {
    audiodsp_feedback_delay_reset(&self->state, &self->config);
    Py_RETURN_NONE;
}

static PyObject *feedback_delay_state_process(
    audiodsp_feedback_delay_object_t *self, PyObject *argument) {
    Py_buffer input = {0};
    if (PyObject_GetBuffer(argument, &input, PyBUF_SIMPLE) < 0) return NULL;
    const Py_ssize_t width = 2 * (Py_ssize_t)self->config.channel_count;
    if (input.len % width) {
        PyBuffer_Release(&input);
        PyErr_SetString(PyExc_ValueError,
            "input must be whole 16-bit frames for the configured channel count");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, input.len);
    if (result != NULL) {
        audiodsp_feedback_delay_process_s16(&self->config, &self->state,
            (int16_t *)PyBytes_AS_STRING(result), (const int16_t *)input.buf,
            (uint32_t)(input.len / width));
    }
    PyBuffer_Release(&input);
    return result;
}

static PyMethodDef feedback_delay_state_methods[] = {
    {"configure", (PyCFunction)feedback_delay_state_configure, METH_VARARGS, NULL},
    {"set_wow_shape", (PyCFunction)feedback_delay_state_wow_shape, METH_O, NULL},
    {"finish", (PyCFunction)feedback_delay_state_finish, METH_NOARGS, NULL},
    {"reset", (PyCFunction)feedback_delay_state_reset, METH_NOARGS, NULL},
    {"process", (PyCFunction)feedback_delay_state_process, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyType_Slot feedback_delay_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, feedback_delay_state_init},
    {Py_tp_dealloc, feedback_delay_state_dealloc},
    {Py_tp_methods, feedback_delay_state_methods},
    {0, NULL},
};

static PyType_Spec feedback_delay_state_spec = {
    .name = "_audiodsp.FeedbackDelayState",
    .basicsize = sizeof(audiodsp_feedback_delay_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = feedback_delay_state_slots,
};

// audiobiquad's two kernels. Both are state plus a small config, so both are
// types rather than plain functions -- the way DynamicsState and
// FeedbackDelayState are, and unlike multiply_s16() below.
//
// The Python side (src/cpython/audiobiquad.py) resolves synthio BlockInputs
// once per chunk and hands the floats down here, which is exactly what the
// MicroPython bindings do with synthio_block_slot_get_limited(). Neither
// target evaluates a block inside the sample loop.

typedef struct {
    PyObject_HEAD
    audiodsp_biquad_f32_config_t config;
    audiodsp_biquad_f32_state_t state;
} audiodsp_biquad_f32_object_t;

static int biquad_f32_state_init(audiodsp_biquad_f32_object_t *self,
    PyObject *args, PyObject *kwargs) {
    unsigned int sample_rate = 48000;
    unsigned int channel_count = 2;
    static char *keywords[] = {"sample_rate", "channel_count", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|II:BiquadF32State",
        keywords, &sample_rate, &channel_count)) return -1;
    if (sample_rate < 1) {
        PyErr_SetString(PyExc_ValueError, "sample_rate must be at least 1");
        return -1;
    }
    if (channel_count < 1 || channel_count > 2) {
        PyErr_SetString(PyExc_ValueError, "channel_count must be 1 or 2");
        return -1;
    }
    audiodsp_biquad_f32_config_init(&self->config, sample_rate, channel_count);
    audiodsp_biquad_f32_state_init(&self->state);
    return 0;
}

static PyObject *biquad_f32_state_configure(
    audiodsp_biquad_f32_object_t *self, PyObject *args) {
    int option;
    double value;
    if (!PyArg_ParseTuple(args, "id:configure", &option, &value)) return NULL;
    if (option < AUDIODSP_BIQUAD_F32_OPT_MODE ||
        option > AUDIODSP_BIQUAD_F32_OPT_MIX) {
        PyErr_SetString(PyExc_ValueError, "unknown biquad option");
        return NULL;
    }
    audiodsp_biquad_f32_configure(&self->config,
        (audiodsp_biquad_f32_option_t)option, (float)value);
    Py_RETURN_NONE;
}

static PyObject *biquad_f32_state_finish(audiodsp_biquad_f32_object_t *self,
    PyObject *unused) {
    (void)unused;
    audiodsp_biquad_f32_config_finish(&self->config);
    Py_RETURN_NONE;
}

static PyObject *biquad_f32_state_reset(audiodsp_biquad_f32_object_t *self,
    PyObject *unused) {
    (void)unused;
    audiodsp_biquad_f32_reset(&self->state);
    Py_RETURN_NONE;
}

// The five normalized coefficients, so a test can compare this kernel with
// `shared/audiodsp_biquad.c`'s fixed-point ones without rendering anything.
static PyObject *biquad_f32_state_coefficients(
    audiodsp_biquad_f32_object_t *self, PyObject *unused) {
    (void)unused;
    audiodsp_biquad_f32_config_finish(&self->config);
    return Py_BuildValue("(ddddd)", (double)self->config.b0,
        (double)self->config.b1, (double)self->config.b2,
        (double)self->config.a1, (double)self->config.a2);
}

static PyObject *biquad_f32_state_process(audiodsp_biquad_f32_object_t *self,
    PyObject *argument) {
    Py_buffer input = {0};
    if (PyObject_GetBuffer(argument, &input, PyBUF_SIMPLE) < 0) return NULL;
    const Py_ssize_t width = 2 * (Py_ssize_t)self->config.channel_count;
    if (input.len % width) {
        PyBuffer_Release(&input);
        PyErr_SetString(PyExc_ValueError,
            "input must be whole 16-bit frames for the configured channel count");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, input.len);
    if (result != NULL) {
        audiodsp_biquad_f32_process_s16(&self->config, &self->state,
            (int16_t *)PyBytes_AS_STRING(result), (const int16_t *)input.buf,
            (uint32_t)(input.len / width));
    }
    PyBuffer_Release(&input);
    return result;
}

static PyMethodDef biquad_f32_state_methods[] = {
    {"configure", (PyCFunction)biquad_f32_state_configure, METH_VARARGS, NULL},
    {"finish", (PyCFunction)biquad_f32_state_finish, METH_NOARGS, NULL},
    {"reset", (PyCFunction)biquad_f32_state_reset, METH_NOARGS, NULL},
    {"coefficients", (PyCFunction)biquad_f32_state_coefficients, METH_NOARGS, NULL},
    {"process", (PyCFunction)biquad_f32_state_process, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyType_Slot biquad_f32_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, biquad_f32_state_init},
    {Py_tp_methods, biquad_f32_state_methods},
    {0, NULL},
};

static PyType_Spec biquad_f32_state_spec = {
    .name = "_audiodsp.BiquadF32State",
    .basicsize = sizeof(audiodsp_biquad_f32_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = biquad_f32_state_slots,
};

typedef struct {
    PyObject_HEAD
    audiodsp_allpass_f32_config_t config;
    audiodsp_allpass_f32_state_t state;
    float *stage_state;
} audiodsp_allpass_f32_object_t;

static int allpass_f32_state_init(audiodsp_allpass_f32_object_t *self,
    PyObject *args, PyObject *kwargs) {
    unsigned int sample_rate = 48000;
    unsigned int channel_count = 2;
    unsigned int stages = 4;
    static char *keywords[] = {"sample_rate", "channel_count", "stages", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|III:AllPassF32State",
        keywords, &sample_rate, &channel_count, &stages)) return -1;
    if (sample_rate < 1) {
        PyErr_SetString(PyExc_ValueError, "sample_rate must be at least 1");
        return -1;
    }
    if (channel_count < 1 || channel_count > 2) {
        PyErr_SetString(PyExc_ValueError, "channel_count must be 1 or 2");
        return -1;
    }
    if (stages < 1 || stages > AUDIODSP_FILTER_F32_MAX_STAGES) {
        PyErr_SetString(PyExc_ValueError,
            "stages must be between 1 and AUDIODSP_FILTER_F32_MAX_STAGES");
        return -1;
    }
    const uint32_t count = (uint32_t)(channel_count * stages);
    float *lanes = PyMem_Calloc(count, sizeof(float));
    if (lanes == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    PyMem_Free(self->stage_state);
    self->stage_state = lanes;
    audiodsp_allpass_f32_config_init(&self->config, sample_rate, channel_count,
        stages);
    audiodsp_allpass_f32_state_init(&self->state, lanes, count);
    return 0;
}

static void allpass_f32_state_dealloc(audiodsp_allpass_f32_object_t *self) {
    PyTypeObject *type = Py_TYPE(self);
    PyMem_Free(self->stage_state);
    self->stage_state = NULL;
    type->tp_free((PyObject *)self);
    Py_DECREF(type);
}

static PyObject *allpass_f32_state_configure(
    audiodsp_allpass_f32_object_t *self, PyObject *args) {
    int option;
    double value;
    if (!PyArg_ParseTuple(args, "id:configure", &option, &value)) return NULL;
    if (option < AUDIODSP_ALLPASS_F32_OPT_FREQUENCY ||
        option > AUDIODSP_ALLPASS_F32_OPT_MIX) {
        PyErr_SetString(PyExc_ValueError, "unknown all-pass option");
        return NULL;
    }
    audiodsp_allpass_f32_configure(&self->config,
        (audiodsp_allpass_f32_option_t)option, (float)value);
    Py_RETURN_NONE;
}

static PyObject *allpass_f32_state_finish(audiodsp_allpass_f32_object_t *self,
    PyObject *unused) {
    (void)unused;
    audiodsp_allpass_f32_config_finish(&self->config);
    Py_RETURN_NONE;
}

static PyObject *allpass_f32_state_reset(audiodsp_allpass_f32_object_t *self,
    PyObject *unused) {
    (void)unused;
    audiodsp_allpass_f32_reset(&self->state);
    Py_RETURN_NONE;
}

static PyObject *allpass_f32_state_coefficient(
    audiodsp_allpass_f32_object_t *self, PyObject *unused) {
    (void)unused;
    audiodsp_allpass_f32_config_finish(&self->config);
    return PyFloat_FromDouble((double)self->config.coefficient);
}

static PyObject *allpass_f32_state_process(
    audiodsp_allpass_f32_object_t *self, PyObject *argument) {
    Py_buffer input = {0};
    if (PyObject_GetBuffer(argument, &input, PyBUF_SIMPLE) < 0) return NULL;
    const Py_ssize_t width = 2 * (Py_ssize_t)self->config.channel_count;
    if (input.len % width) {
        PyBuffer_Release(&input);
        PyErr_SetString(PyExc_ValueError,
            "input must be whole 16-bit frames for the configured channel count");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, input.len);
    if (result != NULL) {
        audiodsp_allpass_f32_process_s16(&self->config, &self->state,
            (int16_t *)PyBytes_AS_STRING(result), (const int16_t *)input.buf,
            (uint32_t)(input.len / width));
    }
    PyBuffer_Release(&input);
    return result;
}

static PyMethodDef allpass_f32_state_methods[] = {
    {"configure", (PyCFunction)allpass_f32_state_configure, METH_VARARGS, NULL},
    {"finish", (PyCFunction)allpass_f32_state_finish, METH_NOARGS, NULL},
    {"reset", (PyCFunction)allpass_f32_state_reset, METH_NOARGS, NULL},
    {"coefficient", (PyCFunction)allpass_f32_state_coefficient, METH_NOARGS, NULL},
    {"process", (PyCFunction)allpass_f32_state_process, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyType_Slot allpass_f32_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, allpass_f32_state_init},
    {Py_tp_dealloc, allpass_f32_state_dealloc},
    {Py_tp_methods, allpass_f32_state_methods},
    {0, NULL},
};

static PyType_Spec allpass_f32_state_spec = {
    .name = "_audiodsp.AllPassF32State",
    .basicsize = sizeof(audiodsp_allpass_f32_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = allpass_f32_state_slots,
};

// audioshaper.Waveshaper's half-band memories, play position and curve. A
// type rather than a plain function, the way FeedbackDelayState is: the
// half-bands carry state across blocks, and the curve is an allocation.

typedef struct {
    PyObject_HEAD
    audiodsp_shaper_config_t config;
    audiodsp_shaper_state_t state;
    int16_t *curve;
} audiodsp_shaper_object_t;

static int waveshaper_state_init(audiodsp_shaper_object_t *self,
    PyObject *args, PyObject *kwargs) {
    unsigned int sample_rate = 48000;
    unsigned int oversample = 4;
    unsigned int channel_count = 2;
    static char *keywords[] = {"sample_rate", "oversample", "channel_count",
                               NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|III:WaveshaperState",
        keywords, &sample_rate, &oversample, &channel_count)) return -1;
    if (sample_rate < 1) {
        PyErr_SetString(PyExc_ValueError, "sample_rate must be at least 1");
        return -1;
    }
    if (oversample != 1 && oversample != 2 && oversample != 4 &&
        oversample != 8) {
        PyErr_SetString(PyExc_ValueError, "oversample must be 1, 2, 4 or 8");
        return -1;
    }
    if (channel_count < 1 || channel_count > 2) {
        PyErr_SetString(PyExc_ValueError, "channel_count must be 1 or 2");
        return -1;
    }
    audiodsp_shaper_config_init(&self->config, sample_rate, oversample);
    audiodsp_shaper_set_channel_count(&self->config, channel_count);
    audiodsp_shaper_state_init(&self->state);
    return 0;
}

static void waveshaper_state_dealloc(audiodsp_shaper_object_t *self) {
    PyTypeObject *type = Py_TYPE(self);
    PyMem_Free(self->curve);
    self->curve = NULL;
    type->tp_free((PyObject *)self);
    Py_DECREF(type);
}

// The curve is copied rather than borrowed: it is data the caller computed
// once and may well drop, and 4096 points is 8 KB -- small beside a delay
// line, and the only way the config's pointer can be relied on for the life
// of the node.
static PyObject *waveshaper_state_load_curve(audiodsp_shaper_object_t *self,
    PyObject *argument) {
    Py_buffer curve = {0};
    if (PyObject_GetBuffer(argument, &curve, PyBUF_SIMPLE) < 0) return NULL;
    if (curve.len % 2 || curve.len < 4) {
        PyBuffer_Release(&curve);
        PyErr_SetString(PyExc_ValueError,
            "curve must be at least two whole int16 points");
        return NULL;
    }
    int16_t *copy = PyMem_Malloc((size_t)curve.len);
    if (copy == NULL) {
        PyBuffer_Release(&curve);
        PyErr_NoMemory();
        return NULL;
    }
    memcpy(copy, curve.buf, (size_t)curve.len);
    PyMem_Free(self->curve);
    self->curve = copy;
    audiodsp_shaper_set_curve(&self->config, copy,
        (uint32_t)(curve.len / 2));
    PyBuffer_Release(&curve);
    Py_RETURN_NONE;
}

static PyObject *waveshaper_state_configure(audiodsp_shaper_object_t *self,
    PyObject *args) {
    int option;
    double value;
    if (!PyArg_ParseTuple(args, "id:configure", &option, &value)) return NULL;
    if (option < AUDIODSP_SHAPER_OPT_PRE_GAIN ||
        option > AUDIODSP_SHAPER_OPT_HYSTERESIS_BIAS) {
        PyErr_SetString(PyExc_ValueError, "unknown waveshaper option");
        return NULL;
    }
    audiodsp_shaper_configure(&self->config,
        (audiodsp_shaper_option_t)option, (float)value);
    Py_RETURN_NONE;
}

static PyObject *waveshaper_state_finish(audiodsp_shaper_object_t *self,
    PyObject *unused) {
    audiodsp_shaper_config_finish(&self->config);
    Py_RETURN_NONE;
}

static PyObject *waveshaper_state_reset(audiodsp_shaper_object_t *self,
    PyObject *unused) {
    audiodsp_shaper_reset(&self->state);
    Py_RETURN_NONE;
}

static PyObject *waveshaper_state_process(audiodsp_shaper_object_t *self,
    PyObject *argument) {
    Py_buffer input = {0};
    if (PyObject_GetBuffer(argument, &input, PyBUF_SIMPLE) < 0) return NULL;
    const Py_ssize_t width = 2 * (Py_ssize_t)self->config.channel_count;
    if (input.len % width) {
        PyBuffer_Release(&input);
        PyErr_SetString(PyExc_ValueError,
            "input must be whole 16-bit frames for the configured channel count");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, input.len);
    if (result != NULL) {
        audiodsp_shaper_process_s16(&self->config, &self->state,
            (int16_t *)PyBytes_AS_STRING(result), (const int16_t *)input.buf,
            (uint32_t)(input.len / width));
    }
    PyBuffer_Release(&input);
    return result;
}

static PyMethodDef waveshaper_state_methods[] = {
    {"load_curve", (PyCFunction)waveshaper_state_load_curve, METH_O, NULL},
    {"configure", (PyCFunction)waveshaper_state_configure, METH_VARARGS, NULL},
    {"finish", (PyCFunction)waveshaper_state_finish, METH_NOARGS, NULL},
    {"reset", (PyCFunction)waveshaper_state_reset, METH_NOARGS, NULL},
    {"process", (PyCFunction)waveshaper_state_process, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyType_Slot waveshaper_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, waveshaper_state_init},
    {Py_tp_dealloc, waveshaper_state_dealloc},
    {Py_tp_methods, waveshaper_state_methods},
    {0, NULL},
};

static PyType_Spec waveshaper_state_spec = {
    .name = "_audiodsp.WaveshaperState",
    .basicsize = sizeof(audiodsp_shaper_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = waveshaper_state_slots,
};

// audioshaper.SampleHold's ratio and held frame. A type rather than a plain
// function because the accumulator is what makes the node exact: it has to
// survive from one block to the next, and the Python side must not be able to
// lose it. The whole of it is two integers and four bytes.

typedef struct {
    PyObject_HEAD
    audiodsp_samplehold_config_t config;
    audiodsp_samplehold_state_t state;
} audiodsp_samplehold_object_t;

static int samplehold_check_ratio(unsigned int num, unsigned int den) {
    if (!audiodsp_samplehold_ratio_ok(num, den)) {
        PyErr_SetString(PyExc_ValueError,
            "num and den must be whole, den <= num (a hold cannot invent "
            "frames)");
        return -1;
    }
    return 0;
}

static int samplehold_state_init(audiodsp_samplehold_object_t *self,
    PyObject *args, PyObject *kwargs) {
    unsigned int num = 1;
    unsigned int den = 1;
    static char *keywords[] = {"num", "den", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|II:SampleHoldState",
        keywords, &num, &den)) return -1;
    if (samplehold_check_ratio(num, den) < 0) return -1;
    audiodsp_samplehold_config_init(&self->config, num, den);
    audiodsp_samplehold_state_init(&self->state, &self->config);
    return 0;
}

static void samplehold_state_dealloc(audiodsp_samplehold_object_t *self) {
    PyTypeObject *type = Py_TYPE(self);
    type->tp_free((PyObject *)self);
    Py_DECREF(type);
}

static PyObject *samplehold_state_configure(audiodsp_samplehold_object_t *self,
    PyObject *args) {
    unsigned int num;
    unsigned int den;
    if (!PyArg_ParseTuple(args, "II:configure", &num, &den)) return NULL;
    if (samplehold_check_ratio(num, den) < 0) return NULL;
    if (audiodsp_samplehold_config_set(&self->config, num, den)) {
        audiodsp_samplehold_reset(&self->state, &self->config);
    }
    Py_RETURN_NONE;
}

// The reduced pair, read back from the kernel rather than reduced again in
// Python: the gcd is part of the arithmetic all three targets share, so the
// number a class discloses has to come from the same place on each of them.
static PyObject *samplehold_state_ratio(audiodsp_samplehold_object_t *self,
    PyObject *unused) {
    (void)unused;
    return Py_BuildValue("(kk)", (unsigned long)self->config.num,
        (unsigned long)self->config.den);
}

static PyObject *samplehold_state_reset(audiodsp_samplehold_object_t *self,
    PyObject *unused) {
    (void)unused;
    audiodsp_samplehold_reset(&self->state, &self->config);
    Py_RETURN_NONE;
}

static PyObject *samplehold_state_process(audiodsp_samplehold_object_t *self,
    PyObject *args) {
    Py_buffer input = {0};
    unsigned int frame_bytes = 0;
    if (!PyArg_ParseTuple(args, "y*I:process", &input, &frame_bytes)) {
        return NULL;
    }
    if (frame_bytes < 1 ||
        frame_bytes > AUDIODSP_SAMPLEHOLD_MAX_FRAME_BYTES ||
        input.len % (Py_ssize_t)frame_bytes) {
        PyBuffer_Release(&input);
        PyErr_SetString(PyExc_ValueError,
            "input must be whole frames of 1 to 4 bytes");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, input.len);
    if (result != NULL) {
        audiodsp_samplehold_process(&self->config, &self->state,
            (uint8_t *)PyBytes_AS_STRING(result), (const uint8_t *)input.buf,
            (uint32_t)(input.len / (Py_ssize_t)frame_bytes), frame_bytes);
    }
    PyBuffer_Release(&input);
    return result;
}

static PyMethodDef samplehold_state_methods[] = {
    {"configure", (PyCFunction)samplehold_state_configure, METH_VARARGS, NULL},
    {"ratio", (PyCFunction)samplehold_state_ratio, METH_NOARGS, NULL},
    {"reset", (PyCFunction)samplehold_state_reset, METH_NOARGS, NULL},
    {"process", (PyCFunction)samplehold_state_process, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL},
};

static PyType_Slot samplehold_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, samplehold_state_init},
    {Py_tp_dealloc, samplehold_state_dealloc},
    {Py_tp_methods, samplehold_state_methods},
    {0, NULL},
};

static PyType_Spec samplehold_state_spec = {
    .name = "_audiodsp.SampleHoldState",
    .basicsize = sizeof(audiodsp_samplehold_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = samplehold_state_slots,
};

// audioladder.Ladder's integrators and solver history. Small enough to sit in
// the object -- unlike FeedbackDelayState above there is no line to allocate,
// four floats a stage and three more a channel is the whole of it -- but a
// type all the same, because it is state and the Python side must not be able
// to lose it between blocks.

typedef struct {
    PyObject_HEAD
    audiodsp_ladder_config_t config;
    audiodsp_ladder_state_t state;
} audiodsp_ladder_object_t;

static int ladder_state_init(audiodsp_ladder_object_t *self, PyObject *args,
    PyObject *kwargs) {
    unsigned int sample_rate = 48000;
    unsigned int channel_count = 2;
    static char *keywords[] = {"sample_rate", "channel_count", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|II:LadderState",
        keywords, &sample_rate, &channel_count)) return -1;
    if (sample_rate < 1) {
        PyErr_SetString(PyExc_ValueError, "sample_rate must be at least 1");
        return -1;
    }
    if (channel_count < 1 || channel_count > 2) {
        PyErr_SetString(PyExc_ValueError, "channel_count must be 1 or 2");
        return -1;
    }
    audiodsp_ladder_config_init(&self->config, sample_rate);
    audiodsp_ladder_set_channel_count(&self->config, channel_count);
    audiodsp_ladder_state_init(&self->state);
    return 0;
}

static PyObject *ladder_state_configure(audiodsp_ladder_object_t *self,
    PyObject *args) {
    int option;
    double value;
    if (!PyArg_ParseTuple(args, "id:configure", &option, &value)) return NULL;
    if (option < AUDIODSP_LADDER_OPT_CUTOFF_HZ ||
        option > AUDIODSP_LADDER_OPT_MIX) {
        PyErr_SetString(PyExc_ValueError, "unknown ladder option");
        return NULL;
    }
    audiodsp_ladder_configure(&self->config,
        (audiodsp_ladder_option_t)option, (float)value);
    Py_RETURN_NONE;
}

static PyObject *ladder_state_finish(audiodsp_ladder_object_t *self,
    PyObject *unused) {
    audiodsp_ladder_config_finish(&self->config);
    Py_RETURN_NONE;
}

static PyObject *ladder_state_reset(audiodsp_ladder_object_t *self,
    PyObject *unused) {
    audiodsp_ladder_reset(&self->state);
    Py_RETURN_NONE;
}

static PyObject *ladder_state_process(audiodsp_ladder_object_t *self,
    PyObject *argument) {
    Py_buffer input = {0};
    if (PyObject_GetBuffer(argument, &input, PyBUF_SIMPLE) < 0) return NULL;
    const Py_ssize_t width = 2 * (Py_ssize_t)self->config.channel_count;
    if (input.len % width) {
        PyBuffer_Release(&input);
        PyErr_SetString(PyExc_ValueError,
            "input must be whole 16-bit frames for the configured channel count");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, input.len);
    if (result != NULL) {
        audiodsp_ladder_process_s16(&self->config, &self->state,
            (int16_t *)PyBytes_AS_STRING(result), (const int16_t *)input.buf,
            (uint32_t)(input.len / width));
    }
    PyBuffer_Release(&input);
    return result;
}

static PyMethodDef ladder_state_methods[] = {
    {"configure", (PyCFunction)ladder_state_configure, METH_VARARGS, NULL},
    {"finish", (PyCFunction)ladder_state_finish, METH_NOARGS, NULL},
    {"reset", (PyCFunction)ladder_state_reset, METH_NOARGS, NULL},
    {"process", (PyCFunction)ladder_state_process, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyType_Slot ladder_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, ladder_state_init},
    {Py_tp_methods, ladder_state_methods},
    {0, NULL},
};

static PyType_Spec ladder_state_spec = {
    .name = "_audiodsp.LadderState",
    .basicsize = sizeof(audiodsp_ladder_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = ladder_state_slots,
};

// audioverb.Tank's twelve lines, its predelay and its filters. All state, and
// a great deal of it: one allocation carved up by the DSP layer, exactly as in
// the MicroPython binding and in audioconvolve's.

typedef struct {
    PyObject_HEAD
    audiodsp_tank_config_t config;
    audiodsp_tank_state_t state;
    int16_t *lines;
} audiodsp_tank_object_t;

static int tank_state_status(audiodsp_tank_status_t status) {
    const char *message;
    switch (status) {
        case AUDIODSP_TANK_OK:
            return 0;
        case AUDIODSP_TANK_ERR_COUNT:
            message = "delays needs 12 line lengths; "
                      "taps needs 4 values per tap";
            break;
        case AUDIODSP_TANK_ERR_LENGTH:
            message = "every line needs at least 4 frames";
            break;
        case AUDIODSP_TANK_ERR_TOTAL:
            message = "the lines do not fit";
            break;
        case AUDIODSP_TANK_ERR_CHANNEL:
            message = "a tap channel is not 0 or 1";
            break;
        case AUDIODSP_TANK_ERR_LINE:
            message = "a tap line index is not 0..11";
            break;
        default:
            message = "a tap offset is past the end of its line";
            break;
    }
    PyErr_SetString(PyExc_ValueError, message);
    return -1;
}

static int tank_state_floats(PyObject *sequence, float *out, uint32_t limit,
    uint32_t *count) {
    PyObject *fast = PySequence_Fast(sequence, "expected a sequence of numbers");
    if (fast == NULL) return -1;
    const Py_ssize_t length = PySequence_Fast_GET_SIZE(fast);
    if ((uint32_t)length > limit) {
        Py_DECREF(fast);
        PyErr_SetString(PyExc_ValueError, "too many values");
        return -1;
    }
    for (Py_ssize_t index = 0; index < length; ++index) {
        const double value =
            PyFloat_AsDouble(PySequence_Fast_GET_ITEM(fast, index));
        if (value == -1.0 && PyErr_Occurred()) {
            Py_DECREF(fast);
            return -1;
        }
        out[index] = (float)value;
    }
    Py_DECREF(fast);
    *count = (uint32_t)length;
    return 0;
}

static int tank_state_init(audiodsp_tank_object_t *self, PyObject *args,
    PyObject *kwargs) {
    unsigned int sample_rate = 48000;
    unsigned int channel_count = 2;
    double max_predelay_ms = 200.0;
    PyObject *delays = NULL;
    PyObject *taps = NULL;
    static char *keywords[] = {"sample_rate", "max_predelay_ms",
                               "channel_count", "delays", "taps", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|IdIOO:TankState",
        keywords, &sample_rate, &max_predelay_ms, &channel_count, &delays,
        &taps)) return -1;
    if (sample_rate < 1) {
        PyErr_SetString(PyExc_ValueError, "sample_rate must be at least 1");
        return -1;
    }
    if (max_predelay_ms < 0.0) {
        PyErr_SetString(PyExc_ValueError,
            "max_predelay_ms must not be negative");
        return -1;
    }
    if (channel_count < 1 || channel_count > 2) {
        PyErr_SetString(PyExc_ValueError, "channel_count must be 1 or 2");
        return -1;
    }
    audiodsp_tank_config_t config;
    audiodsp_tank_config_init(&config, sample_rate, (float)max_predelay_ms);
    audiodsp_tank_set_channel_count(&config, channel_count);
    // The topology first: both tables size the allocation, so neither can be
    // changed once the lines exist.
    if (delays != NULL && delays != Py_None) {
        float values[AUDIODSP_TANK_LINES];
        uint32_t count = 0;
        if (tank_state_floats(delays, values, AUDIODSP_TANK_LINES,
            &count) < 0) return -1;
        uint32_t frames[AUDIODSP_TANK_LINES];
        for (uint32_t line = 0; line < count; ++line) {
            frames[line] = values[line] < 0.0f ? 0u : (uint32_t)values[line];
        }
        if (tank_state_status(
            audiodsp_tank_set_delays(&config, frames, count)) < 0) return -1;
    }
    if (taps != NULL && taps != Py_None) {
        float values[AUDIODSP_TANK_MAX_TAPS * 4u];
        uint32_t count = 0;
        if (tank_state_floats(taps, values, AUDIODSP_TANK_MAX_TAPS * 4u,
            &count) < 0) return -1;
        if (tank_state_status(
            audiodsp_tank_set_taps(&config, values, count)) < 0) return -1;
    }
    const uint32_t samples = audiodsp_tank_buffer_samples(&config);
    int16_t *lines = PyMem_Calloc((size_t)samples, sizeof(int16_t));
    if (lines == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    PyMem_Free(self->lines);
    self->lines = lines;
    self->config = config;
    audiodsp_tank_state_init(&self->state, &self->config, lines);
    return 0;
}

static void tank_state_dealloc(audiodsp_tank_object_t *self) {
    PyTypeObject *type = Py_TYPE(self);
    PyMem_Free(self->lines);
    self->lines = NULL;
    type->tp_free((PyObject *)self);
    Py_DECREF(type);
}

static PyObject *tank_state_configure(audiodsp_tank_object_t *self,
    PyObject *args) {
    int option;
    double value;
    if (!PyArg_ParseTuple(args, "id:configure", &option, &value)) return NULL;
    if (option < AUDIODSP_TANK_OPT_DECAY || option > AUDIODSP_TANK_OPT_MIX) {
        PyErr_SetString(PyExc_ValueError, "unknown tank option");
        return NULL;
    }
    audiodsp_tank_configure(&self->config, (audiodsp_tank_option_t)option,
        (float)value);
    Py_RETURN_NONE;
}

static PyObject *tank_state_finish(audiodsp_tank_object_t *self,
    PyObject *unused) {
    audiodsp_tank_config_finish(&self->config);
    Py_RETURN_NONE;
}

static PyObject *tank_state_reset(audiodsp_tank_object_t *self,
    PyObject *unused) {
    audiodsp_tank_reset(&self->state, &self->config);
    Py_RETURN_NONE;
}

static PyObject *tank_state_process(audiodsp_tank_object_t *self,
    PyObject *argument) {
    Py_buffer input = {0};
    if (PyObject_GetBuffer(argument, &input, PyBUF_SIMPLE) < 0) return NULL;
    const Py_ssize_t width = 2 * (Py_ssize_t)self->config.channel_count;
    if (input.len % width) {
        PyBuffer_Release(&input);
        PyErr_SetString(PyExc_ValueError,
            "input must be whole 16-bit frames for the configured channel count");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, input.len);
    if (result != NULL) {
        audiodsp_tank_process_s16(&self->config, &self->state,
            (int16_t *)PyBytes_AS_STRING(result), (const int16_t *)input.buf,
            (uint32_t)(input.len / width));
    }
    PyBuffer_Release(&input);
    return result;
}

static PyMethodDef tank_state_methods[] = {
    {"configure", (PyCFunction)tank_state_configure, METH_VARARGS, NULL},
    {"finish", (PyCFunction)tank_state_finish, METH_NOARGS, NULL},
    {"reset", (PyCFunction)tank_state_reset, METH_NOARGS, NULL},
    {"process", (PyCFunction)tank_state_process, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyType_Slot tank_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, tank_state_init},
    {Py_tp_dealloc, tank_state_dealloc},
    {Py_tp_methods, tank_state_methods},
    {0, NULL},
};

static PyType_Spec tank_state_spec = {
    .name = "_audiodsp.TankState",
    .basicsize = sizeof(audiodsp_tank_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = tank_state_slots,
};

// audiomodal.Bank's mode table, coefficients and recursion memory. One
// allocation per array, carved up the same way the MicroPython binding does
// it -- the DSP layer never allocates.
typedef struct {
    PyObject_HEAD
    audiodsp_modal_config_t config;
    audiodsp_modal_state_t state;
    audiodsp_modal_mode_t *modes;
    audiodsp_modal_coeff_t *coeffs;
    float *s1;
    float *s2;
} audiodsp_modal_object_t;

static int modal_state_init(audiodsp_modal_object_t *self, PyObject *args,
    PyObject *kwargs) {
    static char *keywords[] = {"sample_rate", "channel_count", "modes", NULL};
    unsigned int sample_rate = 48000;
    unsigned int channel_count = 2;
    unsigned int modes = 8;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|III:ModalState",
        keywords, &sample_rate, &channel_count, &modes)) {
        return -1;
    }
    if (modes < 1u || modes > AUDIODSP_MODAL_MAX_MODES) {
        PyErr_SetString(PyExc_ValueError, "modes out of range");
        return -1;
    }
    if (channel_count < 1u || channel_count > 2u) {
        PyErr_SetString(PyExc_ValueError, "channel_count must be 1 or 2");
        return -1;
    }
    PyMem_Free(self->modes);
    PyMem_Free(self->coeffs);
    PyMem_Free(self->s1);
    PyMem_Free(self->s2);
    self->modes = PyMem_Calloc(modes, sizeof(audiodsp_modal_mode_t));
    self->coeffs = PyMem_Calloc(modes, sizeof(audiodsp_modal_coeff_t));
    if (self->modes == NULL || self->coeffs == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    audiodsp_modal_config_init(&self->config, sample_rate, channel_count,
        modes, self->modes, self->coeffs);
    uint32_t words = audiodsp_modal_state_floats(&self->config);
    self->s1 = PyMem_Calloc(words, sizeof(float));
    self->s2 = PyMem_Calloc(words, sizeof(float));
    if (self->s1 == NULL || self->s2 == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    audiodsp_modal_state_init(&self->state, &self->config, self->s1, self->s2,
        words);
    return 0;
}

static void modal_state_dealloc(audiodsp_modal_object_t *self) {
    PyTypeObject *type = Py_TYPE(self);
    PyMem_Free(self->modes);
    PyMem_Free(self->coeffs);
    PyMem_Free(self->s1);
    PyMem_Free(self->s2);
    self->modes = NULL;
    self->coeffs = NULL;
    self->s1 = NULL;
    self->s2 = NULL;
    type->tp_free((PyObject *)self);
    Py_DECREF(type);
}

static PyObject *modal_state_configure(audiodsp_modal_object_t *self,
    PyObject *args) {
    int option;
    double value;
    if (!PyArg_ParseTuple(args, "id:configure", &option, &value)) return NULL;
    if (option < AUDIODSP_MODAL_OPT_MIX || option > AUDIODSP_MODAL_OPT_GAIN) {
        PyErr_SetString(PyExc_ValueError, "unknown modal option");
        return NULL;
    }
    audiodsp_modal_configure(&self->config, (audiodsp_modal_option_t)option,
        (float)value);
    Py_RETURN_NONE;
}

static PyObject *modal_state_set_mode(audiodsp_modal_object_t *self,
    PyObject *args) {
    unsigned int index;
    double frequency, decay, gain;
    if (!PyArg_ParseTuple(args, "Iddd:set_mode", &index, &frequency, &decay,
        &gain)) {
        return NULL;
    }
    if (index >= self->config.mode_count) {
        PyErr_SetString(PyExc_IndexError, "mode index out of range");
        return NULL;
    }
    audiodsp_modal_set_mode(&self->config, index, (float)frequency,
        (float)decay, (float)gain);
    Py_RETURN_NONE;
}

static PyObject *modal_state_finish(audiodsp_modal_object_t *self,
    PyObject *unused) {
    audiodsp_modal_config_finish(&self->config);
    Py_RETURN_NONE;
}

static PyObject *modal_state_reset(audiodsp_modal_object_t *self,
    PyObject *unused) {
    audiodsp_modal_reset(&self->state);
    Py_RETURN_NONE;
}

static PyObject *modal_state_silent(audiodsp_modal_object_t *self,
    PyObject *unused) {
    return PyBool_FromLong(audiodsp_modal_silent(&self->state) ? 1 : 0);
}

static PyObject *modal_state_process(audiodsp_modal_object_t *self,
    PyObject *argument) {
    Py_buffer input = {0};
    if (PyObject_GetBuffer(argument, &input, PyBUF_SIMPLE) < 0) return NULL;
    const Py_ssize_t width = 2 * (Py_ssize_t)self->config.channel_count;
    if (input.len % width) {
        PyBuffer_Release(&input);
        PyErr_SetString(PyExc_ValueError,
            "input must be whole 16-bit frames for the configured channel count");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, input.len);
    if (result != NULL) {
        audiodsp_modal_process_s16(&self->config, &self->state,
            (int16_t *)PyBytes_AS_STRING(result), (const int16_t *)input.buf,
            (uint32_t)(input.len / width));
    }
    PyBuffer_Release(&input);
    return result;
}

static PyMethodDef modal_state_methods[] = {
    {"configure", (PyCFunction)modal_state_configure, METH_VARARGS, NULL},
    {"set_mode", (PyCFunction)modal_state_set_mode, METH_VARARGS, NULL},
    {"finish", (PyCFunction)modal_state_finish, METH_NOARGS, NULL},
    {"reset", (PyCFunction)modal_state_reset, METH_NOARGS, NULL},
    {"silent", (PyCFunction)modal_state_silent, METH_NOARGS, NULL},
    {"process", (PyCFunction)modal_state_process, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyType_Slot modal_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, modal_state_init},
    {Py_tp_dealloc, modal_state_dealloc},
    {Py_tp_methods, modal_state_methods},
    {0, NULL},
};

static PyType_Spec modal_state_spec = {
    .name = "_audiodsp.ModalState",
    .basicsize = sizeof(audiodsp_modal_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = modal_state_slots,
};

// audioconvolve.Convolver's transform tables, frequency-delay line and stored
// impulse. All state, and a great deal of it -- one allocation carved up by
// the DSP layer, exactly as in the MicroPython binding.

typedef struct {
    PyObject_HEAD
    audiodsp_convolve_config_t config;
    audiodsp_convolve_state_t state;
    float *storage;
} audiodsp_convolver_object_t;

static int convolver_state_init(audiodsp_convolver_object_t *self,
    PyObject *args, PyObject *kwargs) {
    unsigned int sample_rate = 48000;
    unsigned int partitions = 1;
    unsigned int ir_channels = 1;
    unsigned int channel_count = 2;
    static char *keywords[] = {"sample_rate", "partitions", "ir_channels",
                               "channel_count", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|IIII:ConvolverState",
        keywords, &sample_rate, &partitions, &ir_channels,
        &channel_count)) return -1;
    if (sample_rate < 1) {
        PyErr_SetString(PyExc_ValueError, "sample_rate must be at least 1");
        return -1;
    }
    if (partitions < 1 || partitions > AUDIODSP_CONVOLVE_MAX_PARTITIONS) {
        PyErr_SetString(PyExc_ValueError, "partitions out of range");
        return -1;
    }
    if (ir_channels < 1 || ir_channels > 2) {
        PyErr_SetString(PyExc_ValueError, "ir_channels must be 1 or 2");
        return -1;
    }
    if (channel_count < 1 || channel_count > 2) {
        PyErr_SetString(PyExc_ValueError, "channel_count must be 1 or 2");
        return -1;
    }
    audiodsp_convolve_config_init(&self->config, sample_rate, partitions,
        ir_channels);
    audiodsp_convolve_set_channel_count(&self->config, channel_count);
    float *storage = PyMem_Calloc(
        audiodsp_convolve_float_count(&self->config), sizeof(float));
    if (storage == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    PyMem_Free(self->storage);
    self->storage = storage;
    audiodsp_convolve_state_init(&self->state, &self->config, storage);
    return 0;
}

static void convolver_state_dealloc(audiodsp_convolver_object_t *self) {
    PyTypeObject *type = Py_TYPE(self);
    PyMem_Free(self->storage);
    self->storage = NULL;
    type->tp_free((PyObject *)self);
    Py_DECREF(type);
}

static PyObject *convolver_state_configure(audiodsp_convolver_object_t *self,
    PyObject *args) {
    int option;
    double value;
    if (!PyArg_ParseTuple(args, "id:configure", &option, &value)) return NULL;
    if (option != AUDIODSP_CONVOLVE_OPT_MIX) {
        PyErr_SetString(PyExc_ValueError, "unknown convolver option");
        return NULL;
    }
    audiodsp_convolve_configure(&self->config,
        (audiodsp_convolve_option_t)option, (float)value);
    Py_RETURN_NONE;
}

static PyObject *convolver_state_load(audiodsp_convolver_object_t *self,
    PyObject *args) {
    Py_buffer taps = {0};
    unsigned int channels = 1;
    double gain = 1.0;
    if (!PyArg_ParseTuple(args, "y*Id:load", &taps, &channels, &gain)) {
        return NULL;
    }
    if (channels < 1 || channels > 2) {
        PyBuffer_Release(&taps);
        PyErr_SetString(PyExc_ValueError, "channels must be 1 or 2");
        return NULL;
    }
    if (taps.len % (2 * (Py_ssize_t)channels)) {
        PyBuffer_Release(&taps);
        PyErr_SetString(PyExc_ValueError,
            "impulse must be whole int16 frames");
        return NULL;
    }
    uint32_t frames = (uint32_t)(taps.len / (2 * (Py_ssize_t)channels));
    if (frames > self->config.partitions * AUDIODSP_CONVOLVE_FRAMES) {
        PyBuffer_Release(&taps);
        PyErr_SetString(PyExc_ValueError, "impulse is longer than max_taps");
        return NULL;
    }
    audiodsp_convolve_load_s16(&self->state, &self->config,
        (const int16_t *)taps.buf, frames, channels, (float)gain);
    PyBuffer_Release(&taps);
    Py_RETURN_NONE;
}

static PyObject *convolver_state_synthesize(audiodsp_convolver_object_t *self,
    PyObject *args) {
    double decay, damping, predelay, diffusion;
    unsigned int seed;
    if (!PyArg_ParseTuple(args, "ddddI:synthesize", &decay, &damping,
        &predelay, &diffusion, &seed)) return NULL;
    audiodsp_convolve_synthesize(&self->state, &self->config, (float)decay,
        (float)damping, (float)predelay, (float)diffusion, seed);
    Py_RETURN_NONE;
}

static PyObject *convolver_state_reset(audiodsp_convolver_object_t *self,
    PyObject *unused) {
    audiodsp_convolve_reset(&self->state, &self->config);
    Py_RETURN_NONE;
}

static PyObject *convolver_state_taps(audiodsp_convolver_object_t *self,
    PyObject *unused) {
    return PyLong_FromUnsignedLong(
        (unsigned long)self->state.loaded * AUDIODSP_CONVOLVE_FRAMES);
}

static PyObject *convolver_state_process(audiodsp_convolver_object_t *self,
    PyObject *argument) {
    Py_buffer input = {0};
    if (PyObject_GetBuffer(argument, &input, PyBUF_SIMPLE) < 0) return NULL;
    const Py_ssize_t width = 2 * (Py_ssize_t)self->config.channel_count;
    if (input.len % width) {
        PyBuffer_Release(&input);
        PyErr_SetString(PyExc_ValueError,
            "input must be whole 16-bit frames for the configured channel count");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, input.len);
    if (result != NULL) {
        audiodsp_convolve_process_s16(&self->config, &self->state,
            (int16_t *)PyBytes_AS_STRING(result), (const int16_t *)input.buf,
            (uint32_t)(input.len / width));
    }
    PyBuffer_Release(&input);
    return result;
}

static PyMethodDef convolver_state_methods[] = {
    {"configure", (PyCFunction)convolver_state_configure, METH_VARARGS, NULL},
    {"load", (PyCFunction)convolver_state_load, METH_VARARGS, NULL},
    {"synthesize", (PyCFunction)convolver_state_synthesize, METH_VARARGS, NULL},
    {"reset", (PyCFunction)convolver_state_reset, METH_NOARGS, NULL},
    {"taps", (PyCFunction)convolver_state_taps, METH_NOARGS, NULL},
    {"process", (PyCFunction)convolver_state_process, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyType_Slot convolver_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, convolver_state_init},
    {Py_tp_dealloc, convolver_state_dealloc},
    {Py_tp_methods, convolver_state_methods},
    {0, NULL},
};

static PyType_Spec convolver_state_spec = {
    .name = "_audiodsp.ConvolverState",
    .basicsize = sizeof(audiodsp_convolver_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = convolver_state_slots,
};

static PyObject *audiodsp_multiply_s16(PyObject *module, PyObject *args) {
    Py_buffer signal = {0};
    Py_buffer modulator = {0};
    double mix = 1.0;
    unsigned int channel_count = 2;
    if (!PyArg_ParseTuple(args, "y*y*d|I:multiply_s16", &signal, &modulator,
        &mix, &channel_count)) {
        return NULL;
    }
    if (channel_count < 1 || channel_count > 2) {
        PyBuffer_Release(&signal);
        PyBuffer_Release(&modulator);
        PyErr_SetString(PyExc_ValueError, "channel_count must be 1 or 2");
        return NULL;
    }
    const Py_ssize_t width = 2 * (Py_ssize_t)channel_count;
    if (signal.len != modulator.len || signal.len % width) {
        PyBuffer_Release(&signal);
        PyBuffer_Release(&modulator);
        PyErr_SetString(PyExc_ValueError,
            "buffers must be the same whole number of configured frames");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, signal.len);
    if (result != NULL) {
        audiodsp_multiply_config_t config;
        audiodsp_multiply_config_init(&config);
        audiodsp_multiply_set_channel_count(&config, channel_count);
        audiodsp_multiply_set_mix(&config, (float)mix);
        audiodsp_multiply_process_s16(&config,
            (int16_t *)PyBytes_AS_STRING(result), (const int16_t *)signal.buf,
            (const int16_t *)modulator.buf, (uint32_t)(signal.len / width));
    }
    PyBuffer_Release(&signal);
    PyBuffer_Release(&modulator);
    return result;
}

static PyObject *audiodsp_midside_s16(PyObject *module, PyObject *args) {
    Py_buffer signal = {0};
    double width = 1.0;
    unsigned int channel_count = 2;
    if (!PyArg_ParseTuple(args, "y*d|I:midside_s16", &signal, &width,
        &channel_count)) {
        return NULL;
    }
    if (channel_count < 1 || channel_count > 2) {
        PyBuffer_Release(&signal);
        PyErr_SetString(PyExc_ValueError, "channel_count must be 1 or 2");
        return NULL;
    }
    const Py_ssize_t frame = 2 * (Py_ssize_t)channel_count;
    if (signal.len % frame) {
        PyBuffer_Release(&signal);
        PyErr_SetString(PyExc_ValueError,
            "buffer must be a whole number of configured frames");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, signal.len);
    if (result != NULL) {
        audiodsp_midside_config_t config;
        audiodsp_midside_config_init(&config);
        audiodsp_midside_set_channel_count(&config, channel_count);
        audiodsp_midside_set_width(&config, (float)width);
        audiodsp_midside_process_s16(&config,
            (int16_t *)PyBytes_AS_STRING(result), (const int16_t *)signal.buf,
            (uint32_t)(signal.len / frame));
    }
    PyBuffer_Release(&signal);
    return result;
}

static PyObject *audiodsp_py_remix_s16(PyObject *module, PyObject *args) {
    Py_buffer src = {0};
    PyObject *dest_obj = Py_None;
    int src_ch = 0;
    int dst_ch = 0;
    if (!PyArg_ParseTuple(args, "y*ii|O:remix_s16", &src, &src_ch, &dst_ch,
        &dest_obj)) {
        return NULL;
    }
    if (src_ch < 1 || src_ch > 2 || dst_ch < 1 || dst_ch > 2) {
        PyBuffer_Release(&src);
        PyErr_SetString(PyExc_ValueError, "channel_count must be 1 or 2");
        return NULL;
    }
    const Py_ssize_t src_frame = 2 * (Py_ssize_t)src_ch;
    if (src_frame == 0 || src.len % src_frame) {
        PyBuffer_Release(&src);
        PyErr_SetString(PyExc_ValueError,
            "source must be a whole number of frames");
        return NULL;
    }
    const Py_ssize_t frames = src.len / src_frame;
    const Py_ssize_t dst_len = frames * 2 * (Py_ssize_t)dst_ch;
    if (dest_obj == Py_None) {
        PyObject *result = PyBytes_FromStringAndSize(NULL, dst_len);
        if (result == NULL) {
            PyBuffer_Release(&src);
            return NULL;
        }
        audiodsp_remix_s16((int16_t *)PyBytes_AS_STRING(result),
            (const int16_t *)src.buf, (uint32_t)frames,
            (uint32_t)src_ch, (uint32_t)dst_ch);
        PyBuffer_Release(&src);
        return result;
    }
    Py_buffer dst = {0};
    if (PyObject_GetBuffer(dest_obj, &dst,
        PyBUF_WRITABLE | PyBUF_C_CONTIGUOUS) < 0) {
        PyBuffer_Release(&src);
        return NULL;
    }
    if (dst.len < dst_len) {
        PyBuffer_Release(&src);
        PyBuffer_Release(&dst);
        PyErr_SetString(PyExc_ValueError, "dest is too small");
        return NULL;
    }
    audiodsp_remix_s16((int16_t *)dst.buf, (const int16_t *)src.buf,
        (uint32_t)frames, (uint32_t)src_ch, (uint32_t)dst_ch);
    PyBuffer_Release(&src);
    PyBuffer_Release(&dst);
    Py_INCREF(dest_obj);
    return dest_obj;
}

// audiomath.SubOctave's divider. Unlike multiply_s16() above -- which is a
// plain function, because the multiply carries no state at all -- the divider
// is nothing but state: the count has to survive from one block to the next,
// and a chain that restarted it every block would flip polarity at the block
// rate rather than at half the note's frequency. So it is a type, the way
// FeedbackDelayState is, though a very much smaller one: four fields, no
// allocation.

typedef struct {
    PyObject_HEAD
    audiodsp_suboctave_config_t config;
    audiodsp_suboctave_state_t state;
} audiodsp_suboctave_object_t;

static int suboctave_state_init(audiodsp_suboctave_object_t *self,
    PyObject *args, PyObject *kwargs) {
    unsigned int sample_rate = 48000;
    unsigned int channel_count = 2;
    static char *keywords[] = {"sample_rate", "channel_count", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|II:SubOctaveState",
        keywords, &sample_rate, &channel_count)) return -1;
    if (sample_rate < 1) {
        PyErr_SetString(PyExc_ValueError, "sample_rate must be at least 1");
        return -1;
    }
    if (channel_count < 1 || channel_count > 2) {
        PyErr_SetString(PyExc_ValueError, "channel_count must be 1 or 2");
        return -1;
    }
    audiodsp_suboctave_config_init(&self->config, sample_rate);
    audiodsp_suboctave_set_channel_count(&self->config, channel_count);
    audiodsp_suboctave_state_init(&self->state);
    return 0;
}

static PyObject *suboctave_state_configure(audiodsp_suboctave_object_t *self,
    PyObject *args) {
    int option;
    double value;
    if (!PyArg_ParseTuple(args, "id:configure", &option, &value)) return NULL;
    if (option < AUDIODSP_SUBOCTAVE_OPT_ORDER ||
        option > AUDIODSP_SUBOCTAVE_OPT_HOLD_MS) {
        PyErr_SetString(PyExc_ValueError, "unknown sub-octave option");
        return NULL;
    }
    audiodsp_suboctave_configure(&self->config,
        (audiodsp_suboctave_option_t)option, (float)value);
    Py_RETURN_NONE;
}

static PyObject *suboctave_state_finish(audiodsp_suboctave_object_t *self,
    PyObject *unused) {
    audiodsp_suboctave_config_finish(&self->config);
    Py_RETURN_NONE;
}

static PyObject *suboctave_state_reset(audiodsp_suboctave_object_t *self,
    PyObject *unused) {
    audiodsp_suboctave_reset(&self->state);
    Py_RETURN_NONE;
}

static PyObject *suboctave_state_process(audiodsp_suboctave_object_t *self,
    PyObject *argument) {
    Py_buffer input = {0};
    if (PyObject_GetBuffer(argument, &input, PyBUF_SIMPLE) < 0) return NULL;
    const Py_ssize_t width = 2 * (Py_ssize_t)self->config.channel_count;
    if (input.len % width) {
        PyBuffer_Release(&input);
        PyErr_SetString(PyExc_ValueError,
            "input must be whole 16-bit frames for the configured channel count");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, input.len);
    if (result != NULL) {
        audiodsp_suboctave_process_s16(&self->config, &self->state,
            (int16_t *)PyBytes_AS_STRING(result), (const int16_t *)input.buf,
            (uint32_t)(input.len / width));
    }
    PyBuffer_Release(&input);
    return result;
}

static PyMethodDef suboctave_state_methods[] = {
    {"configure", (PyCFunction)suboctave_state_configure, METH_VARARGS, NULL},
    {"finish", (PyCFunction)suboctave_state_finish, METH_NOARGS, NULL},
    {"reset", (PyCFunction)suboctave_state_reset, METH_NOARGS, NULL},
    {"process", (PyCFunction)suboctave_state_process, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static PyType_Slot suboctave_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, suboctave_state_init},
    {Py_tp_methods, suboctave_state_methods},
    {0, NULL},
};

static PyType_Spec suboctave_state_spec = {
    .name = "_audiodsp.SubOctaveState",
    .basicsize = sizeof(audiodsp_suboctave_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = suboctave_state_slots,
};

static PyObject *audiodsp_mix_s16(PyObject *module, PyObject *args) {
    Py_buffer left = {0};
    Py_buffer right = {0};
    if (!PyArg_ParseTuple(args, "y*y*:mix_s16", &left, &right)) {
        return NULL;
    }
    if (left.len != right.len || left.len % 2) {
        PyBuffer_Release(&left);
        PyBuffer_Release(&right);
        PyErr_SetString(PyExc_ValueError, "buffers must have the same even byte length");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, left.len);
    if (result == NULL) {
        PyBuffer_Release(&left);
        PyBuffer_Release(&right);
        return NULL;
    }
    const int16_t *a = (const int16_t *)left.buf;
    const int16_t *b = (const int16_t *)right.buf;
    int16_t *out = (int16_t *)PyBytes_AS_STRING(result);
    for (Py_ssize_t i = 0; i < left.len / 2; i++) {
        int32_t value = (int32_t)a[i] + (int32_t)b[i];
        if (value > INT16_MAX) value = INT16_MAX;
        if (value < INT16_MIN) value = INT16_MIN;
        out[i] = (int16_t)value;
    }
    PyBuffer_Release(&left);
    PyBuffer_Release(&right);
    return result;
}

static PyObject *audiodsp_oscillator_i32(PyObject *module, PyObject *args) {
    PyObject *waveform_object;
    unsigned int accumulator, dds_rate, waveform_start, waveform_end;
    int loudness_left, loudness_right;
    unsigned int channel_count, duration;
    if (!PyArg_ParseTuple(args, "OIIIIiiII:oscillator_i32",
        &waveform_object, &accumulator, &dds_rate, &waveform_start,
        &waveform_end, &loudness_left, &loudness_right, &channel_count,
        &duration)) return NULL;
    if ((channel_count != 1 && channel_count != 2) || duration > UINT16_MAX ||
        loudness_left < INT16_MIN || loudness_left > INT16_MAX ||
        loudness_right < INT16_MIN || loudness_right > INT16_MAX) {
        PyErr_SetString(PyExc_ValueError, "invalid oscillator parameters");
        return NULL;
    }

    Py_buffer waveform = {0};
    if (PyObject_GetBuffer(waveform_object, &waveform,
        PyBUF_FORMAT | PyBUF_C_CONTIGUOUS) < 0) return NULL;
    const char *format = waveform.format == NULL ? "" : waveform.format;
    uint32_t waveform_length = (uint32_t)(waveform.len / sizeof(int16_t));
    if (strcmp(format, "h") != 0 || waveform.len % sizeof(int16_t) ||
        waveform_start >= waveform_end || waveform_end > waveform_length) {
        PyBuffer_Release(&waveform);
        PyErr_SetString(PyExc_ValueError,
            "waveform must be a native signed 16-bit buffer with a valid loop");
        return NULL;
    }

    int32_t *voice = PyMem_Calloc(duration, sizeof(int32_t));
    int32_t *output = PyMem_Calloc((size_t)duration * channel_count,
        sizeof(int32_t));
    if (voice == NULL || output == NULL) {
        PyMem_Free(voice);
        PyMem_Free(output);
        PyBuffer_Release(&waveform);
        return PyErr_NoMemory();
    }
    uint32_t next_accumulator = accumulator;
    bool rendered = audiodsp_oscillator_fill(voice,
        (const int16_t *)waveform.buf, waveform_start, waveform_end, dds_rate,
        &next_accumulator, (uint16_t)duration, 16);
    if (rendered) {
        int16_t loudness[2] = {(int16_t)loudness_left, (int16_t)loudness_right};
        // No previous block to carry an active loudness from, so active is
        // pending and the zero-crossing gate short-circuits.
        int16_t active[2] = {loudness[0], loudness[1]};
        audiodsp_sum_with_loudness(output, voice, active, loudness, duration,
            (uint8_t)channel_count);
    }
    PyObject *data = PyBytes_FromStringAndSize((const char *)output,
        (Py_ssize_t)duration * channel_count * sizeof(int32_t));
    PyMem_Free(voice);
    PyMem_Free(output);
    PyBuffer_Release(&waveform);
    if (data == NULL) return NULL;
    return Py_BuildValue("(NI)", data, next_accumulator);
}

static PyObject *audiodsp_oscillator_raw_i32(PyObject *module, PyObject *args) {
    PyObject *waveform_object;
    unsigned int accumulator, dds_rate, waveform_start, waveform_end, duration;
    if (!PyArg_ParseTuple(args, "OIIIII:oscillator_raw_i32",
        &waveform_object, &accumulator, &dds_rate, &waveform_start,
        &waveform_end, &duration)) return NULL;
    if (duration > UINT16_MAX) {
        PyErr_SetString(PyExc_ValueError, "invalid oscillator duration");
        return NULL;
    }
    Py_buffer waveform = {0};
    if (PyObject_GetBuffer(waveform_object, &waveform,
        PyBUF_FORMAT | PyBUF_C_CONTIGUOUS) < 0) return NULL;
    const char *format = waveform.format == NULL ? "" : waveform.format;
    uint32_t waveform_length = (uint32_t)(waveform.len / sizeof(int16_t));
    if (strcmp(format, "h") != 0 || waveform.len % sizeof(int16_t) ||
        waveform_start >= waveform_end || waveform_end > waveform_length) {
        PyBuffer_Release(&waveform);
        PyErr_SetString(PyExc_ValueError, "invalid oscillator waveform");
        return NULL;
    }
    PyObject *data = PyBytes_FromStringAndSize(NULL,
        (Py_ssize_t)duration * sizeof(int32_t));
    if (data == NULL) {
        PyBuffer_Release(&waveform);
        return NULL;
    }
    memset(PyBytes_AS_STRING(data), 0, PyBytes_GET_SIZE(data));
    uint32_t next_accumulator = accumulator;
    (void)audiodsp_oscillator_fill((int32_t *)PyBytes_AS_STRING(data),
        (const int16_t *)waveform.buf, waveform_start, waveform_end, dds_rate,
        &next_accumulator, (uint16_t)duration, 16);
    PyBuffer_Release(&waveform);
    return Py_BuildValue("(NI)", data, next_accumulator);
}

static PyObject *audiodsp_ring_multiply_i32(PyObject *module, PyObject *args) {
    Py_buffer voice = {0};
    PyObject *waveform_object;
    unsigned int accumulator, dds_rate, waveform_start, waveform_end;
    if (!PyArg_ParseTuple(args, "y*OIIII:ring_multiply_i32", &voice,
        &waveform_object, &accumulator, &dds_rate, &waveform_start,
        &waveform_end)) return NULL;
    if (voice.len % sizeof(int32_t)) {
        PyBuffer_Release(&voice);
        PyErr_SetString(PyExc_ValueError, "invalid ring voice buffer");
        return NULL;
    }
    Py_buffer waveform = {0};
    if (PyObject_GetBuffer(waveform_object, &waveform,
        PyBUF_FORMAT | PyBUF_C_CONTIGUOUS) < 0) {
        PyBuffer_Release(&voice);
        return NULL;
    }
    const char *format = waveform.format == NULL ? "" : waveform.format;
    uint32_t waveform_length = (uint32_t)(waveform.len / sizeof(int16_t));
    if (strcmp(format, "h") != 0 || waveform.len % sizeof(int16_t) ||
        waveform_start >= waveform_end || waveform_end > waveform_length) {
        PyBuffer_Release(&waveform);
        PyBuffer_Release(&voice);
        PyErr_SetString(PyExc_ValueError, "invalid ring waveform");
        return NULL;
    }
    Py_ssize_t duration = voice.len / sizeof(int32_t);
    PyObject *data = PyBytes_FromStringAndSize(NULL, voice.len);
    if (data == NULL) {
        PyBuffer_Release(&waveform);
        PyBuffer_Release(&voice);
        return NULL;
    }
    memcpy(PyBytes_AS_STRING(data), voice.buf, (size_t)voice.len);

    // Mirrors the ring stage of the MicroPython usermod's
    // synth_note_into_buffer() exactly, including two details that are easy
    // to lose: the accumulator advances BEFORE the sample is read, and the
    // product is narrowed to int16 before it goes back into the int32 voice
    // buffer. Dropping either makes the ring sound close but not identical.
    const int16_t *ring = (const int16_t *)waveform.buf;
    int32_t *out = (int32_t *)PyBytes_AS_STRING(data);
    uint32_t offset = waveform_start << 16;
    uint32_t lim = waveform_end << 16;
    uint32_t span = lim - offset;
    uint32_t accum = accumulator;
    if (accum >= lim) {
        accum = offset + (accum - offset) % span;
    }
    for (Py_ssize_t i = 0; i < duration; i++) {
        accum += dds_rate;
        if (accum >= lim) {
            accum -= span;
        }
        // The usermod declares this index int16_t; a wider type is used here
        // because it is identical for every table below 32768 samples and
        // avoids signed overflow above that.
        uint32_t index = accum >> 16;
        int16_t narrowed = (int16_t)((ring[index] * out[i]) / 32768);
        out[i] = narrowed;
    }
    PyBuffer_Release(&waveform);
    PyBuffer_Release(&voice);
    return Py_BuildValue("(NI)", data, accum);
}

static PyObject *audiodsp_apply_loudness_i32(PyObject *module, PyObject *args) {
    Py_buffer voice = {0};
    int left, right;
    unsigned int channels;
    // The loudness this voice was ACTUALLY rendered at when the previous block
    // ended. Omitted means "no previous block", so it starts at pending and
    // the zero-crossing gate never fires. See audiodsp_assign_loudness.
    int active_left = INT_MIN, active_right = INT_MIN;
    if (!PyArg_ParseTuple(args, "y*iiI|ii:apply_loudness_i32", &voice,
        &left, &right, &channels, &active_left, &active_right)) return NULL;
    if (active_left == INT_MIN) active_left = left;
    if (active_right == INT_MIN) active_right = right;
    if (voice.len % sizeof(int32_t) || (channels != 1 && channels != 2) ||
        left < INT16_MIN || left > INT16_MAX ||
        right < INT16_MIN || right > INT16_MAX ||
        active_left < INT16_MIN || active_left > INT16_MAX ||
        active_right < INT16_MIN || active_right > INT16_MAX) {
        PyBuffer_Release(&voice);
        PyErr_SetString(PyExc_ValueError, "invalid loudness parameters");
        return NULL;
    }
    Py_ssize_t duration = voice.len / sizeof(int32_t);
    PyObject *result = PyBytes_FromStringAndSize(NULL,
        duration * channels * sizeof(int32_t));
    if (result == NULL) {
        PyBuffer_Release(&voice);
        return NULL;
    }
    memset(PyBytes_AS_STRING(result), 0, PyBytes_GET_SIZE(result));
    int16_t loudness[2] = {(int16_t)left, (int16_t)right};
    int16_t active[2] = {(int16_t)active_left, (int16_t)active_right};
    audiodsp_sum_with_loudness((int32_t *)PyBytes_AS_STRING(result),
        (const int32_t *)voice.buf, active, loudness, duration,
        (uint8_t)channels);
    PyBuffer_Release(&voice);
    return result;
}

static PyObject *audiodsp_mixdown_i32(PyObject *module, PyObject *args) {
    Py_buffer input = {0};
    unsigned int max_polyphony = 64;
    if (!PyArg_ParseTuple(args, "y*|I:mixdown_i32", &input,
        &max_polyphony)) return NULL;
    if (input.len % sizeof(int32_t) || max_polyphony < 1) {
        PyBuffer_Release(&input);
        PyErr_SetString(PyExc_ValueError,
            "input must contain native signed 32-bit samples");
        return NULL;
    }
    Py_ssize_t count = input.len / sizeof(int32_t);
    PyObject *result = PyBytes_FromStringAndSize(NULL,
        count * sizeof(int16_t));
    if (result == NULL) {
        PyBuffer_Release(&input);
        return NULL;
    }
    int32_t scale = 0x0fffffff / (32768 * (int32_t)max_polyphony - 28000);
    const int32_t *source = input.buf;
    int16_t *destination = (int16_t *)PyBytes_AS_STRING(result);
    for (Py_ssize_t i = 0; i < count; i++) {
        destination[i] = audiodsp_mix_down_sample(source[i], scale,
            -28000, 28000);
    }
    PyBuffer_Release(&input);
    return result;
}

static PyObject *audiodsp_pitch_bend_value(PyObject *module, PyObject *args) {
    unsigned int frequency_scaled;
    int bend_value;
    if (!PyArg_ParseTuple(args, "Ii:pitch_bend", &frequency_scaled,
        &bend_value)) return NULL;
    return PyLong_FromUnsignedLong(audiodsp_pitch_bend(frequency_scaled,
        bend_value));
}

static PyObject *audiodsp_distortion_s16(PyObject *module, PyObject *args) {
    Py_buffer input = {0};
    double drive, pre_gain, post_gain, mix;
    int mode, soft_clip;
    if (!PyArg_ParseTuple(args, "y*dddipd:distortion_s16", &input,
        &drive, &pre_gain, &post_gain, &mode, &soft_clip, &mix)) return NULL;
    if (input.len % sizeof(int16_t) || mode < AUDIODSP_DISTORTION_CLIP ||
        mode > AUDIODSP_DISTORTION_WAVESHAPE) {
        PyBuffer_Release(&input);
        PyErr_SetString(PyExc_ValueError, "invalid distortion parameters");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, input.len);
    if (result == NULL) {
        PyBuffer_Release(&input);
        return NULL;
    }
    audiodsp_distortion_process_s16(
        (int16_t *)PyBytes_AS_STRING(result), (const int16_t *)input.buf,
        input.len / sizeof(int16_t), drive, pre_gain, post_gain,
        (audiodsp_distortion_mode_t)mode, soft_clip, mix);
    PyBuffer_Release(&input);
    return result;
}

static PyObject *audiodsp_echo_s16(PyObject *module, PyObject *args) {
    Py_buffer input = {0}, delay = {0};
    unsigned int left, right, delay_samples, maximum_samples, rate, channels;
    double decay, mix;
    int frequency_shift;
    if (!PyArg_ParseTuple(args, "y*w*IIIIIddpI:echo_s16", &input, &delay,
        &left, &right, &delay_samples, &maximum_samples, &rate, &decay,
        &mix, &frequency_shift, &channels)) return NULL;
    if (input.len % sizeof(int16_t) || channels < 1 || channels > 2 ||
        delay_samples < 1 || delay_samples > maximum_samples ||
        delay.len < (Py_ssize_t)((size_t)maximum_samples * channels * sizeof(int16_t))) {
        PyBuffer_Release(&input);
        PyBuffer_Release(&delay);
        PyErr_SetString(PyExc_ValueError, "invalid echo parameters");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, input.len);
    if (result == NULL) {
        PyBuffer_Release(&input);
        PyBuffer_Release(&delay);
        return NULL;
    }
    audiodsp_echo_positions_t positions = {left, right};
    audiodsp_echo_process_s16((int16_t *)PyBytes_AS_STRING(result),
        (const int16_t *)input.buf, input.len / sizeof(int16_t), delay.buf,
        delay_samples, maximum_samples, rate, decay, mix, frequency_shift,
        (uint8_t)channels, &positions);
    PyBuffer_Release(&input);
    PyBuffer_Release(&delay);
    return Py_BuildValue("(NII)", result, positions.left_position,
        positions.right_position);
}

static PyObject *audiodsp_phaser_s16(PyObject *module, PyObject *args) {
    Py_buffer input = {0}, feedback_words = {0}, allpass_words = {0};
    unsigned int channels, stages;
    double frequency, nyquist, feedback, mix;
    if (!PyArg_ParseTuple(args, "y*w*w*IIdddd:phaser_s16", &input,
        &feedback_words, &allpass_words, &channels, &stages, &frequency,
        &nyquist, &feedback, &mix)) return NULL;
    if (input.len % sizeof(int16_t) || channels < 1 || channels > 2 ||
        stages < 1 || stages > 255 || nyquist <= 0 ||
        feedback_words.len < (Py_ssize_t)(channels * sizeof(int16_t)) ||
        allpass_words.len <
            (Py_ssize_t)((size_t)channels * stages * sizeof(int16_t))) {
        PyBuffer_Release(&input);
        PyBuffer_Release(&feedback_words);
        PyBuffer_Release(&allpass_words);
        PyErr_SetString(PyExc_ValueError, "invalid phaser parameters");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize((const char *)input.buf,
        input.len);
    PyBuffer_Release(&input);
    if (result == NULL) {
        PyBuffer_Release(&feedback_words);
        PyBuffer_Release(&allpass_words);
        return NULL;
    }
    audiodsp_phaser_process_s16((int16_t *)PyBytes_AS_STRING(result),
        PyBytes_GET_SIZE(result) / sizeof(int16_t), feedback_words.buf,
        allpass_words.buf, (uint8_t)channels, (uint8_t)stages, frequency,
        nyquist, feedback, mix);
    PyBuffer_Release(&feedback_words);
    PyBuffer_Release(&allpass_words);
    return result;
}

static PyObject *audiodsp_chorus_s16(PyObject *module, PyObject *args) {
    Py_buffer input = {0}, delay = {0};
    unsigned int position, delay_samples, maximum_samples;
    int voices;
    double mix;
    if (!PyArg_ParseTuple(args, "y*w*IIIid:chorus_s16", &input, &delay,
        &position, &delay_samples, &maximum_samples, &voices, &mix)) return NULL;
    if (input.len % sizeof(int16_t) || voices < 1 || delay_samples < 1 ||
        delay_samples > maximum_samples || delay.len <
            (Py_ssize_t)((size_t)maximum_samples * sizeof(int16_t))) {
        PyBuffer_Release(&input); PyBuffer_Release(&delay);
        PyErr_SetString(PyExc_ValueError, "invalid chorus parameters");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, input.len);
    if (result == NULL) {
        PyBuffer_Release(&input); PyBuffer_Release(&delay); return NULL;
    }
    position = audiodsp_chorus_process_s16(
        (int16_t *)PyBytes_AS_STRING(result), input.buf,
        input.len / sizeof(int16_t), delay.buf, position, delay_samples,
        maximum_samples, voices, mix);
    PyBuffer_Release(&input); PyBuffer_Release(&delay);
    return Py_BuildValue("(NI)", result, position);
}

static PyObject *audiodsp_multitap_s16(PyObject *module, PyObject *args) {
    Py_buffer input = {0}, delay = {0}, offsets = {0}, levels = {0};
    unsigned int position, delay_samples, channels;
    double decay, mix;
    if (!PyArg_ParseTuple(args, "y*w*IIIw*w*dd:multitap_s16", &input,
        &delay, &position, &delay_samples, &channels, &offsets, &levels,
        &decay, &mix)) return NULL;
    size_t tap_count = (size_t)offsets.len / sizeof(uint32_t);
    if (input.len % sizeof(int16_t) || channels < 1 || channels > 2 ||
        delay_samples < 1 || offsets.len % sizeof(uint32_t) ||
        levels.len != (Py_ssize_t)(tap_count * sizeof(double)) ||
        delay.len < (Py_ssize_t)((size_t)delay_samples * channels *
            sizeof(int16_t))) {
        PyBuffer_Release(&input); PyBuffer_Release(&delay);
        PyBuffer_Release(&offsets); PyBuffer_Release(&levels);
        PyErr_SetString(PyExc_ValueError, "invalid multi-tap parameters");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, input.len);
    if (result == NULL) {
        PyBuffer_Release(&input); PyBuffer_Release(&delay);
        PyBuffer_Release(&offsets); PyBuffer_Release(&levels); return NULL;
    }
    position = audiodsp_multitap_process_s16(
        (int16_t *)PyBytes_AS_STRING(result), input.buf,
        input.len / sizeof(int16_t), delay.buf, position, delay_samples,
        (uint8_t)channels, offsets.buf, levels.buf, tap_count, decay, mix);
    PyBuffer_Release(&input); PyBuffer_Release(&delay);
    PyBuffer_Release(&offsets); PyBuffer_Release(&levels);
    return Py_BuildValue("(NI)", result, position);
}

static PyObject *audiodsp_pitchshift_s16(PyObject *module, PyObject *args) {
    Py_buffer input = {0}, window = {0}, overlap = {0};
    unsigned int window_samples, overlap_samples, channels, read_rate;
    unsigned int window_index, overlap_index, read_index;
    double mix;
    if (!PyArg_ParseTuple(args, "y*w*w*IIIIIIId:pitchshift_s16", &input,
        &window, &overlap, &window_samples, &overlap_samples, &channels,
        &read_rate, &window_index, &overlap_index, &read_index, &mix)) return NULL;
    if (input.len % sizeof(int16_t) || channels < 1 || channels > 2 ||
        window_samples < 1 || window.len <
            (Py_ssize_t)((size_t)window_samples * channels * sizeof(int16_t)) ||
        overlap.len < (Py_ssize_t)((size_t)overlap_samples * channels *
            sizeof(int16_t))) {
        PyBuffer_Release(&input); PyBuffer_Release(&window);
        PyBuffer_Release(&overlap);
        PyErr_SetString(PyExc_ValueError, "invalid pitch-shift parameters");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, input.len);
    if (result == NULL) {
        PyBuffer_Release(&input); PyBuffer_Release(&window);
        PyBuffer_Release(&overlap); return NULL;
    }
    audiodsp_pitchshift_positions_t positions = {
        window_index, overlap_index, read_index};
    audiodsp_pitchshift_process_s16((int16_t *)PyBytes_AS_STRING(result),
        input.buf, input.len / sizeof(int16_t), window.buf, window_samples,
        overlap.buf, overlap_samples, (uint8_t)channels, read_rate, mix,
        &positions);
    PyBuffer_Release(&input); PyBuffer_Release(&window);
    PyBuffer_Release(&overlap);
    return Py_BuildValue("(NIII)", result, positions.window_index,
        positions.overlap_index, positions.read_index);
}

static PyObject *audiodsp_freeverb_s16(PyObject *module, PyObject *args) {
    Py_buffer input = {0}, comb = {0}, comb_indices = {0}, filters = {0};
    Py_buffer allpass = {0}, allpass_indices = {0};
    double roomsize, damp, mix;
    if (!PyArg_ParseTuple(args, "y*w*w*w*w*w*ddd:freeverb_s16", &input,
        &comb, &comb_indices, &filters, &allpass, &allpass_indices,
        &roomsize, &damp, &mix)) return NULL;
    if (input.len % sizeof(int16_t) ||
        comb.len < AUDIODSP_FREEVERB_COMB_SAMPLES * (Py_ssize_t)sizeof(int16_t) ||
        comb_indices.len < 8 * (Py_ssize_t)sizeof(uint32_t) ||
        filters.len < 8 * (Py_ssize_t)sizeof(int16_t) ||
        allpass.len < AUDIODSP_FREEVERB_ALLPASS_SAMPLES *
            (Py_ssize_t)sizeof(int16_t) ||
        allpass_indices.len < 4 * (Py_ssize_t)sizeof(uint32_t)) {
        PyBuffer_Release(&input); PyBuffer_Release(&comb);
        PyBuffer_Release(&comb_indices); PyBuffer_Release(&filters);
        PyBuffer_Release(&allpass); PyBuffer_Release(&allpass_indices);
        PyErr_SetString(PyExc_ValueError, "invalid freeverb state");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, input.len);
    if (result != NULL) {
        audiodsp_freeverb_process_s16((int16_t *)PyBytes_AS_STRING(result),
            input.buf, input.len / sizeof(int16_t), comb.buf,
            comb_indices.buf, filters.buf, allpass.buf, allpass_indices.buf,
            roomsize, damp, mix);
    }
    PyBuffer_Release(&input); PyBuffer_Release(&comb);
    PyBuffer_Release(&comb_indices); PyBuffer_Release(&filters);
    PyBuffer_Release(&allpass); PyBuffer_Release(&allpass_indices);
    return result;
}

static PyMethodDef audiodsp_methods[] = {
    {"mix_s16", audiodsp_mix_s16, METH_VARARGS, PyDoc_STR("Saturating mix of two native-endian signed 16-bit PCM buffers.")},
    {"oscillator_i32", audiodsp_oscillator_i32, METH_VARARGS, NULL},
    {"oscillator_raw_i32", audiodsp_oscillator_raw_i32, METH_VARARGS, NULL},
    {"ring_multiply_i32", audiodsp_ring_multiply_i32, METH_VARARGS, NULL},
    {"apply_loudness_i32", audiodsp_apply_loudness_i32, METH_VARARGS, NULL},
    {"mixdown_i32", audiodsp_mixdown_i32, METH_VARARGS, NULL},
    {"pitch_bend", audiodsp_pitch_bend_value, METH_VARARGS, NULL},
    {"distortion_s16", audiodsp_distortion_s16, METH_VARARGS, NULL},
    {"echo_s16", audiodsp_echo_s16, METH_VARARGS, NULL},
    {"phaser_s16", audiodsp_phaser_s16, METH_VARARGS, NULL},
    {"chorus_s16", audiodsp_chorus_s16, METH_VARARGS, NULL},
    {"multitap_s16", audiodsp_multitap_s16, METH_VARARGS, NULL},
    {"pitchshift_s16", audiodsp_pitchshift_s16, METH_VARARGS, NULL},
    {"freeverb_s16", audiodsp_freeverb_s16, METH_VARARGS, NULL},
    {"multiply_s16", audiodsp_multiply_s16, METH_VARARGS, NULL},
    {"midside_s16", audiodsp_midside_s16, METH_VARARGS, NULL},
    {"remix_s16", audiodsp_py_remix_s16, METH_VARARGS, PyDoc_STR("Interleaved s16 native-endian channel convert between 1 and 2 channels. Optional writable dest.")},
    {NULL, NULL, 0, NULL},
};

static int audiodsp_exec(PyObject *module) {
    audiodsp_state_t *state = PyModule_GetState(module);
    state->buffer_owner_type = PyType_FromModuleAndSpec(module, &buffer_owner_spec, NULL);
    if (state->buffer_owner_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "BufferOwner", state->buffer_owner_type) < 0) return -1;
    state->rawsample_type = PyType_FromModuleAndSpec(module, &rawsample_spec, NULL);
    if (state->rawsample_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "RawSample", state->rawsample_type) < 0) return -1;
    state->envelope_state_type = PyType_FromModuleAndSpec(module,
        &envelope_state_spec, NULL);
    if (state->envelope_state_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "EnvelopeState",
        state->envelope_state_type) < 0) return -1;
    state->biquad_state_type = PyType_FromModuleAndSpec(module,
        &biquad_state_spec, NULL);
    if (state->biquad_state_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "BiquadState",
        state->biquad_state_type) < 0) return -1;
    state->dynamics_state_type = PyType_FromModuleAndSpec(module,
        &dynamics_state_spec, NULL);
    if (state->dynamics_state_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "DynamicsState",
        state->dynamics_state_type) < 0) return -1;
    state->splitter_ring_type = PyType_FromModuleAndSpec(module,
        &splitter_ring_spec, NULL);
    if (state->splitter_ring_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "SplitterRing",
        state->splitter_ring_type) < 0) return -1;
    if (PyModule_AddIntConstant(module, "SPLITTER_CHUNK_FRAMES",
        AUDIODSP_SPLITTER_CHUNK_FRAMES) < 0) return -1;
    // The ring's depth. A caller reasoning about a block bigger than this
    // needs the number rather than a comment about it. audiodsp#87.
    if (PyModule_AddIntConstant(module, "SPLITTER_RING_FRAMES",
        AUDIODSP_SPLITTER_RING_FRAMES) < 0) return -1;
    if (PyModule_AddIntConstant(module, "DYNAMICS_FRAMES",
        AUDIODSP_DYNAMICS_FRAMES) < 0) return -1;
    if (PyModule_AddIntConstant(module, "MULTIPLY_FRAMES",
        AUDIODSP_MULTIPLY_FRAMES) < 0) return -1;
    if (PyModule_AddIntConstant(module, "MIDSIDE_FRAMES",
        AUDIODSP_MIDSIDE_FRAMES) < 0) return -1;
    state->feedback_delay_state_type = PyType_FromModuleAndSpec(module,
        &feedback_delay_state_spec, NULL);
    if (state->feedback_delay_state_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "FeedbackDelayState",
        state->feedback_delay_state_type) < 0) return -1;
    if (PyModule_AddIntConstant(module, "FEEDBACK_DELAY_FRAMES",
        AUDIODSP_FEEDBACK_DELAY_FRAMES) < 0) return -1;
    state->waveshaper_state_type = PyType_FromModuleAndSpec(module,
        &waveshaper_state_spec, NULL);
    if (state->waveshaper_state_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "WaveshaperState",
        state->waveshaper_state_type) < 0) return -1;
    if (PyModule_AddIntConstant(module, "SHAPER_FRAMES",
        AUDIODSP_SHAPER_FRAMES) < 0) return -1;
    if (PyModule_AddIntConstant(module, "SHAPER_MAX_OVERSAMPLE",
        AUDIODSP_SHAPER_MAX_OVERSAMPLE) < 0) return -1;
    state->samplehold_state_type = PyType_FromModuleAndSpec(module,
        &samplehold_state_spec, NULL);
    if (state->samplehold_state_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "SampleHoldState",
        state->samplehold_state_type) < 0) return -1;
    if (PyModule_AddIntConstant(module, "SAMPLEHOLD_FRAMES",
        AUDIODSP_SAMPLEHOLD_FRAMES) < 0) return -1;
    if (PyModule_AddIntConstant(module, "SAMPLEHOLD_MAX_RATIO",
        AUDIODSP_SAMPLEHOLD_MAX_RATIO) < 0) return -1;
    state->ladder_state_type = PyType_FromModuleAndSpec(module,
        &ladder_state_spec, NULL);
    if (state->ladder_state_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "LadderState",
        state->ladder_state_type) < 0) return -1;
    if (PyModule_AddIntConstant(module, "LADDER_FRAMES",
        AUDIODSP_LADDER_FRAMES) < 0) return -1;
    state->biquad_f32_state_type = PyType_FromModuleAndSpec(module,
        &biquad_f32_state_spec, NULL);
    if (state->biquad_f32_state_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "BiquadF32State",
        state->biquad_f32_state_type) < 0) return -1;
    state->allpass_f32_state_type = PyType_FromModuleAndSpec(module,
        &allpass_f32_state_spec, NULL);
    if (state->allpass_f32_state_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "AllPassF32State",
        state->allpass_f32_state_type) < 0) return -1;
    if (PyModule_AddIntConstant(module, "FILTER_F32_FRAMES",
        AUDIODSP_FILTER_F32_FRAMES) < 0) return -1;
    if (PyModule_AddIntConstant(module, "FILTER_F32_MAX_STAGES",
        AUDIODSP_FILTER_F32_MAX_STAGES) < 0) return -1;
    state->suboctave_state_type = PyType_FromModuleAndSpec(module,
        &suboctave_state_spec, NULL);
    if (state->suboctave_state_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "SubOctaveState",
        state->suboctave_state_type) < 0) return -1;
    if (PyModule_AddIntConstant(module, "SUBOCTAVE_FRAMES",
        AUDIODSP_SUBOCTAVE_FRAMES) < 0) return -1;
    state->convolver_state_type = PyType_FromModuleAndSpec(module,
        &convolver_state_spec, NULL);
    if (state->convolver_state_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "ConvolverState",
        state->convolver_state_type) < 0) return -1;
    if (PyModule_AddIntConstant(module, "CONVOLVE_FRAMES",
        AUDIODSP_CONVOLVE_FRAMES) < 0) return -1;
    if (PyModule_AddIntConstant(module, "CONVOLVE_MAX_PARTITIONS",
        AUDIODSP_CONVOLVE_MAX_PARTITIONS) < 0) return -1;
    state->tank_state_type = PyType_FromModuleAndSpec(module,
        &tank_state_spec, NULL);
    if (state->tank_state_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "TankState",
        state->tank_state_type) < 0) return -1;
    state->modal_state_type = PyType_FromModuleAndSpec(module,
        &modal_state_spec, NULL);
    if (state->modal_state_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "ModalState",
        state->modal_state_type) < 0) return -1;
    if (PyModule_AddIntConstant(module, "MODAL_FRAMES",
        AUDIODSP_MODAL_FRAMES) < 0) return -1;
    if (PyModule_AddIntConstant(module, "MODAL_MAX_MODES",
        AUDIODSP_MODAL_MAX_MODES) < 0) return -1;
    if (PyModule_AddIntConstant(module, "TANK_FRAMES",
        AUDIODSP_TANK_FRAMES) < 0) return -1;
    if (PyModule_AddIntConstant(module, "TANK_LINES",
        AUDIODSP_TANK_LINES) < 0) return -1;
    if (PyModule_AddIntConstant(module, "TANK_MAX_TAPS",
        AUDIODSP_TANK_MAX_TAPS) < 0) return -1;
    if (PyModule_AddStringConstant(module, "__version__",
    AUDIODSP_VERSION) < 0) return -1;
    // The commit this was built from, so the CPython target answers the same
    // question a board does. audiodsp#55; see src/cp_compat/audiodsp_build.h for
    // why "unknown" is a real answer and not a failure.
    if (PyModule_AddStringConstant(module, "__revision__",
        AUDIODSP_REVISION) < 0) return -1;
    if (PyModule_AddIntConstant(module, "ABI_VERSION", 1) < 0) return -1;
    return 0;
}

static int audiodsp_traverse(PyObject *module, visitproc visit, void *arg) {
    audiodsp_state_t *state = PyModule_GetState(module);
    Py_VISIT(state->error);
    Py_VISIT(state->buffer_owner_type);
    Py_VISIT(state->rawsample_type);
    Py_VISIT(state->envelope_state_type);
    Py_VISIT(state->biquad_state_type);
    Py_VISIT(state->dynamics_state_type);
    Py_VISIT(state->splitter_ring_type);
    Py_VISIT(state->feedback_delay_state_type);
    Py_VISIT(state->waveshaper_state_type);
    Py_VISIT(state->samplehold_state_type);
    Py_VISIT(state->ladder_state_type);
    Py_VISIT(state->biquad_f32_state_type);
    Py_VISIT(state->allpass_f32_state_type);
    Py_VISIT(state->suboctave_state_type);
    Py_VISIT(state->convolver_state_type);
    Py_VISIT(state->tank_state_type);
    Py_VISIT(state->modal_state_type);
    return 0;
}

static int audiodsp_clear(PyObject *module) {
    audiodsp_state_t *state = PyModule_GetState(module);
    Py_CLEAR(state->error);
    Py_CLEAR(state->buffer_owner_type);
    Py_CLEAR(state->rawsample_type);
    Py_CLEAR(state->envelope_state_type);
    Py_CLEAR(state->biquad_state_type);
    Py_CLEAR(state->dynamics_state_type);
    Py_CLEAR(state->splitter_ring_type);
    Py_CLEAR(state->feedback_delay_state_type);
    Py_CLEAR(state->waveshaper_state_type);
    Py_CLEAR(state->samplehold_state_type);
    Py_CLEAR(state->ladder_state_type);
    Py_CLEAR(state->biquad_f32_state_type);
    Py_CLEAR(state->allpass_f32_state_type);
    Py_CLEAR(state->suboctave_state_type);
    Py_CLEAR(state->convolver_state_type);
    Py_CLEAR(state->tank_state_type);
    Py_CLEAR(state->modal_state_type);
    return 0;
}

static PyModuleDef_Slot audiodsp_slots[] = {
    {Py_mod_exec, audiodsp_exec},
    {0, NULL},
};

static struct PyModuleDef audiodsp_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_audiodsp",
    .m_doc = "Private native core for pydevices-audiodsp.",
    .m_size = sizeof(audiodsp_state_t),
    .m_methods = audiodsp_methods,
    .m_slots = audiodsp_slots,
    .m_traverse = audiodsp_traverse,
    .m_clear = audiodsp_clear,
};

PyMODINIT_FUNC PyInit__audiodsp(void) {
    return PyModuleDef_Init(&audiodsp_module);
}
