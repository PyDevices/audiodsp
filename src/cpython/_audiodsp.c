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
#include "shared/audiodsp_cast.h"
#include "shared/audiodsp_remix.h"
#include "shared/audiodsp_multiply.h"
#include "shared/audiodsp_suboctave.h"
#include "shared/audiodsp_convolve.h"
#include "shared/audiodsp_feedback_delay.h"
#include "shared/audiodsp_shaper.h"
#include "shared/audiodsp_samplehold.h"
#include "shared/audiodsp_drift.h"
#include "shared/audiodsp_ladder.h"
#include "shared/audiodsp_oscillator.h"
#include "shared/audiodsp_tank.h"
#include "shared/audiodsp_modal.h"
#include "shared/audiodsp_sampler.h"
#include "shared/audiodsp_meter.h"
#include "shared/audiodsp_features.h"
#include "shared/audiodsp_filter_f32.h"
#include "shared/audiodsp_vorbis.h"
#include "shared/audiodsp_graph.h"

// setup.py defines this from the VERSION file; the fallback is only for
// someone compiling this source by hand.
#ifndef AUDIODSP_VERSION
#define AUDIODSP_VERSION "0.0.0+unknown"
#endif
#ifndef AUDIODSP_REVISION
#define AUDIODSP_REVISION "unknown"
#endif

// Every method, getter, setter and __init__ of the node types below runs
// inside a critical section on its own object. Without the GIL, a call that
// reconfigures a node (a new curve, a new room, __init__ run again) while
// another thread is in the middle of rendering it would free buffers the
// render is still reading; with it, the second call waits. On a build with
// the GIL these are no-ops (and before 3.13 they do not exist). Using one
// node from two threads at once is still not supported: the lock keeps the
// interpreter alive, it doesn't make the interleaving of two callers mean
// anything.
#ifndef Py_BEGIN_CRITICAL_SECTION
#define Py_BEGIN_CRITICAL_SECTION(op) {
#define Py_END_CRITICAL_SECTION() }
#endif

// Each wraps an existing implementation `name`, whose self is a `type *`, as
// `name_locked`, which is what the type's tables point at. Every PyCFunction
// shape a node type uses (METH_NOARGS, METH_O, METH_VARARGS) is self plus one
// object.
#define AUDIODSP_LOCKED_ARGS(name, type) \
    static PyObject *name##_locked(PyObject *self, PyObject *arg) { \
        PyObject *result; \
        Py_BEGIN_CRITICAL_SECTION(self); \
        result = name((type *)self, arg); \
        Py_END_CRITICAL_SECTION(); \
        return result; \
    }
#define AUDIODSP_LOCKED_KW(name, type) \
    static PyObject *name##_locked(PyObject *self, PyObject *args, \
        PyObject *kwargs) { \
        PyObject *result; \
        Py_BEGIN_CRITICAL_SECTION(self); \
        result = name((type *)self, args, kwargs); \
        Py_END_CRITICAL_SECTION(); \
        return result; \
    }
#define AUDIODSP_LOCKED_INIT(name, type) \
    static int name##_locked(PyObject *self, PyObject *args, \
        PyObject *kwargs) { \
        int result; \
        Py_BEGIN_CRITICAL_SECTION(self); \
        result = name((type *)self, args, kwargs); \
        Py_END_CRITICAL_SECTION(); \
        return result; \
    }
#define AUDIODSP_LOCKED_GET(name, type) \
    static PyObject *name##_locked(PyObject *self, void *closure) { \
        PyObject *result; \
        Py_BEGIN_CRITICAL_SECTION(self); \
        result = name((type *)self, closure); \
        Py_END_CRITICAL_SECTION(); \
        return result; \
    }
#define AUDIODSP_LOCKED_SET(name, type) \
    static int name##_locked(PyObject *self, PyObject *value, \
        void *closure) { \
        int result; \
        Py_BEGIN_CRITICAL_SECTION(self); \
        result = name((type *)self, value, closure); \
        Py_END_CRITICAL_SECTION(); \
        return result; \
    }

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
    PyObject *drift_state_type;
    PyObject *ladder_state_type;
    PyObject *oscillator_state_type;
    PyObject *biquad_f32_state_type;
    PyObject *allpass_f32_state_type;
    PyObject *suboctave_state_type;
    PyObject *convolver_state_type;
    PyObject *meter_state_type;
    PyObject *features_state_type;
    PyObject *vorbis_stream_type;
    PyObject *tank_state_type;
    PyObject *modal_state_type;
    PyObject *sampler_state_type;
    PyObject *graph_type;
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

// A held `Py_buffer` is deliberately NOT visited, here or in RawSample. If
// the exporter is a `memoryview` and the collector finds it in a cycle with
// this object, it may clear the memoryview first: `memory_clear` cannot
// release a view that still has an export, so it drops the managed buffer
// anyway and our later `PyBuffer_Release` runs against a dead view, a
// segfault in the collector. Left unvisited, our reference keeps the exporter
// reachable for as long as we hold it, so it is only ever freed after we
// release it. The cost is a leak, not a crash, for the rare exporter whose
// own references lead back to us. Heap types visit their type.
static int buffer_owner_traverse(audiodsp_buffer_owner_t *self, visitproc visit, void *arg) {
    Py_VISIT(Py_TYPE(self));
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

AUDIODSP_LOCKED_ARGS(buffer_owner_release, audiodsp_buffer_owner_t)
AUDIODSP_LOCKED_ARGS(buffer_owner_bytes, audiodsp_buffer_owner_t)
static PyMethodDef buffer_owner_methods[] = {
    {"release", (PyCFunction)buffer_owner_release_locked, METH_NOARGS, NULL},
    {"bytes", (PyCFunction)buffer_owner_bytes_locked, METH_NOARGS, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_GET(buffer_owner_format, audiodsp_buffer_owner_t)
AUDIODSP_LOCKED_GET(buffer_owner_nbytes, audiodsp_buffer_owner_t)
AUDIODSP_LOCKED_GET(buffer_owner_itemsize, audiodsp_buffer_owner_t)
static PyGetSetDef buffer_owner_getset[] = {
    {"format", (getter)buffer_owner_format_locked, NULL, NULL, NULL},
    {"nbytes", (getter)buffer_owner_nbytes_locked, NULL, NULL, NULL},
    {"itemsize", (getter)buffer_owner_itemsize_locked, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

AUDIODSP_LOCKED_INIT(buffer_owner_init, audiodsp_buffer_owner_t)
static PyType_Slot buffer_owner_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, buffer_owner_init_locked},
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
    // Not `exporter.obj`: see buffer_owner_traverse.
    Py_VISIT(Py_TYPE(self));
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

// The most one pull can hand back: the whole buffer, as the native RawSample's
// base struct says. The pump sizes its ring from it, and Python has no other
// way to ask.
static PyObject *rawsample_get_max_buffer_length(
    audiodsp_rawsample_object_t *self, void *closure) {
    return PyLong_FromUnsignedLong(self->info.max_buffer_length);
}

AUDIODSP_LOCKED_ARGS(rawsample_deinit, audiodsp_rawsample_object_t)
AUDIODSP_LOCKED_KW(rawsample_reset_buffer, audiodsp_rawsample_object_t)
AUDIODSP_LOCKED_KW(rawsample_get_buffer, audiodsp_rawsample_object_t)
AUDIODSP_LOCKED_ARGS(rawsample_enter, audiodsp_rawsample_object_t)
AUDIODSP_LOCKED_ARGS(rawsample_exit, audiodsp_rawsample_object_t)
static PyMethodDef rawsample_methods[] = {
    {"deinit", (PyCFunction)rawsample_deinit_locked, METH_NOARGS, NULL},
    {"_reset_buffer", (PyCFunction)rawsample_reset_buffer_locked, METH_VARARGS | METH_KEYWORDS, NULL},
    {"_get_buffer", (PyCFunction)rawsample_get_buffer_locked, METH_VARARGS | METH_KEYWORDS, NULL},
    {"__enter__", (PyCFunction)rawsample_enter_locked, METH_NOARGS, NULL},
    {"__exit__", (PyCFunction)rawsample_exit_locked, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_GET(rawsample_get_sample_rate, audiodsp_rawsample_object_t)
AUDIODSP_LOCKED_GET(rawsample_get_bits_per_sample, audiodsp_rawsample_object_t)
AUDIODSP_LOCKED_GET(rawsample_get_channel_count, audiodsp_rawsample_object_t)
AUDIODSP_LOCKED_GET(rawsample_get_samples_signed, audiodsp_rawsample_object_t)
AUDIODSP_LOCKED_GET(rawsample_get_single_buffer, audiodsp_rawsample_object_t)
AUDIODSP_LOCKED_GET(rawsample_get_max_buffer_length, audiodsp_rawsample_object_t)
AUDIODSP_LOCKED_SET(rawsample_set_sample_rate, audiodsp_rawsample_object_t)
static PyGetSetDef rawsample_getset[] = {
    {"sample_rate", (getter)rawsample_get_sample_rate_locked, (setter)rawsample_set_sample_rate_locked, NULL, NULL},
    {"bits_per_sample", (getter)rawsample_get_bits_per_sample_locked, NULL, NULL, NULL},
    {"channel_count", (getter)rawsample_get_channel_count_locked, NULL, NULL, NULL},
    {"samples_signed", (getter)rawsample_get_samples_signed_locked, NULL, NULL, NULL},
    {"single_buffer", (getter)rawsample_get_single_buffer_locked, NULL, NULL, NULL},
    {"max_buffer_length", (getter)rawsample_get_max_buffer_length_locked, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

AUDIODSP_LOCKED_INIT(rawsample_init, audiodsp_rawsample_object_t)
static PyType_Slot rawsample_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, rawsample_init_locked},
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
// at press time, so it needs this to stay faithful. See docs/how-it-works/upstream-diff.md.
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

AUDIODSP_LOCKED_ARGS(envelope_state_step, audiodsp_envelope_state_object_t)
AUDIODSP_LOCKED_ARGS(envelope_state_release, audiodsp_envelope_state_object_t)
AUDIODSP_LOCKED_ARGS(envelope_state_reattack, audiodsp_envelope_state_object_t)
AUDIODSP_LOCKED_KW(envelope_state_set_definition, audiodsp_envelope_state_object_t)
static PyMethodDef envelope_state_methods[] = {
    {"step", (PyCFunction)envelope_state_step_locked, METH_O, NULL},
    {"release", (PyCFunction)envelope_state_release_locked, METH_NOARGS, NULL},
    {"reattack", (PyCFunction)envelope_state_reattack_locked, METH_NOARGS, NULL},
    {"set_definition", (PyCFunction)envelope_state_set_definition_locked,
     METH_VARARGS | METH_KEYWORDS, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_GET(envelope_state_level, audiodsp_envelope_state_object_t)
AUDIODSP_LOCKED_GET(envelope_state_kind, audiodsp_envelope_state_object_t)
static PyGetSetDef envelope_state_getset[] = {
    {"level", (getter)envelope_state_level_locked, NULL, NULL, NULL},
    {"state", (getter)envelope_state_kind_locked, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

AUDIODSP_LOCKED_INIT(envelope_state_init, audiodsp_envelope_state_object_t)
static PyType_Slot envelope_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, envelope_state_init_locked},
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
// docs/how-it-works/upstream-diff.md. CircuitPython caps channel_count at 2.
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
    // they render CircuitPython's bytes. audiodsp#77, 2026-09-09.
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

AUDIODSP_LOCKED_ARGS(biquad_state_reset, audiodsp_biquad_state_object_t)
AUDIODSP_LOCKED_ARGS(biquad_state_process, audiodsp_biquad_state_object_t)
AUDIODSP_LOCKED_ARGS(biquad_state_process_s16, audiodsp_biquad_state_object_t)
static PyMethodDef biquad_state_methods[] = {
    {"reset", (PyCFunction)biquad_state_reset_locked, METH_NOARGS, NULL},
    {"process_i32", (PyCFunction)biquad_state_process_locked, METH_VARARGS, NULL},
    {"process_s16", (PyCFunction)biquad_state_process_s16_locked, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_INIT(biquad_state_init, audiodsp_biquad_state_object_t)
static PyType_Slot biquad_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, biquad_state_init_locked},
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

AUDIODSP_LOCKED_ARGS(dynamics_state_set_sample_rate, audiodsp_dynamics_object_t)
AUDIODSP_LOCKED_ARGS(dynamics_state_configure, audiodsp_dynamics_object_t)
AUDIODSP_LOCKED_ARGS(dynamics_state_finish, audiodsp_dynamics_object_t)
AUDIODSP_LOCKED_ARGS(dynamics_state_reset, audiodsp_dynamics_object_t)
AUDIODSP_LOCKED_ARGS(dynamics_state_process, audiodsp_dynamics_object_t)
static PyMethodDef dynamics_state_methods[] = {
    {"set_sample_rate", (PyCFunction)dynamics_state_set_sample_rate_locked, METH_O, NULL},
    {"configure", (PyCFunction)dynamics_state_configure_locked, METH_VARARGS, NULL},
    {"finish", (PyCFunction)dynamics_state_finish_locked, METH_NOARGS, NULL},
    {"reset", (PyCFunction)dynamics_state_reset_locked, METH_NOARGS, NULL},
    {"process", (PyCFunction)dynamics_state_process_locked, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_GET(dynamics_state_gain_reduction_db, audiodsp_dynamics_object_t)
AUDIODSP_LOCKED_GET(dynamics_state_get_sample_rate, audiodsp_dynamics_object_t)
static PyGetSetDef dynamics_state_getset[] = {
    {"gain_reduction_db", (getter)dynamics_state_gain_reduction_db_locked, NULL, NULL, NULL},
    {"sample_rate", (getter)dynamics_state_get_sample_rate_locked, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

AUDIODSP_LOCKED_INIT(dynamics_state_init, audiodsp_dynamics_object_t)
static PyType_Slot dynamics_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, dynamics_state_init_locked},
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

static PyObject *splitter_ring_clear(audiodsp_splitter_object_t *self,
    PyObject *Py_UNUSED(ignored)) {
    audiodsp_splitter_clear(&self->state);
    Py_RETURN_NONE;
}

AUDIODSP_LOCKED_ARGS(splitter_ring_write, audiodsp_splitter_object_t)
AUDIODSP_LOCKED_ARGS(splitter_ring_starved, audiodsp_splitter_object_t)
AUDIODSP_LOCKED_ARGS(splitter_ring_take, audiodsp_splitter_object_t)
AUDIODSP_LOCKED_ARGS(splitter_ring_clear, audiodsp_splitter_object_t)
static PyMethodDef splitter_ring_methods[] = {
    {"write", (PyCFunction)splitter_ring_write_locked, METH_O, NULL},
    {"starved", (PyCFunction)splitter_ring_starved_locked, METH_O, NULL},
    {"take", (PyCFunction)splitter_ring_take_locked, METH_O, NULL},
    {"clear", (PyCFunction)splitter_ring_clear_locked, METH_NOARGS, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_INIT(splitter_ring_init, audiodsp_splitter_object_t)
static PyType_Slot splitter_ring_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, splitter_ring_init_locked},
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
    // The table the state still reads while a new one waits for the wow
    // depth to ramp to zero (`state.wow_shape_playing`), held the same way
    // until the swap.
    Py_buffer wow_shape_playing;
    int wow_shape_playing_held;
} audiodsp_feedback_delay_object_t;

static void feedback_delay_release_shape(
    audiodsp_feedback_delay_object_t *self) {
    if (self->wow_shape_held) {
        PyBuffer_Release(&self->wow_shape);
        self->wow_shape_held = 0;
    }
    if (self->wow_shape_playing_held) {
        PyBuffer_Release(&self->wow_shape_playing);
        self->wow_shape_playing_held = 0;
    }
}

// The config's table is about to be replaced (it was `old`). A state still
// reading it keeps doing so until its depth reaches zero, so its view moves
// to `wow_shape_playing`; otherwise it is let go.
static void feedback_delay_retire_shape(
    audiodsp_feedback_delay_object_t *self, const int16_t *old) {
    if (!self->wow_shape_held) {
        return;
    }
    if (self->state.wow_shape_playing == old) {
        if (self->wow_shape_playing_held) {
            PyBuffer_Release(&self->wow_shape_playing);
        }
        self->wow_shape_playing = self->wow_shape;
        self->wow_shape_playing_held = 1;
    } else {
        PyBuffer_Release(&self->wow_shape);
    }
    self->wow_shape_held = 0;
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
        option > AUDIODSP_FEEDBACK_DELAY_OPT_WOW_DEPTH_RAMP_MS) {
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
    const int16_t *old = self->config.wow_shape;
    if (argument == Py_None) {
        audiodsp_feedback_delay_set_wow_shape(&self->config, NULL, 0);
        feedback_delay_retire_shape(self, old);
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
    feedback_delay_retire_shape(self, old);
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

// `frames` of output from silence: the node's source has ended or run dry,
// so its tail rings out, and once it has ended the node rests (audiodsp#180).
static PyObject *feedback_delay_state_process_silence(
    audiodsp_feedback_delay_object_t *self, PyObject *argument) {
    const Py_ssize_t frames = PyLong_AsSsize_t(argument);
    if (frames == -1 && PyErr_Occurred()) return NULL;
    if (frames < 0) {
        PyErr_SetString(PyExc_ValueError, "frames must not be negative");
        return NULL;
    }
    const Py_ssize_t width = 2 * (Py_ssize_t)(self->config.channel_count);
    PyObject *result = PyBytes_FromStringAndSize(NULL, frames * width);
    if (result != NULL) {
        audiodsp_feedback_delay_process_silence(&self->config, &self->state,
            (int16_t *)PyBytes_AS_STRING(result), (uint32_t)frames);
    }
    return result;
}

AUDIODSP_LOCKED_ARGS(feedback_delay_state_configure, audiodsp_feedback_delay_object_t)
AUDIODSP_LOCKED_ARGS(feedback_delay_state_wow_shape, audiodsp_feedback_delay_object_t)
AUDIODSP_LOCKED_ARGS(feedback_delay_state_finish, audiodsp_feedback_delay_object_t)
AUDIODSP_LOCKED_ARGS(feedback_delay_state_reset, audiodsp_feedback_delay_object_t)
AUDIODSP_LOCKED_ARGS(feedback_delay_state_process, audiodsp_feedback_delay_object_t)
AUDIODSP_LOCKED_ARGS(feedback_delay_state_process_silence, audiodsp_feedback_delay_object_t)
static PyMethodDef feedback_delay_state_methods[] = {
    {"configure", (PyCFunction)feedback_delay_state_configure_locked, METH_VARARGS, NULL},
    {"set_wow_shape", (PyCFunction)feedback_delay_state_wow_shape_locked, METH_O, NULL},
    {"finish", (PyCFunction)feedback_delay_state_finish_locked, METH_NOARGS, NULL},
    {"reset", (PyCFunction)feedback_delay_state_reset_locked, METH_NOARGS, NULL},
    {"process", (PyCFunction)feedback_delay_state_process_locked, METH_O, NULL},
    {"process_silence", (PyCFunction)feedback_delay_state_process_silence_locked, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_INIT(feedback_delay_state_init, audiodsp_feedback_delay_object_t)
static PyType_Slot feedback_delay_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, feedback_delay_state_init_locked},
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

AUDIODSP_LOCKED_ARGS(biquad_f32_state_configure, audiodsp_biquad_f32_object_t)
AUDIODSP_LOCKED_ARGS(biquad_f32_state_finish, audiodsp_biquad_f32_object_t)
AUDIODSP_LOCKED_ARGS(biquad_f32_state_reset, audiodsp_biquad_f32_object_t)
AUDIODSP_LOCKED_ARGS(biquad_f32_state_coefficients, audiodsp_biquad_f32_object_t)
AUDIODSP_LOCKED_ARGS(biquad_f32_state_process, audiodsp_biquad_f32_object_t)
static PyMethodDef biquad_f32_state_methods[] = {
    {"configure", (PyCFunction)biquad_f32_state_configure_locked, METH_VARARGS, NULL},
    {"finish", (PyCFunction)biquad_f32_state_finish_locked, METH_NOARGS, NULL},
    {"reset", (PyCFunction)biquad_f32_state_reset_locked, METH_NOARGS, NULL},
    {"coefficients", (PyCFunction)biquad_f32_state_coefficients_locked, METH_NOARGS, NULL},
    {"process", (PyCFunction)biquad_f32_state_process_locked, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_INIT(biquad_f32_state_init, audiodsp_biquad_f32_object_t)
static PyType_Slot biquad_f32_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, biquad_f32_state_init_locked},
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

AUDIODSP_LOCKED_ARGS(allpass_f32_state_configure, audiodsp_allpass_f32_object_t)
AUDIODSP_LOCKED_ARGS(allpass_f32_state_finish, audiodsp_allpass_f32_object_t)
AUDIODSP_LOCKED_ARGS(allpass_f32_state_reset, audiodsp_allpass_f32_object_t)
AUDIODSP_LOCKED_ARGS(allpass_f32_state_coefficient, audiodsp_allpass_f32_object_t)
AUDIODSP_LOCKED_ARGS(allpass_f32_state_process, audiodsp_allpass_f32_object_t)
static PyMethodDef allpass_f32_state_methods[] = {
    {"configure", (PyCFunction)allpass_f32_state_configure_locked, METH_VARARGS, NULL},
    {"finish", (PyCFunction)allpass_f32_state_finish_locked, METH_NOARGS, NULL},
    {"reset", (PyCFunction)allpass_f32_state_reset_locked, METH_NOARGS, NULL},
    {"coefficient", (PyCFunction)allpass_f32_state_coefficient_locked, METH_NOARGS, NULL},
    {"process", (PyCFunction)allpass_f32_state_process_locked, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_INIT(allpass_f32_state_init, audiodsp_allpass_f32_object_t)
static PyType_Slot allpass_f32_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, allpass_f32_state_init_locked},
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
    if (oversample < 1 || oversample > (int)AUDIODSP_SHAPER_MAX_OVERSAMPLE ||
        (oversample & (oversample - 1)) != 0) {
        PyErr_Format(PyExc_ValueError,
            "oversample must be a power of two, 1 to %d",
            (int)AUDIODSP_SHAPER_MAX_OVERSAMPLE);
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

static PyObject *waveshaper_state_recentre(audiodsp_shaper_object_t *self,
    PyObject *unused) {
    return PyBool_FromLong(audiodsp_shaper_recentre(&self->config,
        &self->state));
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

AUDIODSP_LOCKED_ARGS(waveshaper_state_load_curve, audiodsp_shaper_object_t)
AUDIODSP_LOCKED_ARGS(waveshaper_state_configure, audiodsp_shaper_object_t)
AUDIODSP_LOCKED_ARGS(waveshaper_state_finish, audiodsp_shaper_object_t)
AUDIODSP_LOCKED_ARGS(waveshaper_state_reset, audiodsp_shaper_object_t)
AUDIODSP_LOCKED_ARGS(waveshaper_state_recentre, audiodsp_shaper_object_t)
AUDIODSP_LOCKED_ARGS(waveshaper_state_process, audiodsp_shaper_object_t)
static PyMethodDef waveshaper_state_methods[] = {
    {"load_curve", (PyCFunction)waveshaper_state_load_curve_locked, METH_O, NULL},
    {"configure", (PyCFunction)waveshaper_state_configure_locked, METH_VARARGS, NULL},
    {"finish", (PyCFunction)waveshaper_state_finish_locked, METH_NOARGS, NULL},
    {"reset", (PyCFunction)waveshaper_state_reset_locked, METH_NOARGS, NULL},
    {"recentre", (PyCFunction)waveshaper_state_recentre_locked, METH_NOARGS, NULL},
    {"process", (PyCFunction)waveshaper_state_process_locked, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_INIT(waveshaper_state_init, audiodsp_shaper_object_t)
static PyType_Slot waveshaper_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, waveshaper_state_init_locked},
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

AUDIODSP_LOCKED_ARGS(samplehold_state_configure, audiodsp_samplehold_object_t)
AUDIODSP_LOCKED_ARGS(samplehold_state_ratio, audiodsp_samplehold_object_t)
AUDIODSP_LOCKED_ARGS(samplehold_state_reset, audiodsp_samplehold_object_t)
AUDIODSP_LOCKED_ARGS(samplehold_state_process, audiodsp_samplehold_object_t)
static PyMethodDef samplehold_state_methods[] = {
    {"configure", (PyCFunction)samplehold_state_configure_locked, METH_VARARGS, NULL},
    {"ratio", (PyCFunction)samplehold_state_ratio_locked, METH_NOARGS, NULL},
    {"reset", (PyCFunction)samplehold_state_reset_locked, METH_NOARGS, NULL},
    {"process", (PyCFunction)samplehold_state_process_locked, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_INIT(samplehold_state_init, audiodsp_samplehold_object_t)
static PyType_Slot samplehold_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, samplehold_state_init_locked},
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

// audiopump.DriftResampler's kernel: the interpolator's history and the
// control loop. The Python class reads the Ring and hands this the frames
// plan() asked for; everything that decides a sample or a ratio is here, in
// the same C the other two runtimes run.

typedef struct {
    PyObject_HEAD
    audiodsp_drift_t drift;
    float *work;
} audiodsp_drift_object_t;

static int drift_state_init(audiodsp_drift_object_t *self, PyObject *args,
    PyObject *kwargs) {
    unsigned int channels, frames, in_rate, out_rate, target, max_ppm;
    unsigned int capacity;
    double bandwidth;
    static char *keywords[] = {"channel_count", "frames", "in_rate",
        "out_rate", "target", "max_ppm", "bandwidth", "capacity", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "IIIIIIdI:DriftState",
        keywords, &channels, &frames, &in_rate, &out_rate, &target, &max_ppm,
        &bandwidth, &capacity)) return -1;
    if (channels < 1 || channels > 2) {
        PyErr_SetString(PyExc_ValueError, "channel_count must be 1 or 2");
        return -1;
    }
    if (frames < 16 || frames > AUDIODSP_DRIFT_MAX_FRAMES) {
        PyErr_SetString(PyExc_ValueError, "the ring's frames must be 16 to 1024");
        return -1;
    }
    if (!audiodsp_drift_rates_ok(in_rate, out_rate)) {
        PyErr_SetString(PyExc_ValueError,
            "sample_rate must be from the ring's rate to twice it");
        return -1;
    }
    if (max_ppm > AUDIODSP_DRIFT_MAX_PPM) {
        PyErr_SetString(PyExc_ValueError, "max_ppm must be from 0 to 10000");
        return -1;
    }
    const float bw = (float)bandwidth;
    if (!(bw >= AUDIODSP_DRIFT_MIN_BANDWIDTH && bw <= AUDIODSP_DRIFT_MAX_BANDWIDTH)) {
        PyErr_SetString(PyExc_ValueError, "bandwidth must be from 0.001 to 0.1 Hz");
        return -1;
    }
    const uint32_t least = audiodsp_drift_min_target(frames, in_rate,
        out_rate, max_ppm);
    if (target == 0) {
        // None in Python.
        target = audiodsp_drift_default_target(least, capacity);
    }
    if (target < least || target > audiodsp_drift_max_target(capacity)) {
        PyErr_SetString(PyExc_ValueError,
            "target must cover a block and fit in the ring");
        return -1;
    }
    const uint32_t work = audiodsp_drift_capacity(frames, in_rate, out_rate,
        max_ppm);
    PyMem_Free(self->work);
    self->work = PyMem_Malloc(sizeof(float) * work * channels);
    if (self->work == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    audiodsp_drift_init(&self->drift, self->work, channels, frames, in_rate,
        out_rate, target, max_ppm, bw);
    return 0;
}

static void drift_state_dealloc(audiodsp_drift_object_t *self) {
    PyTypeObject *type = Py_TYPE(self);
    PyMem_Free(self->work);
    type->tp_free((PyObject *)self);
    Py_DECREF(type);
}

static PyObject *drift_state_plan(audiodsp_drift_object_t *self, PyObject *arg) {
    unsigned long level = PyLong_AsUnsignedLong(arg);
    if (level == (unsigned long)-1 && PyErr_Occurred()) return NULL;
    if (self->work == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "DriftState not initialised");
        return NULL;
    }
    return PyLong_FromUnsignedLong(
        audiodsp_drift_plan(&self->drift, (uint32_t)level));
}

static PyObject *drift_state_render(audiodsp_drift_object_t *self,
    PyObject *args) {
    Py_buffer input = {0};
    if (!PyArg_ParseTuple(args, "y*:render", &input)) return NULL;
    const Py_ssize_t frame = (Py_ssize_t)self->drift.channels * 2;
    if (self->work == NULL || input.len != (Py_ssize_t)self->drift.pending * frame) {
        PyBuffer_Release(&input);
        PyErr_SetString(PyExc_ValueError,
            "render takes exactly the frames plan() asked for");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL,
        (Py_ssize_t)self->drift.frames * frame);
    if (result != NULL) {
        audiodsp_drift_push(&self->drift, (const int16_t *)input.buf,
            self->drift.pending);
        audiodsp_drift_render(&self->drift,
            (int16_t *)PyBytes_AS_STRING(result));
    }
    PyBuffer_Release(&input);
    return result;
}

static PyObject *drift_state_reset(audiodsp_drift_object_t *self,
    PyObject *unused) {
    (void)unused;
    if (self->work != NULL) audiodsp_drift_reset(&self->drift);
    Py_RETURN_NONE;
}

// (ppm, fill, rate, underruns, target)
static PyObject *drift_state_status(audiodsp_drift_object_t *self,
    PyObject *unused) {
    (void)unused;
    return Py_BuildValue("(dddkk)",
        (double)audiodsp_drift_ppm(&self->drift),
        (double)audiodsp_drift_fill(&self->drift),
        (double)audiodsp_drift_rate(&self->drift),
        (unsigned long)self->drift.underruns,
        (unsigned long)self->drift.target);
}

static PyObject *drift_state_set_target(audiodsp_drift_object_t *self,
    PyObject *args) {
    unsigned int target, capacity;
    if (!PyArg_ParseTuple(args, "II:set_target", &target, &capacity)) return NULL;
    const audiodsp_drift_t *d = &self->drift;
    if (target < audiodsp_drift_min_target(d->frames, d->in_rate, d->out_rate,
        d->max_ppm) || target > audiodsp_drift_max_target(capacity)) {
        PyErr_SetString(PyExc_ValueError,
            "target must cover a block and fit in the ring");
        return NULL;
    }
    self->drift.target = target;
    Py_RETURN_NONE;
}

AUDIODSP_LOCKED_ARGS(drift_state_plan, audiodsp_drift_object_t)
AUDIODSP_LOCKED_ARGS(drift_state_render, audiodsp_drift_object_t)
AUDIODSP_LOCKED_ARGS(drift_state_reset, audiodsp_drift_object_t)
AUDIODSP_LOCKED_ARGS(drift_state_status, audiodsp_drift_object_t)
AUDIODSP_LOCKED_ARGS(drift_state_set_target, audiodsp_drift_object_t)
static PyMethodDef drift_state_methods[] = {
    {"plan", (PyCFunction)drift_state_plan_locked, METH_O, NULL},
    {"render", (PyCFunction)drift_state_render_locked, METH_VARARGS, NULL},
    {"reset", (PyCFunction)drift_state_reset_locked, METH_NOARGS, NULL},
    {"status", (PyCFunction)drift_state_status_locked, METH_NOARGS, NULL},
    {"set_target", (PyCFunction)drift_state_set_target_locked, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_INIT(drift_state_init, audiodsp_drift_object_t)
static PyType_Slot drift_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, drift_state_init_locked},
    {Py_tp_dealloc, drift_state_dealloc},
    {Py_tp_methods, drift_state_methods},
    {0, NULL},
};

static PyType_Spec drift_state_spec = {
    .name = "_audiodsp.DriftState",
    .basicsize = sizeof(audiodsp_drift_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = drift_state_slots,
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
        option > AUDIODSP_LADDER_OPT_CUTOFF_OCTAVES) {
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

// process(), with the cutoff read every sample from `cutoff`: whole frames of
// signed 16-bit audio, `channels` samples a frame, of which the first is
// read. A separate method so the unmodulated path keeps its single argument.
static PyObject *ladder_state_process_cutoff(audiodsp_ladder_object_t *self,
    PyObject *args) {
    PyObject *input_object;
    PyObject *cutoff_object;
    unsigned int channels;
    if (!PyArg_ParseTuple(args, "OOI:process_cutoff", &input_object,
        &cutoff_object, &channels)) return NULL;
    if (channels < 1 || channels > 2) {
        PyErr_SetString(PyExc_ValueError,
            "the cutoff input's channel_count must be 1 or 2");
        return NULL;
    }
    Py_buffer input = {0};
    Py_buffer cutoff = {0};
    PyObject *result = NULL;
    const Py_ssize_t width = 2 * (Py_ssize_t)self->config.channel_count;
    Py_ssize_t frames = 0;
    if (PyObject_GetBuffer(input_object, &input, PyBUF_SIMPLE) < 0) {
        return NULL;
    }
    if (PyObject_GetBuffer(cutoff_object, &cutoff, PyBUF_SIMPLE) < 0) {
        PyBuffer_Release(&input);
        return NULL;
    }
    if (input.len % width) {
        PyErr_SetString(PyExc_ValueError,
            "input must be whole 16-bit frames for the configured channel count");
        goto done;
    }
    frames = input.len / width;
    if (cutoff.len < frames * 2 * (Py_ssize_t)channels) {
        PyErr_SetString(PyExc_ValueError,
            "the cutoff input is shorter than the frames given");
        goto done;
    }
    result = PyBytes_FromStringAndSize(NULL, input.len);
    if (result != NULL) {
        audiodsp_ladder_process_cutoff_s16(&self->config, &self->state,
            (int16_t *)PyBytes_AS_STRING(result), (const int16_t *)input.buf,
            (uint32_t)frames, (const int16_t *)cutoff.buf, channels);
    }
done:
    PyBuffer_Release(&cutoff);
    PyBuffer_Release(&input);
    return result;
}

AUDIODSP_LOCKED_ARGS(ladder_state_configure, audiodsp_ladder_object_t)
AUDIODSP_LOCKED_ARGS(ladder_state_finish, audiodsp_ladder_object_t)
AUDIODSP_LOCKED_ARGS(ladder_state_reset, audiodsp_ladder_object_t)
AUDIODSP_LOCKED_ARGS(ladder_state_process, audiodsp_ladder_object_t)
AUDIODSP_LOCKED_ARGS(ladder_state_process_cutoff, audiodsp_ladder_object_t)
static PyMethodDef ladder_state_methods[] = {
    {"configure", (PyCFunction)ladder_state_configure_locked, METH_VARARGS, NULL},
    {"finish", (PyCFunction)ladder_state_finish_locked, METH_NOARGS, NULL},
    {"reset", (PyCFunction)ladder_state_reset_locked, METH_NOARGS, NULL},
    {"process", (PyCFunction)ladder_state_process_locked, METH_O, NULL},
    {"process_cutoff", (PyCFunction)ladder_state_process_cutoff_locked,
        METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_INIT(ladder_state_init, audiodsp_ladder_object_t)
static PyType_Slot ladder_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, ladder_state_init_locked},
    {Py_tp_methods, ladder_state_methods},
    {0, NULL},
};

static PyType_Spec ladder_state_spec = {
    .name = "_audiodsp.LadderState",
    .basicsize = sizeof(audiodsp_ladder_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = ladder_state_slots,
};

// audiooscillator.Oscillator's phase and the half-finished sample its
// band-limiting kernel is still adding to. The pulling loop over the four
// inputs is the Python wrapper's; this renders one run of frames from the
// inputs it is handed.

typedef struct {
    PyObject_HEAD
    audiodsp_oscillator_config_t config;
    audiodsp_oscillator_state_t state;
} audiodsp_oscillator_object_t;

static int oscillator_state_init(audiodsp_oscillator_object_t *self,
    PyObject *args, PyObject *kwargs) {
    unsigned int sample_rate = 48000;
    unsigned int channel_count = 1;
    static char *keywords[] = {"sample_rate", "channel_count", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|II:OscillatorState",
        keywords, &sample_rate, &channel_count)) return -1;
    if (sample_rate < 1) {
        PyErr_SetString(PyExc_ValueError, "sample_rate must be at least 1");
        return -1;
    }
    if (channel_count < 1 || channel_count > 2) {
        PyErr_SetString(PyExc_ValueError, "channel_count must be 1 or 2");
        return -1;
    }
    audiodsp_oscillator_config_init(&self->config, sample_rate);
    audiodsp_oscillator_set_channel_count(&self->config, channel_count);
    audiodsp_oscillator_config_finish(&self->config);
    audiodsp_oscillator_state_init(&self->state);
    return 0;
}

static PyObject *oscillator_state_configure(
    audiodsp_oscillator_object_t *self, PyObject *args) {
    int option;
    double value;
    if (!PyArg_ParseTuple(args, "id:configure", &option, &value)) return NULL;
    if (option < AUDIODSP_OSCILLATOR_OPT_FREQUENCY ||
        option > AUDIODSP_OSCILLATOR_OPT_PWM_DEPTH) {
        PyErr_SetString(PyExc_ValueError, "unknown oscillator option");
        return NULL;
    }
    audiodsp_oscillator_configure(&self->config,
        (audiodsp_oscillator_option_t)option, (float)value);
    Py_RETURN_NONE;
}

static PyObject *oscillator_state_finish(audiodsp_oscillator_object_t *self,
    PyObject *unused) {
    audiodsp_oscillator_config_finish(&self->config);
    Py_RETURN_NONE;
}

static PyObject *oscillator_state_reset(audiodsp_oscillator_object_t *self,
    PyObject *unused) {
    audiodsp_oscillator_reset(&self->state);
    Py_RETURN_NONE;
}

// process(frames, inputs): `inputs` is four entries, pitch, fm, pwm and sync,
// each None or a (buffer, channel_count) pair holding at least `frames`
// frames. Returns the rendered frames as bytes.
static PyObject *oscillator_state_process(audiodsp_oscillator_object_t *self,
    PyObject *args) {
    unsigned int frames;
    PyObject *inputs;
    if (!PyArg_ParseTuple(args, "IO:process", &frames, &inputs)) return NULL;
    if (!PyTuple_Check(inputs) ||
        PyTuple_GET_SIZE(inputs) != AUDIODSP_OSCILLATOR_INPUTS) {
        PyErr_SetString(PyExc_TypeError, "inputs must be a 4-tuple");
        return NULL;
    }
    Py_buffer views[AUDIODSP_OSCILLATOR_INPUTS];
    const int16_t *pointers[AUDIODSP_OSCILLATOR_INPUTS];
    uint32_t strides[AUDIODSP_OSCILLATOR_INPUTS];
    PyObject *result = NULL;
    for (int index = 0; index < AUDIODSP_OSCILLATOR_INPUTS; ++index) {
        pointers[index] = NULL;
        strides[index] = 1;
        views[index].obj = NULL;
    }
    for (int held = 0; held < AUDIODSP_OSCILLATOR_INPUTS; ++held) {
        PyObject *entry = PyTuple_GET_ITEM(inputs, held);
        if (entry == Py_None) {
            continue;
        }
        PyObject *buffer;
        unsigned int channels;
        if (!PyArg_ParseTuple(entry, "OI:input", &buffer, &channels)) {
            goto done;
        }
        if (channels < 1 || channels > 2) {
            PyErr_SetString(PyExc_ValueError,
                "an input's channel_count must be 1 or 2");
            goto done;
        }
        if (PyObject_GetBuffer(buffer, &views[held], PyBUF_SIMPLE) < 0) {
            views[held].obj = NULL;
            goto done;
        }
        if ((size_t)views[held].len < (size_t)frames * 2u * channels) {
            PyErr_SetString(PyExc_ValueError,
                "an input is shorter than the frames asked for");
            goto done;
        }
        pointers[held] = (const int16_t *)views[held].buf;
        strides[held] = channels;
    }
    result = PyBytes_FromStringAndSize(NULL,
        (Py_ssize_t)frames * 2 * (Py_ssize_t)self->config.channel_count);
    if (result != NULL) {
        audiodsp_oscillator_process_s16(&self->config, &self->state,
            (int16_t *)PyBytes_AS_STRING(result), frames, pointers, strides);
    }
done:
    for (int index = 0; index < AUDIODSP_OSCILLATOR_INPUTS; ++index) {
        if (views[index].obj != NULL) {
            PyBuffer_Release(&views[index]);
        }
    }
    return result;
}

AUDIODSP_LOCKED_ARGS(oscillator_state_configure, audiodsp_oscillator_object_t)
AUDIODSP_LOCKED_ARGS(oscillator_state_finish, audiodsp_oscillator_object_t)
AUDIODSP_LOCKED_ARGS(oscillator_state_reset, audiodsp_oscillator_object_t)
AUDIODSP_LOCKED_ARGS(oscillator_state_process, audiodsp_oscillator_object_t)
static PyMethodDef oscillator_state_methods[] = {
    {"configure", (PyCFunction)oscillator_state_configure_locked, METH_VARARGS, NULL},
    {"finish", (PyCFunction)oscillator_state_finish_locked, METH_NOARGS, NULL},
    {"reset", (PyCFunction)oscillator_state_reset_locked, METH_NOARGS, NULL},
    {"process", (PyCFunction)oscillator_state_process_locked, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_INIT(oscillator_state_init, audiodsp_oscillator_object_t)
static PyType_Slot oscillator_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, oscillator_state_init_locked},
    {Py_tp_methods, oscillator_state_methods},
    {0, NULL},
};

static PyType_Spec oscillator_state_spec = {
    .name = "_audiodsp.OscillatorState",
    .basicsize = sizeof(audiodsp_oscillator_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = oscillator_state_slots,
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
    // A tuple, not PySequence_Fast: a list's items would be borrowed, and
    // PyFloat_AsDouble can run an item's __float__, which may change the
    // list (or another thread may) and free the item being read.
    PyObject *fast = PySequence_Tuple(sequence);
    if (fast == NULL) {
        if (PyErr_ExceptionMatches(PyExc_TypeError)) {
            PyErr_SetString(PyExc_TypeError, "expected a sequence of numbers");
        }
        return -1;
    }
    const Py_ssize_t length = PyTuple_GET_SIZE(fast);
    if ((uint32_t)length > limit) {
        Py_DECREF(fast);
        PyErr_SetString(PyExc_ValueError, "too many values");
        return -1;
    }
    for (Py_ssize_t index = 0; index < length; ++index) {
        const double value =
            PyFloat_AsDouble(PyTuple_GET_ITEM(fast, index));
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

// A re-cut of a running node (audiodsp#169): the new tables checked on a
// copy, then the network rebuilt empty on them, exactly as tank_state_init
// leaves a new one. `delays` or `taps` may be None to keep that table.
static PyObject *tank_state_recut(audiodsp_tank_object_t *self,
    PyObject *args) {
    PyObject *delays = Py_None;
    PyObject *taps = Py_None;
    if (!PyArg_ParseTuple(args, "OO:recut", &delays, &taps)) return NULL;
    uint32_t frames[AUDIODSP_TANK_LINES];
    uint32_t frame_count = 0;
    float tap_values[AUDIODSP_TANK_MAX_TAPS * 4u];
    uint32_t tap_count = 0;
    if (delays != Py_None) {
        float values[AUDIODSP_TANK_LINES];
        if (tank_state_floats(delays, values, AUDIODSP_TANK_LINES,
            &frame_count) < 0) return NULL;
        for (uint32_t line = 0; line < frame_count; ++line) {
            frames[line] = values[line] < 0.0f ? 0u : (uint32_t)values[line];
        }
    }
    if (taps != Py_None) {
        if (tank_state_floats(taps, tap_values, AUDIODSP_TANK_MAX_TAPS * 4u,
            &tap_count) < 0) return NULL;
    }
    audiodsp_tank_config_t recut;
    if (tank_state_status(audiodsp_tank_recut(&recut, &self->config,
        delays != Py_None ? frames : NULL, frame_count,
        taps != Py_None ? tap_values : NULL, tap_count)) < 0) return NULL;
    const uint32_t samples = audiodsp_tank_buffer_samples(&recut);
    int16_t *lines = self->lines;
    if (samples != audiodsp_tank_buffer_samples(&self->config)) {
        lines = PyMem_Calloc((size_t)samples, sizeof(int16_t));
        if (lines == NULL) return PyErr_NoMemory();
        PyMem_Free(self->lines);
        self->lines = lines;
    } else {
        memset(lines, 0, (size_t)samples * sizeof(int16_t));
    }
    self->config = recut;
    audiodsp_tank_state_init(&self->state, &self->config, lines);
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

// `frames` of output from silence: the node's source has ended or run dry,
// so its tail rings out, and once it has ended the node rests (audiodsp#180).
static PyObject *tank_state_process_silence(
    audiodsp_tank_object_t *self, PyObject *argument) {
    const Py_ssize_t frames = PyLong_AsSsize_t(argument);
    if (frames == -1 && PyErr_Occurred()) return NULL;
    if (frames < 0) {
        PyErr_SetString(PyExc_ValueError, "frames must not be negative");
        return NULL;
    }
    const Py_ssize_t width = 2 * (Py_ssize_t)(self->config.channel_count == 1u ? 1u : 2u);
    PyObject *result = PyBytes_FromStringAndSize(NULL, frames * width);
    if (result != NULL) {
        audiodsp_tank_process_silence(&self->config, &self->state,
            (int16_t *)PyBytes_AS_STRING(result), (uint32_t)frames);
    }
    return result;
}

AUDIODSP_LOCKED_ARGS(tank_state_configure, audiodsp_tank_object_t)
AUDIODSP_LOCKED_ARGS(tank_state_finish, audiodsp_tank_object_t)
AUDIODSP_LOCKED_ARGS(tank_state_reset, audiodsp_tank_object_t)
AUDIODSP_LOCKED_ARGS(tank_state_recut, audiodsp_tank_object_t)
AUDIODSP_LOCKED_ARGS(tank_state_process, audiodsp_tank_object_t)
AUDIODSP_LOCKED_ARGS(tank_state_process_silence, audiodsp_tank_object_t)
static PyMethodDef tank_state_methods[] = {
    {"configure", (PyCFunction)tank_state_configure_locked, METH_VARARGS, NULL},
    {"finish", (PyCFunction)tank_state_finish_locked, METH_NOARGS, NULL},
    {"reset", (PyCFunction)tank_state_reset_locked, METH_NOARGS, NULL},
    {"recut", (PyCFunction)tank_state_recut_locked, METH_VARARGS, NULL},
    {"process", (PyCFunction)tank_state_process_locked, METH_O, NULL},
    {"process_silence", (PyCFunction)tank_state_process_silence_locked, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_INIT(tank_state_init, audiodsp_tank_object_t)
static PyType_Slot tank_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, tank_state_init_locked},
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

AUDIODSP_LOCKED_ARGS(modal_state_configure, audiodsp_modal_object_t)
AUDIODSP_LOCKED_ARGS(modal_state_set_mode, audiodsp_modal_object_t)
AUDIODSP_LOCKED_ARGS(modal_state_finish, audiodsp_modal_object_t)
AUDIODSP_LOCKED_ARGS(modal_state_reset, audiodsp_modal_object_t)
AUDIODSP_LOCKED_ARGS(modal_state_silent, audiodsp_modal_object_t)
AUDIODSP_LOCKED_ARGS(modal_state_process, audiodsp_modal_object_t)
static PyMethodDef modal_state_methods[] = {
    {"configure", (PyCFunction)modal_state_configure_locked, METH_VARARGS, NULL},
    {"set_mode", (PyCFunction)modal_state_set_mode_locked, METH_VARARGS, NULL},
    {"finish", (PyCFunction)modal_state_finish_locked, METH_NOARGS, NULL},
    {"reset", (PyCFunction)modal_state_reset_locked, METH_NOARGS, NULL},
    {"silent", (PyCFunction)modal_state_silent_locked, METH_NOARGS, NULL},
    {"process", (PyCFunction)modal_state_process_locked, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_INIT(modal_state_init, audiodsp_modal_object_t)
static PyType_Slot modal_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, modal_state_init_locked},
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

// audiosampler.Sampler's voice table, mix accumulator and the buffer each
// voice is reading. The kernel borrows a pointer into each buffer; this
// object holds the Py_buffer that keeps the exporter alive and unresized for
// as long as the voice sounds, and lets go of it when the voice stops --
// whether Python stopped it or the sample ran out, which `render` reports.
// `buffers` and `held` have a slot per kernel slot: voice `i` at `i`, and
// the tail fading out voice `i`'s last note at `voice_count + i`.
typedef struct {
    PyObject_HEAD
    audiodsp_sampler_t sampler;
    audiodsp_sampler_voice_t *voices;
    float *mix;
    int16_t *out;
    Py_buffer *buffers;
    char *held;
} audiodsp_sampler_object_t;

static void sampler_state_drop(audiodsp_sampler_object_t *self, uint32_t index) {
    if (self->held != NULL && self->held[index]) {
        self->held[index] = 0;
        PyBuffer_Release(&self->buffers[index]);
    }
}

static void sampler_state_drop_all(audiodsp_sampler_object_t *self) {
    const uint32_t slots = AUDIODSP_SAMPLER_SLOTS(self->sampler.voice_count);
    for (uint32_t i = 0; self->voices != NULL && i < slots; ++i) {
        sampler_state_drop(self, i);
    }
}

// The kernel has just handed voice `index`'s note to its tail: the hold on
// the buffer goes with it.
static void sampler_state_hand(audiodsp_sampler_object_t *self, uint32_t index) {
    const uint32_t tail = self->sampler.voice_count + index;
    sampler_state_drop(self, tail);
    if (self->held[index]) {
        self->buffers[tail] = self->buffers[index];
        self->held[tail] = 1;
        self->held[index] = 0;
    }
}

static int sampler_state_init(audiodsp_sampler_object_t *self, PyObject *args,
    PyObject *kwargs) {
    static char *keywords[] = {"sample_rate", "channel_count", "frames",
                               "voice_count", NULL};
    unsigned int sample_rate = 48000;
    unsigned int channel_count = 2;
    unsigned int frames = AUDIODSP_SAMPLER_FRAMES;
    unsigned int voice_count = 8;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|IIII:SamplerState",
        keywords, &sample_rate, &channel_count, &frames, &voice_count)) {
        return -1;
    }
    if (sample_rate < 1u) {
        PyErr_SetString(PyExc_ValueError, "sample_rate must be at least 1");
        return -1;
    }
    if (channel_count < 1u || channel_count > 2u) {
        PyErr_SetString(PyExc_ValueError, "channel_count must be 1 or 2");
        return -1;
    }
    if (frames < 1u || frames > AUDIODSP_SAMPLER_MAX_FRAMES) {
        PyErr_SetString(PyExc_ValueError, "frames out of range");
        return -1;
    }
    if (voice_count < 1u || voice_count > AUDIODSP_SAMPLER_MAX_VOICES) {
        PyErr_SetString(PyExc_ValueError, "voice_count out of range");
        return -1;
    }
    sampler_state_drop_all(self);
    PyMem_Free(self->voices);
    PyMem_Free(self->mix);
    PyMem_Free(self->out);
    PyMem_Free(self->buffers);
    PyMem_Free(self->held);
    const uint32_t slots = AUDIODSP_SAMPLER_SLOTS(voice_count);
    self->voices = PyMem_Calloc(slots, sizeof(audiodsp_sampler_voice_t));
    self->mix = PyMem_Calloc((size_t)frames * channel_count, sizeof(float));
    self->out = PyMem_Calloc((size_t)frames * channel_count, sizeof(int16_t));
    self->buffers = PyMem_Calloc(slots, sizeof(Py_buffer));
    self->held = PyMem_Calloc(slots, 1);
    if (self->voices == NULL || self->mix == NULL || self->out == NULL ||
        self->buffers == NULL || self->held == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    audiodsp_sampler_init(&self->sampler, sample_rate, channel_count, frames,
        voice_count, self->voices, self->mix);
    return 0;
}

static void sampler_state_dealloc(audiodsp_sampler_object_t *self) {
    PyTypeObject *type = Py_TYPE(self);
    sampler_state_drop_all(self);
    PyMem_Free(self->voices);
    PyMem_Free(self->mix);
    PyMem_Free(self->out);
    PyMem_Free(self->buffers);
    PyMem_Free(self->held);
    self->voices = NULL;
    self->mix = NULL;
    self->out = NULL;
    self->buffers = NULL;
    self->held = NULL;
    type->tp_free((PyObject *)self);
    Py_DECREF(type);
}

static int sampler_state_voice(audiodsp_sampler_object_t *self, PyObject *arg,
    uint32_t *index) {
    unsigned long value = PyLong_AsUnsignedLong(arg);
    if (value == (unsigned long)-1 && PyErr_Occurred()) {
        return -1;
    }
    if (value >= self->sampler.voice_count) {
        PyErr_SetString(PyExc_IndexError, "voice index out of range");
        return -1;
    }
    *index = (uint32_t)value;
    return 0;
}

static PyObject *sampler_state_play(audiodsp_sampler_object_t *self,
    PyObject *args, PyObject *kwargs) {
    static char *keywords[] = {"index", "buffer", "rate", "pitch",
                               "loop_start", "loop_end", "loop_mode", "level",
                               "pan", "delay", "attack", "hold", "decay",
                               "sustain", "release",
                               "cutoff",
                               "resonance",
                               "mod_delay",
                               "mod_attack",
                               "mod_hold",
                               "mod_decay",
                               "mod_sustain",
                               "mod_release",
                               "mod_to_pitch",
                               "mod_to_cutoff",
                               "vibrato_delay",
                               "vibrato_rate",
                               "vibrato_depth",
                               "lfo_delay",
                               "lfo_rate",
                               "lfo_to_pitch",
                               "lfo_to_cutoff",
                               "lfo_to_volume", NULL};
    PyObject *index_obj;
    PyObject *buffer_obj;
    audiodsp_sampler_note_t note;
    audiodsp_sampler_note_init(&note);
    double rate = 0.0, pitch = 1.0, level = 1.0, pan = 0.0;
    double delay = 0.0, attack = 0.0, hold = 0.0, decay = 0.0, sustain = 1.0,
           release = 0.0;
    unsigned long loop_start = 0, loop_end = 0, loop_mode = 0;
    double mod[18] = {0.0};
    mod[6] = 1.0;  // mod_sustain
    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
        "OO|ddkkkdddddddddddddddddddddddddd:play",
        keywords, &index_obj, &buffer_obj, &rate, &pitch, &loop_start,
        &loop_end, &loop_mode, &level, &pan, &delay, &attack, &hold, &decay,
        &sustain, &release, &mod[0], &mod[1], &mod[2], &mod[3], &mod[4], &mod[5], &mod[6], &mod[7], &mod[8], &mod[9], &mod[10], &mod[11], &mod[12], &mod[13], &mod[14], &mod[15], &mod[16], &mod[17])) {
        return NULL;
    }
    uint32_t index;
    if (sampler_state_voice(self, index_obj, &index) < 0) {
        return NULL;
    }
    if (loop_start > UINT32_MAX || loop_end > UINT32_MAX) {
        PyErr_SetString(PyExc_ValueError, "loop bounds out of range");
        return NULL;
    }
    Py_buffer view = {0};
    if (PyObject_GetBuffer(buffer_obj, &view, PyBUF_SIMPLE) < 0) {
        return NULL;
    }
    if (view.len < 2 || view.len % 2) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError,
            "buffer must hold at least one whole 16-bit sample");
        return NULL;
    }
    note.rate = (float)rate;
    note.pitch = (float)pitch;
    note.loop_start = (uint32_t)loop_start;
    note.loop_end = (uint32_t)loop_end;
    note.loop_mode = (uint32_t)loop_mode;
    note.level = (float)level;
    note.pan = (float)pan;
    note.delay = (float)delay;
    note.attack = (float)attack;
    note.hold = (float)hold;
    note.decay = (float)decay;
    note.sustain = (float)sustain;
    note.release = (float)release;
    note.cutoff = (float)mod[0];
    note.resonance = (float)mod[1];
    note.mod_delay = (float)mod[2];
    note.mod_attack = (float)mod[3];
    note.mod_hold = (float)mod[4];
    note.mod_decay = (float)mod[5];
    note.mod_sustain = (float)mod[6];
    note.mod_release = (float)mod[7];
    note.mod_to_pitch = (float)mod[8];
    note.mod_to_cutoff = (float)mod[9];
    note.vibrato_delay = (float)mod[10];
    note.vibrato_rate = (float)mod[11];
    note.vibrato_depth = (float)mod[12];
    note.lfo_delay = (float)mod[13];
    note.lfo_rate = (float)mod[14];
    note.lfo_to_pitch = (float)mod[15];
    note.lfo_to_cutoff = (float)mod[16];
    note.lfo_to_volume = (float)mod[17];
    // Let go of what this voice was reading only once the new buffer is
    // known to be good, so a refused play() leaves the voice as it was.
    // The kernel leaves the voice and its tail untouched on a refusal.
    bool handed = audiodsp_sampler_audible(&self->sampler, index);
    if (!audiodsp_sampler_play(&self->sampler, index, (const int16_t *)view.buf,
        (uint32_t)(view.len / 2), &note)) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError,
            "loop_mode must be 0, 1 or 3 and the loop must fit the buffer");
        return NULL;
    }
    if (handed) {
        sampler_state_hand(self, index);
    }
    sampler_state_drop(self, index);
    self->buffers[index] = view;
    self->held[index] = 1;
    Py_RETURN_NONE;
}

static PyObject *sampler_state_release(audiodsp_sampler_object_t *self,
    PyObject *arg) {
    uint32_t index;
    if (sampler_state_voice(self, arg, &index) < 0) return NULL;
    audiodsp_sampler_release(&self->sampler, index);
    if (!audiodsp_sampler_playing(&self->sampler, index)) {
        sampler_state_drop(self, index);
    }
    Py_RETURN_NONE;
}

static PyObject *sampler_state_stop(audiodsp_sampler_object_t *self,
    PyObject *arg) {
    uint32_t index;
    if (sampler_state_voice(self, arg, &index) < 0) return NULL;
    if (audiodsp_sampler_audible(&self->sampler, index)) {
        sampler_state_hand(self, index);
    }
    audiodsp_sampler_stop(&self->sampler, index);
    sampler_state_drop(self, index);
    Py_RETURN_NONE;
}

// No fade: the voice and its tail are silent at once and both buffers are
// let go, for deinit().
static PyObject *sampler_state_cut(audiodsp_sampler_object_t *self,
    PyObject *arg) {
    uint32_t index;
    if (sampler_state_voice(self, arg, &index) < 0) return NULL;
    audiodsp_sampler_cut(&self->sampler, index);
    sampler_state_drop(self, index);
    sampler_state_drop(self, self->sampler.voice_count + index);
    Py_RETURN_NONE;
}

static PyObject *sampler_state_playing(audiodsp_sampler_object_t *self,
    PyObject *arg) {
    uint32_t index;
    if (sampler_state_voice(self, arg, &index) < 0) return NULL;
    return PyBool_FromLong(audiodsp_sampler_playing(&self->sampler, index));
}

static PyObject *sampler_state_set_level(audiodsp_sampler_object_t *self,
    PyObject *args) {
    PyObject *index_obj;
    double value;
    uint32_t index;
    if (!PyArg_ParseTuple(args, "Od:set_level", &index_obj, &value)) return NULL;
    if (sampler_state_voice(self, index_obj, &index) < 0) return NULL;
    audiodsp_sampler_set_level(&self->sampler, index, (float)value);
    Py_RETURN_NONE;
}

static PyObject *sampler_state_set_pan(audiodsp_sampler_object_t *self,
    PyObject *args) {
    PyObject *index_obj;
    double value;
    uint32_t index;
    if (!PyArg_ParseTuple(args, "Od:set_pan", &index_obj, &value)) return NULL;
    if (sampler_state_voice(self, index_obj, &index) < 0) return NULL;
    audiodsp_sampler_set_pan(&self->sampler, index, (float)value);
    Py_RETURN_NONE;
}

static PyObject *sampler_state_set_pitch(audiodsp_sampler_object_t *self,
    PyObject *args) {
    PyObject *index_obj;
    double value;
    uint32_t index;
    if (!PyArg_ParseTuple(args, "Od:set_pitch", &index_obj, &value)) return NULL;
    if (sampler_state_voice(self, index_obj, &index) < 0) return NULL;
    audiodsp_sampler_set_pitch(&self->sampler, index, (float)value);
    Py_RETURN_NONE;
}

static PyObject *sampler_state_set_cutoff(audiodsp_sampler_object_t *self,
    PyObject *args) {
    PyObject *index_obj;
    double value;
    if (!PyArg_ParseTuple(args, "Od:set_cutoff", &index_obj, &value)) return NULL;
    uint32_t index;
    if (sampler_state_voice(self, index_obj, &index) < 0) return NULL;
    audiodsp_sampler_set_cutoff(&self->sampler, index, (float)value);
    Py_RETURN_NONE;
}

static PyObject *sampler_state_set_vibrato_depth(
    audiodsp_sampler_object_t *self, PyObject *args) {
    PyObject *index_obj;
    double value;
    if (!PyArg_ParseTuple(args, "Od:set_vibrato_depth", &index_obj, &value)) {
        return NULL;
    }
    uint32_t index;
    if (sampler_state_voice(self, index_obj, &index) < 0) return NULL;
    audiodsp_sampler_set_vibrato_depth(&self->sampler, index, (float)value);
    Py_RETURN_NONE;
}

static PyObject *sampler_state_sounding(audiodsp_sampler_object_t *self,
    PyObject *unused) {
    uint32_t count = 0;
    // A tail still fading counts: it is sound the output has yet to play.
    for (uint32_t i = 0; i < self->sampler.voice_count; ++i) {
        count += audiodsp_sampler_playing(&self->sampler, i) ? 1u : 0u;
        count += audiodsp_sampler_fading(&self->sampler, i) ? 1u : 0u;
    }
    return PyLong_FromUnsignedLong(count);
}

static PyObject *sampler_state_render(audiodsp_sampler_object_t *self,
    PyObject *unused) {
    audiodsp_sampler_render_s16(&self->sampler, self->out);
    for (uint32_t i = 0; i < self->sampler.voice_count; ++i) {
        if (!audiodsp_sampler_playing(&self->sampler, i)) {
            sampler_state_drop(self, i);
        }
        if (!audiodsp_sampler_fading(&self->sampler, i)) {
            sampler_state_drop(self, self->sampler.voice_count + i);
        }
    }
    return PyBytes_FromStringAndSize((const char *)self->out,
        (Py_ssize_t)self->sampler.frames * self->sampler.channel_count * 2);
}

AUDIODSP_LOCKED_KW(sampler_state_play, audiodsp_sampler_object_t)
AUDIODSP_LOCKED_ARGS(sampler_state_release, audiodsp_sampler_object_t)
AUDIODSP_LOCKED_ARGS(sampler_state_stop, audiodsp_sampler_object_t)
AUDIODSP_LOCKED_ARGS(sampler_state_cut, audiodsp_sampler_object_t)
AUDIODSP_LOCKED_ARGS(sampler_state_playing, audiodsp_sampler_object_t)
AUDIODSP_LOCKED_ARGS(sampler_state_set_level, audiodsp_sampler_object_t)
AUDIODSP_LOCKED_ARGS(sampler_state_set_pan, audiodsp_sampler_object_t)
AUDIODSP_LOCKED_ARGS(sampler_state_set_pitch, audiodsp_sampler_object_t)
AUDIODSP_LOCKED_ARGS(sampler_state_set_cutoff, audiodsp_sampler_object_t)
AUDIODSP_LOCKED_ARGS(sampler_state_set_vibrato_depth, audiodsp_sampler_object_t)
AUDIODSP_LOCKED_ARGS(sampler_state_sounding, audiodsp_sampler_object_t)
AUDIODSP_LOCKED_ARGS(sampler_state_render, audiodsp_sampler_object_t)
static PyMethodDef sampler_state_methods[] = {
    {"play", (PyCFunction)sampler_state_play_locked, METH_VARARGS | METH_KEYWORDS, NULL},
    {"release", (PyCFunction)sampler_state_release_locked, METH_O, NULL},
    {"stop", (PyCFunction)sampler_state_stop_locked, METH_O, NULL},
    {"cut", (PyCFunction)sampler_state_cut_locked, METH_O, NULL},
    {"playing", (PyCFunction)sampler_state_playing_locked, METH_O, NULL},
    {"set_level", (PyCFunction)sampler_state_set_level_locked, METH_VARARGS, NULL},
    {"set_pan", (PyCFunction)sampler_state_set_pan_locked, METH_VARARGS, NULL},
    {"set_pitch", (PyCFunction)sampler_state_set_pitch_locked, METH_VARARGS, NULL},
    {"set_cutoff", (PyCFunction)sampler_state_set_cutoff_locked, METH_VARARGS, NULL},
    {"set_vibrato_depth", (PyCFunction)sampler_state_set_vibrato_depth_locked, METH_VARARGS, NULL},
    {"sounding", (PyCFunction)sampler_state_sounding_locked, METH_NOARGS, NULL},
    {"render", (PyCFunction)sampler_state_render_locked, METH_NOARGS, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_INIT(sampler_state_init, audiodsp_sampler_object_t)
static PyType_Slot sampler_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, sampler_state_init_locked},
    {Py_tp_dealloc, sampler_state_dealloc},
    {Py_tp_methods, sampler_state_methods},
    {0, NULL},
};

static PyType_Spec sampler_state_spec = {
    .name = "_audiodsp.SamplerState",
    .basicsize = sizeof(audiodsp_sampler_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = sampler_state_slots,
};

// audioconvolve.Convolver's transform tables, frequency-delay line and stored
// impulse. All state, and a great deal of it, in two allocations carved up
// by the DSP layer: the history in one and the impulse in another, the
// layout the MicroPython binding uses, so that load() and synthesize() can
// build a new impulse into a buffer of their own and swap it in with
// audiodsp_convolve_install rather than write the node's own in place.
// There is no pump on another thread here to need that, and a desktop has
// the memory to do it in place; it is done this way so the desktop tests
// run the same build-and-install path a board runs, with the crossfade of
// the block in flight going through the same install.

typedef struct {
    PyObject_HEAD
    audiodsp_convolve_config_t config;
    audiodsp_convolve_state_t state;
    float *storage;
    float *impulse;
    // room_peak_gain: read by the last synthesize(), 0 after a load().
    float peak_gain;
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
        audiodsp_convolve_float_count_apart(&self->config), sizeof(float));
    float *impulse = storage == NULL ? NULL : PyMem_Calloc(
        audiodsp_convolve_impulse_floats(&self->config), sizeof(float));
    if (impulse == NULL) {
        PyMem_Free(storage);
        PyErr_NoMemory();
        return -1;
    }
    PyMem_Free(self->storage);
    PyMem_Free(self->impulse);
    self->storage = storage;
    self->impulse = impulse;
    audiodsp_convolve_state_init_apart(&self->state, &self->config, storage,
        impulse);
    return 0;
}

static void convolver_state_dealloc(audiodsp_convolver_object_t *self) {
    PyTypeObject *type = Py_TYPE(self);
    PyMem_Free(self->storage);
    PyMem_Free(self->impulse);
    self->storage = NULL;
    self->impulse = NULL;
    type->tp_free((PyObject *)self);
    Py_DECREF(type);
}

// A new impulse for load() and synthesize(): built into an allocation of
// the call's own with the node's transform tables, then installed, which is
// where the node keeps or resets its audio in flight. `work` is the build's
// block and scratch, two transforms long.
typedef struct {
    float *impulse;
    float *work;
} convolver_build_t;

static int convolver_build_begin(audiodsp_convolver_object_t *self,
    convolver_build_t *build) {
    build->impulse = PyMem_Malloc(
        audiodsp_convolve_impulse_floats(&self->config) * sizeof(float));
    build->work = build->impulse == NULL ? NULL
        : PyMem_Malloc(2u * AUDIODSP_CONVOLVE_FFT * sizeof(float));
    if (build->work == NULL) {
        PyMem_Free(build->impulse);
        PyErr_NoMemory();
        return -1;
    }
    return 0;
}

static void convolver_build_end(audiodsp_convolver_object_t *self,
    convolver_build_t *build, uint32_t loaded) {
    float *previous = audiodsp_convolve_install(&self->state, &self->config,
        build->impulse, loaded);
    self->impulse = build->impulse;
    PyMem_Free(previous);
    PyMem_Free(build->work);
}

static PyObject *convolver_state_configure(audiodsp_convolver_object_t *self,
    PyObject *args) {
    int option;
    double value;
    if (!PyArg_ParseTuple(args, "id:configure", &option, &value)) return NULL;
    if (option != AUDIODSP_CONVOLVE_OPT_MIX
            && option != AUDIODSP_CONVOLVE_OPT_WET_GAIN) {
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
    convolver_build_t build;
    if (convolver_build_begin(self, &build) < 0) {
        PyBuffer_Release(&taps);
        return NULL;
    }
    uint32_t loaded = audiodsp_convolve_build_s16(&self->state,
        &self->config, build.impulse, build.work,
        build.work + AUDIODSP_CONVOLVE_FFT, (const int16_t *)taps.buf,
        frames, channels, (float)gain);
    convolver_build_end(self, &build, loaded);
    self->peak_gain = 0.0f;
    PyBuffer_Release(&taps);
    Py_RETURN_NONE;
}

static PyObject *convolver_state_synthesize(audiodsp_convolver_object_t *self,
    PyObject *args) {
    double decay, damping, predelay, diffusion;
    unsigned int seed;
    if (!PyArg_ParseTuple(args, "ddddI:synthesize", &decay, &damping,
        &predelay, &diffusion, &seed)) return NULL;
    float *fold = PyMem_Malloc(
        audiodsp_convolve_peak_floats(&self->config) * sizeof(float));
    if (fold == NULL) return PyErr_NoMemory();
    convolver_build_t build;
    if (convolver_build_begin(self, &build) < 0) {
        PyMem_Free(fold);
        return NULL;
    }
    float peak = 0.0f;
    uint32_t loaded = audiodsp_convolve_build_room_peak(&self->state,
        &self->config, build.impulse, build.work,
        build.work + AUDIODSP_CONVOLVE_FFT, (float)decay, (float)damping,
        (float)predelay, (float)diffusion, seed, fold, &peak);
    PyMem_Free(fold);
    convolver_build_end(self, &build, loaded);
    self->peak_gain = peak;
    Py_RETURN_NONE;
}

static PyObject *convolver_state_room_peak(audiodsp_convolver_object_t *self,
    PyObject *args) {
    double decay, damping, predelay, diffusion;
    unsigned int seed;
    if (!PyArg_ParseTuple(args, "ddddI:room_peak", &decay, &damping,
        &predelay, &diffusion, &seed)) return NULL;
    const uint32_t fold_floats = audiodsp_convolve_peak_floats(&self->config);
    float *work = PyMem_Malloc(
        (fold_floats + AUDIODSP_CONVOLVE_FFT) * sizeof(float));
    if (work == NULL) return PyErr_NoMemory();
    float peak = audiodsp_convolve_room_peak(&self->config,
        work + fold_floats, (float)decay, (float)damping, (float)predelay,
        (float)diffusion, seed, work);
    PyMem_Free(work);
    return PyFloat_FromDouble((double)peak);
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

static PyObject *convolver_state_peak_gain(audiodsp_convolver_object_t *self,
    PyObject *unused) {
    return PyFloat_FromDouble((double)self->peak_gain);
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

// `frames` of output from silence: the node's source has ended or run dry,
// so its tail rings out, and once it has ended the node rests (audiodsp#180).
static PyObject *convolver_state_process_silence(
    audiodsp_convolver_object_t *self, PyObject *argument) {
    const Py_ssize_t frames = PyLong_AsSsize_t(argument);
    if (frames == -1 && PyErr_Occurred()) return NULL;
    if (frames < 0) {
        PyErr_SetString(PyExc_ValueError, "frames must not be negative");
        return NULL;
    }
    const Py_ssize_t width = 2 * (Py_ssize_t)(self->config.channel_count == 1u ? 1u : 2u);
    PyObject *result = PyBytes_FromStringAndSize(NULL, frames * width);
    if (result != NULL) {
        audiodsp_convolve_process_silence(&self->config, &self->state,
            (int16_t *)PyBytes_AS_STRING(result), (uint32_t)frames);
    }
    return result;
}

AUDIODSP_LOCKED_ARGS(convolver_state_configure, audiodsp_convolver_object_t)
AUDIODSP_LOCKED_ARGS(convolver_state_load, audiodsp_convolver_object_t)
AUDIODSP_LOCKED_ARGS(convolver_state_synthesize, audiodsp_convolver_object_t)
AUDIODSP_LOCKED_ARGS(convolver_state_room_peak, audiodsp_convolver_object_t)
AUDIODSP_LOCKED_ARGS(convolver_state_reset, audiodsp_convolver_object_t)
AUDIODSP_LOCKED_ARGS(convolver_state_taps, audiodsp_convolver_object_t)
AUDIODSP_LOCKED_ARGS(convolver_state_peak_gain, audiodsp_convolver_object_t)
AUDIODSP_LOCKED_ARGS(convolver_state_process, audiodsp_convolver_object_t)
AUDIODSP_LOCKED_ARGS(convolver_state_process_silence, audiodsp_convolver_object_t)
static PyMethodDef convolver_state_methods[] = {
    {"configure", (PyCFunction)convolver_state_configure_locked, METH_VARARGS, NULL},
    {"load", (PyCFunction)convolver_state_load_locked, METH_VARARGS, NULL},
    {"synthesize", (PyCFunction)convolver_state_synthesize_locked, METH_VARARGS, NULL},
    {"room_peak", (PyCFunction)convolver_state_room_peak_locked, METH_VARARGS, NULL},
    {"reset", (PyCFunction)convolver_state_reset_locked, METH_NOARGS, NULL},
    {"taps", (PyCFunction)convolver_state_taps_locked, METH_NOARGS, NULL},
    {"peak_gain", (PyCFunction)convolver_state_peak_gain_locked, METH_NOARGS, NULL},
    {"process", (PyCFunction)convolver_state_process_locked, METH_O, NULL},
    {"process_silence", (PyCFunction)convolver_state_process_silence_locked, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_INIT(convolver_state_init, audiodsp_convolver_object_t)
static PyType_Slot convolver_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, convolver_state_init_locked},
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

// --- MeterState: audiometer's engine (shared/audiodsp_meter.c) -------------

typedef struct {
    PyObject_HEAD
    audiodsp_meter_t *m;
} audiodsp_meter_object_t;

static int meter_state_init(audiodsp_meter_object_t *self, PyObject *args, PyObject *kwargs) {
    if (!PyArg_ParseTuple(args, ":MeterState")) return -1;
    if (self->m == NULL) {
        self->m = audiodsp_meter_new();
        if (self->m == NULL) {
            PyErr_NoMemory();
            return -1;
        }
    }
    return 0;
}

static void meter_state_dealloc(audiodsp_meter_object_t *self) {
    PyTypeObject *type = Py_TYPE(self);
    audiodsp_meter_free(self->m);
    self->m = NULL;
    type->tp_free((PyObject *)self);
    Py_DECREF(type);
}

static PyObject *meter_state_configure(audiodsp_meter_object_t *self, PyObject *args) {
    int bands;
    double lo, hi;
    if (!PyArg_ParseTuple(args, "idd:configure", &bands, &lo, &hi)) return NULL;
    if (bands < 0 || bands > AUDIODSP_METER_MAX_BANDS || lo <= 0 || hi <= lo) {
        PyErr_SetString(PyExc_ValueError, "bands 0..96, and 0 < low_hz < high_hz");
        return NULL;
    }
    audiodsp_meter_configure(self->m, bands, (float)lo, (float)hi);
    Py_RETURN_NONE;
}

static PyObject *meter_state_feed(audiodsp_meter_object_t *self, PyObject *args) {
    Py_buffer pcm = {0};
    unsigned int rate, channels = 2;
    if (!PyArg_ParseTuple(args, "y*I|I:feed", &pcm, &rate, &channels)) return NULL;
    if (rate < 1000 || rate > 384000 || channels < 1 || channels > 8
        || pcm.len % (2 * (Py_ssize_t)channels)) {
        PyBuffer_Release(&pcm);
        PyErr_SetString(PyExc_ValueError, "whole s16 frames, 1..8 channels, 1 kHz..384 kHz");
        return NULL;
    }
    audiodsp_meter_feed_s16(self->m, (const int16_t *)pcm.buf,
        (uint32_t)(pcm.len / (2 * (Py_ssize_t)channels)), channels, rate);
    PyBuffer_Release(&pcm);
    Py_RETURN_NONE;
}

static PyObject *meter_state_read(audiodsp_meter_object_t *self, PyObject *unused) {
    uint8_t levels[AUDIODSP_METER_MAX_BANDS];
    uint8_t pk = 0, rms = 0;
    uint32_t n = 0;
    uint32_t seq = audiodsp_meter_read(self->m, levels, sizeof(levels), &pk, &rms, &n);
    return Py_BuildValue("(ky#ii)", (unsigned long)seq, (const char *)levels, (Py_ssize_t)n, pk, rms);
}

static PyObject *meter_state_stats(audiodsp_meter_object_t *self, PyObject *unused) {
    audiodsp_meter_stats_t st;
    audiodsp_meter_stats(self->m, &st);
    return Py_BuildValue("(OkkKKkK)", st.enabled ? Py_True : Py_False, (unsigned long)st.bands,
        (unsigned long)st.analyses, (unsigned long long)st.feed_us,
        (unsigned long long)st.analysis_us, (unsigned long)st.max_analysis_us,
        (unsigned long long)st.elapsed_us);
}

AUDIODSP_LOCKED_ARGS(meter_state_configure, audiodsp_meter_object_t)
AUDIODSP_LOCKED_ARGS(meter_state_feed, audiodsp_meter_object_t)
AUDIODSP_LOCKED_ARGS(meter_state_read, audiodsp_meter_object_t)
AUDIODSP_LOCKED_ARGS(meter_state_stats, audiodsp_meter_object_t)
static PyMethodDef meter_state_methods[] = {
    {"configure", (PyCFunction)meter_state_configure_locked, METH_VARARGS, NULL},
    {"feed", (PyCFunction)meter_state_feed_locked, METH_VARARGS, NULL},
    {"read", (PyCFunction)meter_state_read_locked, METH_NOARGS, NULL},
    {"stats", (PyCFunction)meter_state_stats_locked, METH_NOARGS, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_INIT(meter_state_init, audiodsp_meter_object_t)
static PyType_Slot meter_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, meter_state_init_locked},
    {Py_tp_dealloc, meter_state_dealloc},
    {Py_tp_methods, meter_state_methods},
    {0, NULL},
};

static PyType_Spec meter_state_spec = {
    .name = "_audiodsp.MeterState",
    .basicsize = sizeof(audiodsp_meter_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = meter_state_slots,
};

// --- FeaturesState: audiofeatures' engine (shared/audiodsp_features.c) ------

typedef struct {
    PyObject_HEAD
    audiodsp_features_t *f;
} audiodsp_features_object_t;

static int features_state_init(audiodsp_features_object_t *self, PyObject *args, PyObject *kwargs) {
    unsigned int slices;
    if (!PyArg_ParseTuple(args, "I:FeaturesState", &slices)) return -1;
    if (slices < 8 || slices > 100000) {
        PyErr_SetString(PyExc_ValueError, "slices 8..100000");
        return -1;
    }
    audiodsp_features_free(self->f);
    self->f = audiodsp_features_new(slices);
    if (self->f == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    return 0;
}

static void features_state_dealloc(audiodsp_features_object_t *self) {
    PyTypeObject *type = Py_TYPE(self);
    audiodsp_features_free(self->f);
    self->f = NULL;
    type->tp_free((PyObject *)self);
    Py_DECREF(type);
}

static PyObject *features_state_feed(audiodsp_features_object_t *self, PyObject *args) {
    Py_buffer pcm = {0};
    unsigned int rate, channels = 1;
    if (!PyArg_ParseTuple(args, "y*I|I:feed", &pcm, &rate, &channels)) return NULL;
    if (rate < AUDIODSP_FEATURES_MIN_RATE || rate > AUDIODSP_FEATURES_MAX_RATE) {
        PyBuffer_Release(&pcm);
        PyErr_SetString(PyExc_ValueError, "sample_rate 16 kHz..384 kHz");
        return NULL;
    }
    if (channels < 1 || channels > 8 || pcm.len % (2 * (Py_ssize_t)channels)) {
        PyBuffer_Release(&pcm);
        PyErr_SetString(PyExc_ValueError, "whole s16 frames, 1..8 channels");
        return NULL;
    }
    int rc = audiodsp_features_feed_s16(self->f, (const int16_t *)pcm.buf,
        (size_t)(pcm.len / (2 * (Py_ssize_t)channels)), channels, rate);
    PyBuffer_Release(&pcm);
    if (rc != 0) {
        return PyErr_NoMemory();
    }
    return PyLong_FromUnsignedLong(audiodsp_features_waiting(self->f));
}

// read(buf, raw, voice=None): whole slices into a writable buffer, int8 or
// (raw) uint16, and a byte a slice into voice when it is given
static PyObject *features_state_read(audiodsp_features_object_t *self, PyObject *args) {
    Py_buffer out = {0}, vb = {0};
    int raw;
    PyObject *voice_obj = Py_None;
    if (!PyArg_ParseTuple(args, "w*p|O:read", &out, &raw, &voice_obj)) return NULL;
    size_t width = raw ? 2 * AUDIODSP_FEATURES_SIZE : AUDIODSP_FEATURES_SIZE;
    size_t max = (size_t)out.len / width;
    uint8_t *voice = NULL;
    if (voice_obj != Py_None) {
        if (PyObject_GetBuffer(voice_obj, &vb, PyBUF_WRITABLE) != 0) {
            PyBuffer_Release(&out);
            return NULL;
        }
        voice = (uint8_t *)vb.buf;
        if ((size_t)vb.len < max) {
            max = (size_t)vb.len;
        }
    }
    size_t n = raw ? audiodsp_features_read_u16(self->f, (uint16_t *)out.buf, max, voice)
        : audiodsp_features_read_s8(self->f, (int8_t *)out.buf, max, voice);
    PyBuffer_Release(&out);
    if (voice != NULL) {
        PyBuffer_Release(&vb);
    }
    return PyLong_FromSize_t(n);
}

static PyObject *features_state_speaking(audiodsp_features_object_t *self, PyObject *unused) {
    return PyBool_FromLong(audiodsp_features_speaking(self->f));
}

static PyObject *features_state_reset(audiodsp_features_object_t *self, PyObject *unused) {
    audiodsp_features_reset(self->f);
    Py_RETURN_NONE;
}

static PyObject *features_state_stats(audiodsp_features_object_t *self, PyObject *unused) {
    audiodsp_features_stats_t st;
    audiodsp_features_stats(self->f, &st);
    return Py_BuildValue("(kkkkkO)", (unsigned long)st.slices, (unsigned long)st.dropped,
        (unsigned long)st.waiting, (unsigned long)st.capacity, (unsigned long)st.rate,
        st.resampling ? Py_True : Py_False);
}

AUDIODSP_LOCKED_ARGS(features_state_feed, audiodsp_features_object_t)
AUDIODSP_LOCKED_ARGS(features_state_read, audiodsp_features_object_t)
AUDIODSP_LOCKED_ARGS(features_state_reset, audiodsp_features_object_t)
AUDIODSP_LOCKED_ARGS(features_state_speaking, audiodsp_features_object_t)
AUDIODSP_LOCKED_ARGS(features_state_stats, audiodsp_features_object_t)
static PyMethodDef features_state_methods[] = {
    {"feed", (PyCFunction)features_state_feed_locked, METH_VARARGS, NULL},
    {"read", (PyCFunction)features_state_read_locked, METH_VARARGS, NULL},
    {"reset", (PyCFunction)features_state_reset_locked, METH_NOARGS, NULL},
    {"speaking", (PyCFunction)features_state_speaking_locked, METH_NOARGS, NULL},
    {"stats", (PyCFunction)features_state_stats_locked, METH_NOARGS, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_INIT(features_state_init, audiodsp_features_object_t)
static PyType_Slot features_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, features_state_init_locked},
    {Py_tp_dealloc, features_state_dealloc},
    {Py_tp_methods, features_state_methods},
    {0, NULL},
};

static PyType_Spec features_state_spec = {
    .name = "_audiodsp.FeaturesState",
    .basicsize = sizeof(audiodsp_features_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = features_state_slots,
};

// --- Graph: audiograph's runner (shared/audiodsp_graph.c) ------------------
//
// The same calls as the MicroPython binding (src/audiograph/Graph.c), with
// the same arguments and the same refusals, so a graph built on the desktop
// is the graph a board runs. audiograph.py re-exports the type as is: there
// is nothing for Python to add between a caller and the runner.

typedef struct {
    PyObject_HEAD
    audiodsp_graph_t graph;
    uint32_t next_buffer;
    // Memoryviews of the wow tables the delay nodes borrow, kept alive (and
    // their arrays kept from resizing) for as long as the graph is.
    PyObject *keep;
    // The tables as single blocks; the graph sees them through page
    // pointers into these (shared/audiodsp_graph.h).
    int16_t *bufs;
    audiodsp_graph_node_t *table;
    audiodsp_graph_record_t *records;
    float *shadow;
    // Two output blocks for _get_buffer, used in turn.
    int16_t *out;
    uint8_t which;
    bool deinited;
} audiodsp_graph_object_t;

static int graph_state_init(audiodsp_graph_object_t *self, PyObject *args,
    PyObject *kwargs) {
    static char *keywords[] = {"sample_rate", "frames", "channel_count",
        "buffers", "nodes", "queue", NULL};
    long sample_rate = 48000, frames = 128, channels = 2, buffers = 64,
        nodes = 64, queue = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|$llllll:Graph", keywords,
            &sample_rate, &frames, &channels, &buffers, &nodes, &queue)) {
        return -1;
    }
    if (self->graph.nodes != NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Graph is already initialised");
        return -1;
    }
    if (frames < 1 || frames > (long)AUDIODSP_GRAPH_MAX_FRAMES) {
        PyErr_SetString(PyExc_ValueError, "frames must be from 1 to 256");
        return -1;
    }
    if (channels < 1 || channels > 2) {
        PyErr_SetString(PyExc_ValueError, "channel_count must be 1 or 2");
        return -1;
    }
    if (buffers < 1 || buffers > 4096 || nodes < 1 || nodes > 4096) {
        PyErr_SetString(PyExc_ValueError,
            "buffers and nodes must be from 1 to 4096");
        return -1;
    }
    if (sample_rate < 1 || sample_rate > 0x7FFFFFFFL) {
        PyErr_SetString(PyExc_ValueError, "sample_rate must be at least 1");
        return -1;
    }
    if (queue == 0) {
        queue = 4 * nodes > (long)AUDIODSP_GRAPH_QUEUE_MIN
            ? 4 * nodes : (long)AUDIODSP_GRAPH_QUEUE_MIN;
    }
    if (queue < 1 || queue > 65536) {
        PyErr_SetString(PyExc_ValueError, "queue must be from 1 to 65536");
        return -1;
    }
    int16_t *bufs = PyMem_Calloc((size_t)buffers * (size_t)frames,
        sizeof(int16_t));
    audiodsp_graph_node_t *table = PyMem_Calloc((size_t)nodes,
        sizeof(audiodsp_graph_node_t));
    audiodsp_graph_record_t *records = PyMem_Calloc((size_t)queue,
        sizeof(audiodsp_graph_record_t));
    float *shadow = PyMem_Calloc((size_t)nodes * AUDIODSP_GRAPH_MAX_PARAMS,
        sizeof(float));
    int16_t *out = PyMem_Calloc(2u * (size_t)frames * (size_t)channels,
        sizeof(int16_t));
    uint32_t node_pages = audiodsp_graph_pages((uint32_t)nodes, AUDIODSP_GRAPH_NODE_PAGE);
    uint32_t record_pages = audiodsp_graph_pages((uint32_t)queue, AUDIODSP_GRAPH_RECORD_PAGE);
    int16_t **buf_at = PyMem_Calloc((size_t)buffers, sizeof(int16_t *));
    audiodsp_graph_node_t **node_at = PyMem_Calloc(node_pages, sizeof(audiodsp_graph_node_t *));
    audiodsp_graph_record_t **record_at = PyMem_Calloc(record_pages,
        sizeof(audiodsp_graph_record_t *));
    float **shadow_at = PyMem_Calloc(node_pages, sizeof(float *));
    PyObject *keep = PyList_New(0);
    if (bufs == NULL || table == NULL || records == NULL || shadow == NULL ||
        out == NULL || buf_at == NULL || node_at == NULL || record_at == NULL ||
        shadow_at == NULL || keep == NULL) {
        PyMem_Free(bufs);
        PyMem_Free(table);
        PyMem_Free(records);
        PyMem_Free(shadow);
        PyMem_Free(out);
        PyMem_Free(buf_at);
        PyMem_Free(node_at);
        PyMem_Free(record_at);
        PyMem_Free(shadow_at);
        Py_XDECREF(keep);
        if (!PyErr_Occurred()) {
            PyErr_NoMemory();
        }
        return -1;
    }
    for (long i = 0; i < buffers; ++i) {
        buf_at[i] = bufs + (size_t)i * (size_t)frames;
    }
    for (uint32_t i = 0; i < node_pages; ++i) {
        node_at[i] = table + (size_t)i * AUDIODSP_GRAPH_NODE_PAGE;
        shadow_at[i] = shadow + (size_t)i * AUDIODSP_GRAPH_SHADOW_PAGE;
    }
    for (uint32_t i = 0; i < record_pages; ++i) {
        record_at[i] = records + (size_t)i * AUDIODSP_GRAPH_RECORD_PAGE;
    }
    audiodsp_graph_init(&self->graph, (uint32_t)sample_rate, (uint32_t)frames,
        (uint32_t)channels, buf_at, (uint32_t)buffers, node_at, (uint32_t)nodes);
    audiodsp_graph_queue_init(&self->graph, record_at, (uint32_t)queue, shadow_at);
    self->bufs = bufs;
    self->table = table;
    self->records = records;
    self->shadow = shadow;
    self->out = out;
    self->which = 0;
    self->deinited = false;
    self->next_buffer = 0;
    self->keep = keep;
    return 0;
}

static void graph_state_dealloc(audiodsp_graph_object_t *self) {
    PyTypeObject *type = Py_TYPE(self);
    if (self->graph.nodes != NULL) {
        for (uint32_t i = 0; i < self->graph.nnodes; ++i) {
            PyMem_Free(audiodsp_graph_node(&self->graph, i)->data);
        }
    }
    PyMem_Free(self->graph.nodes);
    PyMem_Free(self->graph.bufs);
    PyMem_Free(self->graph.records);
    PyMem_Free(self->graph.shadow);
    PyMem_Free(self->table);
    PyMem_Free(self->bufs);
    PyMem_Free(self->records);
    PyMem_Free(self->shadow);
    PyMem_Free(self->out);
    self->graph.nodes = NULL;
    self->graph.bufs = NULL;
    self->graph.records = NULL;
    self->graph.shadow = NULL;
    self->table = NULL;
    self->bufs = NULL;
    self->records = NULL;
    self->shadow = NULL;
    self->out = NULL;
    Py_CLEAR(self->keep);
    type->tp_free((PyObject *)self);
    Py_DECREF(type);
}

static bool graph_live(audiodsp_graph_object_t *self) {
    if (self->graph.nodes == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Graph.__init__ was not called");
        return false;
    }
    if (self->deinited) {
        PyErr_SetString(PyExc_ValueError,
            "Object has been deinitialized and can no longer be used. Create a new object.");
        return false;
    }
    return true;
}

// The queue, from the control side. No lock to pass: CPython's pump pulls on
// this thread, inside the same critical section as every call here.
static void graph_push(audiodsp_graph_object_t *self, uint8_t op, long node,
    long index, float value, uint32_t mask) {
    audiodsp_graph_enqueue(&self->graph, op, (uint32_t)node, (uint32_t)index,
        value, mask, NULL, NULL);
}

static bool graph_check(audiodsp_graph_object_t *self, long node, long index,
    const char *message) {
    if (node < 0 || index < 0 || !audiodsp_graph_valid(&self->graph,
            (uint32_t)node, (uint32_t)index)) {
        PyErr_SetString(PyExc_ValueError, message);
        return false;
    }
    return true;
}

// buffer(): a fresh block buffer's id.
static PyObject *graph_state_buffer(audiodsp_graph_object_t *self,
    PyObject *unused) {
    (void)unused;
    if (!graph_live(self)) {
        return NULL;
    }
    if (self->next_buffer >= self->graph.nbufs) {
        PyErr_SetString(PyExc_ValueError, "no buffers left");
        return NULL;
    }
    return PyLong_FromUnsignedLong(self->next_buffer++);
}

// A buffer id, or AUDIODSP_GRAPH_NONE for None. False with an exception set.
static bool graph_buffer_id(PyObject *obj, int16_t *id) {
    if (obj == Py_None) {
        *id = AUDIODSP_GRAPH_NONE;
        return true;
    }
    long v = PyLong_AsLong(obj);
    if (v == -1 && PyErr_Occurred()) {
        return false;
    }
    if (v < 0 || v > 32767) {
        PyErr_SetString(PyExc_ValueError, "bad buffer id");
        return false;
    }
    *id = (int16_t)v;
    return true;
}

// add(kind, group, inputs, output, arg=0.0): the node's index.
static PyObject *graph_state_add(audiodsp_graph_object_t *self,
    PyObject *args) {
    long kind, group;
    PyObject *inputs_obj, *output_obj;
    double arg = 0.0;
    if (!PyArg_ParseTuple(args, "llOO|d:add", &kind, &group, &inputs_obj,
            &output_obj, &arg)) {
        return NULL;
    }
    if (!graph_live(self)) {
        return NULL;
    }
    if (kind < 0 || kind >= (long)AUDIODSP_GRAPH_KINDS) {
        PyErr_SetString(PyExc_ValueError, "unknown node kind");
        return NULL;
    }
    if (group < 0 || (group > 31 && group != (long)AUDIODSP_GRAPH_ALWAYS)) {
        PyErr_SetString(PyExc_ValueError, "group must be 0..31 or ALWAYS");
        return NULL;
    }
    PyObject *seq = PySequence_Fast(inputs_obj, "inputs must be a sequence");
    if (seq == NULL) {
        return NULL;
    }
    Py_ssize_t len = PySequence_Fast_GET_SIZE(seq);
    if (len > (Py_ssize_t)AUDIODSP_GRAPH_MAX_INPUTS) {
        Py_DECREF(seq);
        PyErr_SetString(PyExc_ValueError, "too many inputs");
        return NULL;
    }
    int16_t inputs[AUDIODSP_GRAPH_MAX_INPUTS];
    for (Py_ssize_t i = 0; i < len; ++i) {
        if (!graph_buffer_id(PySequence_Fast_GET_ITEM(seq, i), &inputs[i])) {
            Py_DECREF(seq);
            return NULL;
        }
    }
    Py_DECREF(seq);
    int16_t output;
    if (!graph_buffer_id(output_obj, &output)) {
        return NULL;
    }
    size_t size = audiodsp_graph_node_data_size((audiodsp_graph_kind_t)kind,
        self->graph.sample_rate, (float)arg);
    void *data = NULL;
    if (size) {
        data = PyMem_Calloc(1, size);
        if (data == NULL) {
            return PyErr_NoMemory();
        }
    }
    int index = audiodsp_graph_add(&self->graph, (audiodsp_graph_kind_t)kind,
        (uint32_t)group, inputs, (uint32_t)len, output, data, (float)arg);
    if (index < 0) {
        PyMem_Free(data);
        PyErr_SetString(PyExc_ValueError, "graph is full, a buffer id is out "
            "of range, or the node has no output");
        return NULL;
    }
    return PyLong_FromLong(index);
}

static PyObject *graph_state_set(audiodsp_graph_object_t *self,
    PyObject *args) {
    long node, index;
    double value;
    if (!PyArg_ParseTuple(args, "lld:set", &node, &index, &value)) {
        return NULL;
    }
    if (!graph_live(self)) {
        return NULL;
    }
    if (!graph_check(self, node, index, "bad node or parameter index")) {
        return NULL;
    }
    graph_push(self, AUDIODSP_GRAPH_OP_SET, node, index, (float)value, 0);
    Py_RETURN_NONE;
}

static PyObject *graph_state_get(audiodsp_graph_object_t *self,
    PyObject *args) {
    long node, index;
    if (!PyArg_ParseTuple(args, "ll:get", &node, &index)) {
        return NULL;
    }
    if (!graph_live(self)) {
        return NULL;
    }
    float value = 0.0f;
    if (node < 0 || index < 0 || !audiodsp_graph_get_shadow(&self->graph,
            (uint32_t)node, (uint32_t)index, &value)) {
        PyErr_SetString(PyExc_ValueError, "bad node or parameter index");
        return NULL;
    }
    return PyFloat_FromDouble((double)value);
}

// table(node, int16_array): a feedback delay's wow shape, borrowed.
static PyObject *graph_state_table(audiodsp_graph_object_t *self,
    PyObject *args) {
    long node;
    PyObject *table;
    if (!PyArg_ParseTuple(args, "lO:table", &node, &table)) {
        return NULL;
    }
    if (!graph_live(self)) {
        return NULL;
    }
    // A memoryview holds the export, so the array cannot be resized under
    // the delay while the graph keeps it.
    PyObject *view = PyMemoryView_FromObject(table);
    if (view == NULL) {
        return NULL;
    }
    Py_buffer *info = PyMemoryView_GET_BUFFER(view);
    const char *format = info->format ? info->format : "B";
    if (strcmp(format, "h") != 0 || info->itemsize != 2 ||
        !PyBuffer_IsContiguous(info, 'C')) {
        Py_DECREF(view);
        PyErr_SetString(PyExc_TypeError, "table must be an int16 array");
        return NULL;
    }
    if (!audiodsp_graph_set_table(&self->graph, (uint32_t)node,
            (const int16_t *)info->buf, (uint32_t)(info->len / 2))) {
        Py_DECREF(view);
        PyErr_SetString(PyExc_ValueError,
            "not a delay node, or a bad table length");
        return NULL;
    }
    int failed = PyList_Append(self->keep, view);
    Py_DECREF(view);
    if (failed) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *graph_state_reset(audiodsp_graph_object_t *self,
    PyObject *arg) {
    long node = PyLong_AsLong(arg);
    if (node == -1 && PyErr_Occurred()) {
        return NULL;
    }
    if (!graph_live(self)) {
        return NULL;
    }
    if (!graph_check(self, node, 0, "bad node")) {
        return NULL;
    }
    graph_push(self, AUDIODSP_GRAPH_OP_RESET, node, 0, 0.0f, 0);
    Py_RETURN_NONE;
}

// enable(mask): the groups a pull renders, queued like set().
static PyObject *graph_state_enable(audiodsp_graph_object_t *self,
    PyObject *arg) {
    unsigned long mask = PyLong_AsUnsignedLongMask(arg);
    if (mask == (unsigned long)-1 && PyErr_Occurred()) {
        return NULL;
    }
    if (!graph_live(self)) {
        return NULL;
    }
    graph_push(self, AUDIODSP_GRAPH_OP_ENABLE, 0, 0, 0.0f, (uint32_t)mask);
    Py_RETURN_NONE;
}

static PyObject *graph_state_hold(audiodsp_graph_object_t *self,
    PyObject *unused) {
    (void)unused;
    if (!graph_live(self)) {
        return NULL;
    }
    audiodsp_graph_hold(&self->graph);
    Py_RETURN_NONE;
}

static PyObject *graph_state_commit(audiodsp_graph_object_t *self,
    PyObject *unused) {
    (void)unused;
    if (!graph_live(self)) {
        return NULL;
    }
    audiodsp_graph_commit(&self->graph);
    Py_RETURN_NONE;
}

static PyObject *graph_state_sounding(audiodsp_graph_object_t *self,
    PyObject *unused) {
    (void)unused;
    if (!graph_live(self)) {
        return NULL;
    }
    uint32_t sounding, frame, applied;
    audiodsp_graph_published(&self->graph, &sounding, &frame, &applied);
    return Py_BuildValue("(kkkk)", (unsigned long)sounding,
        (unsigned long)frame, (unsigned long)applied,
        (unsigned long)self->graph.pushed);
}

static PyObject *graph_state_stats(audiodsp_graph_object_t *self,
    PyObject *unused) {
    (void)unused;
    if (!graph_live(self)) {
        return NULL;
    }
    const audiodsp_graph_t *g = &self->graph;
    uint32_t sounding, frame, applied;
    audiodsp_graph_published(g, &sounding, &frame, &applied);
    return Py_BuildValue("{s:k,s:k,s:k,s:k,s:k,s:k,s:k}",
        "capacity", (unsigned long)g->capacity,
        "pushed", (unsigned long)g->pushed,
        "applied", (unsigned long)applied,
        "overflows", (unsigned long)g->overflows,
        "splits", (unsigned long)g->splits,
        "high_water", (unsigned long)g->high_water,
        "frame", (unsigned long)frame);
}

static PyObject *graph_state_deinit(audiodsp_graph_object_t *self,
    PyObject *unused) {
    (void)unused;
    self->deinited = true;
    Py_RETURN_NONE;
}

static PyObject *graph_state_enter(audiodsp_graph_object_t *self,
    PyObject *unused) {
    (void)unused;
    if (!graph_live(self)) {
        return NULL;
    }
    return Py_NewRef((PyObject *)self);
}

static PyObject *graph_state_exit(audiodsp_graph_object_t *self,
    PyObject *args) {
    (void)args;
    self->deinited = true;
    Py_RETURN_NONE;
}

// _set_now(node, index, value): writes the parameter straight into the node,
// past the queue. Not for use: the MicroPython twin exists so the pump probe
// can show its check catches a write that skips the queue, and this one keeps
// the two method sets the same.
static PyObject *graph_state_set_now(audiodsp_graph_object_t *self,
    PyObject *args) {
    long node, index;
    double value;
    if (!PyArg_ParseTuple(args, "lld:_set_now", &node, &index, &value)) {
        return NULL;
    }
    if (!graph_live(self)) {
        return NULL;
    }
    if (node < 0 || index < 0 || !audiodsp_graph_set(&self->graph,
            (uint32_t)node, (uint32_t)index, (float)value)) {
        PyErr_SetString(PyExc_ValueError, "bad node or parameter index");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *graph_state_reset_buffer(audiodsp_graph_object_t *self,
    PyObject *args, PyObject *kwargs) {
    int single_channel_output = 0;
    unsigned int audio_channel = 0;
    static char *keywords[] = {
        "single_channel_output", "audio_channel", NULL
    };
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|pI:_reset_buffer",
            keywords, &single_channel_output, &audio_channel)) {
        return NULL;
    }
    if (!graph_live(self)) {
        return NULL;
    }
    Py_RETURN_NONE;
}

// One block per pull, rendered from the queue, and never DONE.
static PyObject *graph_state_get_buffer(audiodsp_graph_object_t *self,
    PyObject *args, PyObject *kwargs) {
    int single_channel_output = 0;
    unsigned int audio_channel = 0;
    static char *keywords[] = {
        "single_channel_output", "audio_channel", NULL
    };
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|pI:_get_buffer",
            keywords, &single_channel_output, &audio_channel)) {
        return NULL;
    }
    if (!graph_live(self)) {
        return NULL;
    }
    const uint32_t channels = self->graph.channel_count;
    const uint32_t frames = self->graph.frames;
    const uint32_t samples = frames * channels;
    if (!single_channel_output || audio_channel == 0) {
        self->which ^= 1u;
        audiodsp_graph_render(&self->graph, self->out + self->which * samples);
    }
    const int16_t *block = self->out + self->which * samples;
    PyObject *data;
    if (single_channel_output && channels > 1) {
        const uint32_t channel = audio_channel < channels ? audio_channel : 0;
        data = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)frames * 2);
        if (data != NULL) {
            int16_t *dst = (int16_t *)(void *)PyBytes_AS_STRING(data);
            for (uint32_t i = 0; i < frames; ++i) {
                dst[i] = block[i * channels + channel];
            }
        }
    } else {
        data = PyBytes_FromStringAndSize((const char *)block,
            (Py_ssize_t)samples * 2);
    }
    if (data == NULL) {
        return NULL;
    }
    return Py_BuildValue("(iN)", 1 /* GET_BUFFER_MORE_DATA */, data);
}

#define GRAPH_FORMAT_GETTER(name, expr) \
    static PyObject *graph_get_##name(audiodsp_graph_object_t *self, \
        void *closure) { \
        (void)closure; \
        if (!graph_live(self)) { \
            return NULL; \
        } \
        return expr; \
    }
GRAPH_FORMAT_GETTER(sample_rate,
    PyLong_FromUnsignedLong(self->graph.sample_rate))
GRAPH_FORMAT_GETTER(channel_count,
    PyLong_FromUnsignedLong(self->graph.channel_count))
GRAPH_FORMAT_GETTER(bits_per_sample, PyLong_FromLong(16))
GRAPH_FORMAT_GETTER(samples_signed, Py_NewRef(Py_True))
GRAPH_FORMAT_GETTER(single_buffer, Py_NewRef(Py_False))
GRAPH_FORMAT_GETTER(max_buffer_length, PyLong_FromUnsignedLong(
    self->graph.frames * self->graph.channel_count * 2u))

// run(out=None, block=0, enable=0xFFFFFFFF): renders one block into `out`
// at block `block` (frames * channel_count samples each), and returns the
// mask of groups whose envelopes are still sounding.
static PyObject *graph_state_run(audiodsp_graph_object_t *self,
    PyObject *args) {
    PyObject *out_obj = Py_None, *enable_obj = NULL;
    long block = 0;
    if (!PyArg_ParseTuple(args, "|OlO:run", &out_obj, &block, &enable_obj)) {
        return NULL;
    }
    if (!graph_live(self)) {
        return NULL;
    }
    uint32_t enable = 0;
    if (enable_obj != NULL) {
        // Truncated to 32 bits, as the MicroPython binding takes it.
        unsigned long mask = PyLong_AsUnsignedLongMask(enable_obj);
        if (mask == (unsigned long)-1 && PyErr_Occurred()) {
            return NULL;
        }
        enable = (uint32_t)mask;
    }
    Py_buffer view = {0};
    int16_t *out = NULL;
    if (out_obj != Py_None) {
        if (PyObject_GetBuffer(out_obj, &view, PyBUF_WRITABLE) < 0) {
            return NULL;
        }
        size_t block_bytes = (size_t)self->graph.frames *
            self->graph.channel_count * 2u;
        if (block < 0 || (size_t)(block + 1) * block_bytes > (size_t)view.len) {
            PyBuffer_Release(&view);
            PyErr_SetString(PyExc_ValueError, "out is too small for that block");
            return NULL;
        }
        out = (int16_t *)(void *)((uint8_t *)view.buf +
            (size_t)block * block_bytes);
    }
    uint32_t sounding = enable_obj != NULL
        ? audiodsp_graph_render_mask(&self->graph, out, enable)
        : audiodsp_graph_render(&self->graph, out);
    if (out != NULL) {
        PyBuffer_Release(&view);
    }
    return PyLong_FromUnsignedLong(sounding);
}

static PyObject *graph_state_peak(audiodsp_graph_object_t *self,
    PyObject *arg) {
    long id = PyLong_AsLong(arg);
    if (id == -1 && PyErr_Occurred()) {
        return NULL;
    }
    if (!graph_live(self)) {
        return NULL;
    }
    int32_t peak = audiodsp_graph_peak(&self->graph, (uint32_t)id);
    if (id < 0 || peak < 0) {
        PyErr_SetString(PyExc_ValueError, "bad buffer id");
        return NULL;
    }
    return PyLong_FromLong(peak);
}

static int16_t *graph_block_of(audiodsp_graph_object_t *self, long id) {
    if (id < 0 || (uint32_t)id >= self->graph.nbufs) {
        PyErr_SetString(PyExc_ValueError, "bad buffer id");
        return NULL;
    }
    return self->graph.bufs[id];
}

// read(buffer, dst): copy a block out into an int16 buffer of `frames`.
static PyObject *graph_state_read(audiodsp_graph_object_t *self,
    PyObject *args) {
    long id;
    Py_buffer dst;
    if (!PyArg_ParseTuple(args, "lw*:read", &id, &dst)) {
        return NULL;
    }
    const int16_t *src = graph_live(self) ? graph_block_of(self, id) : NULL;
    if (src == NULL) {
        PyBuffer_Release(&dst);
        return NULL;
    }
    if ((size_t)dst.len != (size_t)self->graph.frames * 2u) {
        PyBuffer_Release(&dst);
        PyErr_SetString(PyExc_ValueError, "dst must hold one block of int16");
        return NULL;
    }
    memcpy(dst.buf, src, (size_t)dst.len);
    PyBuffer_Release(&dst);
    Py_RETURN_NONE;
}

// write(buffer, src): fill a block from an int16 buffer of `frames`.
static PyObject *graph_state_write(audiodsp_graph_object_t *self,
    PyObject *args) {
    long id;
    Py_buffer src;
    if (!PyArg_ParseTuple(args, "ly*:write", &id, &src)) {
        return NULL;
    }
    int16_t *dst = graph_live(self) ? graph_block_of(self, id) : NULL;
    if (dst == NULL) {
        PyBuffer_Release(&src);
        return NULL;
    }
    if ((size_t)src.len != (size_t)self->graph.frames * 2u) {
        PyBuffer_Release(&src);
        PyErr_SetString(PyExc_ValueError, "src must hold one block of int16");
        return NULL;
    }
    memcpy(dst, src.buf, (size_t)src.len);
    PyBuffer_Release(&src);
    Py_RETURN_NONE;
}

AUDIODSP_LOCKED_ARGS(graph_state_buffer, audiodsp_graph_object_t)
AUDIODSP_LOCKED_ARGS(graph_state_add, audiodsp_graph_object_t)
AUDIODSP_LOCKED_ARGS(graph_state_set, audiodsp_graph_object_t)
AUDIODSP_LOCKED_ARGS(graph_state_set_now, audiodsp_graph_object_t)
AUDIODSP_LOCKED_ARGS(graph_state_enter, audiodsp_graph_object_t)
AUDIODSP_LOCKED_ARGS(graph_state_exit, audiodsp_graph_object_t)
AUDIODSP_LOCKED_ARGS(graph_state_get, audiodsp_graph_object_t)
AUDIODSP_LOCKED_ARGS(graph_state_table, audiodsp_graph_object_t)
AUDIODSP_LOCKED_ARGS(graph_state_reset, audiodsp_graph_object_t)
AUDIODSP_LOCKED_ARGS(graph_state_run, audiodsp_graph_object_t)
AUDIODSP_LOCKED_ARGS(graph_state_peak, audiodsp_graph_object_t)
AUDIODSP_LOCKED_ARGS(graph_state_read, audiodsp_graph_object_t)
AUDIODSP_LOCKED_ARGS(graph_state_write, audiodsp_graph_object_t)
AUDIODSP_LOCKED_ARGS(graph_state_enable, audiodsp_graph_object_t)
AUDIODSP_LOCKED_ARGS(graph_state_hold, audiodsp_graph_object_t)
AUDIODSP_LOCKED_ARGS(graph_state_commit, audiodsp_graph_object_t)
AUDIODSP_LOCKED_ARGS(graph_state_sounding, audiodsp_graph_object_t)
AUDIODSP_LOCKED_ARGS(graph_state_stats, audiodsp_graph_object_t)
AUDIODSP_LOCKED_ARGS(graph_state_deinit, audiodsp_graph_object_t)
AUDIODSP_LOCKED_KW(graph_state_reset_buffer, audiodsp_graph_object_t)
AUDIODSP_LOCKED_KW(graph_state_get_buffer, audiodsp_graph_object_t)
AUDIODSP_LOCKED_GET(graph_get_sample_rate, audiodsp_graph_object_t)
AUDIODSP_LOCKED_GET(graph_get_channel_count, audiodsp_graph_object_t)
AUDIODSP_LOCKED_GET(graph_get_bits_per_sample, audiodsp_graph_object_t)
AUDIODSP_LOCKED_GET(graph_get_samples_signed, audiodsp_graph_object_t)
AUDIODSP_LOCKED_GET(graph_get_single_buffer, audiodsp_graph_object_t)
AUDIODSP_LOCKED_GET(graph_get_max_buffer_length, audiodsp_graph_object_t)
static PyGetSetDef graph_state_getset[] = {
    {"sample_rate", (getter)graph_get_sample_rate_locked, NULL, NULL, NULL},
    {"channel_count", (getter)graph_get_channel_count_locked, NULL, NULL, NULL},
    {"bits_per_sample", (getter)graph_get_bits_per_sample_locked, NULL, NULL, NULL},
    {"samples_signed", (getter)graph_get_samples_signed_locked, NULL, NULL, NULL},
    {"single_buffer", (getter)graph_get_single_buffer_locked, NULL, NULL, NULL},
    {"max_buffer_length", (getter)graph_get_max_buffer_length_locked, NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},
};
static PyMethodDef graph_state_methods[] = {
    {"enable", (PyCFunction)graph_state_enable_locked, METH_O,
     PyDoc_STR("enable(mask): the groups a pull renders, queued like set().")},
    {"hold", (PyCFunction)graph_state_hold_locked, METH_NOARGS,
     PyDoc_STR("hold(): keep what is queued until commit(), so it lands in one block.")},
    {"commit", (PyCFunction)graph_state_commit_locked, METH_NOARGS,
     PyDoc_STR("commit(): publish what was queued since hold().")},
    {"sounding", (PyCFunction)graph_state_sounding_locked, METH_NOARGS,
     PyDoc_STR("sounding(): (groups sounding, frames rendered, records applied, records queued).")},
    {"stats", (PyCFunction)graph_state_stats_locked, METH_NOARGS,
     PyDoc_STR("stats(): the queue's counts.")},
    {"deinit", (PyCFunction)graph_state_deinit_locked, METH_NOARGS,
     PyDoc_STR("deinit(): release the graph.")},
    {"__enter__", (PyCFunction)graph_state_enter_locked, METH_NOARGS, NULL},
    {"__exit__", (PyCFunction)graph_state_exit_locked, METH_VARARGS, NULL},
    {"_set_now", (PyCFunction)graph_state_set_now_locked, METH_VARARGS, NULL},
    {"_reset_buffer", (PyCFunction)graph_state_reset_buffer_locked, METH_VARARGS | METH_KEYWORDS, NULL},
    {"_get_buffer", (PyCFunction)graph_state_get_buffer_locked, METH_VARARGS | METH_KEYWORDS, NULL},
    {"buffer", (PyCFunction)graph_state_buffer_locked, METH_NOARGS,
     PyDoc_STR("buffer(): a fresh block buffer's id.")},
    {"add", (PyCFunction)graph_state_add_locked, METH_VARARGS,
     PyDoc_STR("add(kind, group, inputs, output, arg=0.0): append a node; returns its index.")},
    {"set", (PyCFunction)graph_state_set_locked, METH_VARARGS,
     PyDoc_STR("set(node, index, value): queue a parameter for the next block.")},
    {"get", (PyCFunction)graph_state_get_locked, METH_VARARGS,
     PyDoc_STR("get(node, index): read a parameter back.")},
    {"table", (PyCFunction)graph_state_table_locked, METH_VARARGS,
     PyDoc_STR("table(node, int16_array): a feedback delay's wow shape, borrowed.")},
    {"reset", (PyCFunction)graph_state_reset_locked, METH_O,
     PyDoc_STR("reset(node): clear a node's running state at the next block.")},
    {"run", (PyCFunction)graph_state_run_locked, METH_VARARGS,
     PyDoc_STR("run(out=None, block=0, enable=0xFFFFFFFF): render one block; returns the sounding groups.")},
    {"peak", (PyCFunction)graph_state_peak_locked, METH_O,
     PyDoc_STR("peak(buffer): a block's peak.")},
    {"read", (PyCFunction)graph_state_read_locked, METH_VARARGS,
     PyDoc_STR("read(buffer, dst): copy a block out.")},
    {"write", (PyCFunction)graph_state_write_locked, METH_VARARGS,
     PyDoc_STR("write(buffer, src): fill a block.")},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_INIT(graph_state_init, audiodsp_graph_object_t)
static PyType_Slot graph_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, graph_state_init_locked},
    {Py_tp_dealloc, graph_state_dealloc},
    {Py_tp_methods, graph_state_methods},
    {Py_tp_getset, graph_state_getset},
    {Py_tp_doc, (void *)PyDoc_STR("Graph(*, sample_rate=48000, frames=128, "
        "channel_count=2, buffers=64, nodes=64): a whole patch rendered by "
        "one C call per block. See docs/how-it-works/audiograph.md.")},
    {0, NULL},
};

static PyType_Spec graph_state_spec = {
    // audiograph.py re-exports this type as audiograph.Graph.
    .name = "audiograph.Graph",
    .basicsize = sizeof(audiodsp_graph_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = graph_state_slots,
};

// The node kinds and parameter numbers audiograph exports, by name, from
// the same enums the MicroPython module table reads.
static int graph_add_constants(PyObject *module) {
    static const struct {
        const char *name;
        long value;
    } constants[] = {
        {"CONST", AUDIODSP_GRAPH_CONST},
        {"OSC", AUDIODSP_GRAPH_OSC},
        {"LADDER", AUDIODSP_GRAPH_LADDER},
        {"BIQUAD", AUDIODSP_GRAPH_BIQUAD},
        {"FBDELAY", AUDIODSP_GRAPH_FBDELAY},
        {"ENV", AUDIODSP_GRAPH_ENV},
        {"LFO", AUDIODSP_GRAPH_LFO},
        {"NOISE", AUDIODSP_GRAPH_NOISE},
        {"MIX", AUDIODSP_GRAPH_MIX},
        {"VCA", AUDIODSP_GRAPH_VCA},
        {"OUT", AUDIODSP_GRAPH_OUT},
        {"PHASER", AUDIODSP_GRAPH_PHASER},
        {"LAG", AUDIODSP_GRAPH_LAG},
        {"SAMPLE_HOLD", AUDIODSP_GRAPH_SAMPLE_HOLD},
        {"TWOPOLE", AUDIODSP_GRAPH_TWOPOLE},
        {"OVERDRIVE", AUDIODSP_GRAPH_OVERDRIVE},
        {"TANH", AUDIODSP_GRAPH_TANH},
        {"PAN", AUDIODSP_GRAPH_PAN},
        {"ALWAYS", AUDIODSP_GRAPH_ALWAYS},
        {"UNIT", 16384},
        {"ENV_ATTACK", AUDIODSP_GRAPH_ENV_ATTACK},
        {"ENV_DECAY", AUDIODSP_GRAPH_ENV_DECAY},
        {"ENV_SUSTAIN", AUDIODSP_GRAPH_ENV_SUSTAIN},
        {"ENV_RELEASE", AUDIODSP_GRAPH_ENV_RELEASE},
        {"LFO_RATE", AUDIODSP_GRAPH_LFO_RATE},
        {"LFO_AMPLITUDE", AUDIODSP_GRAPH_LFO_AMPLITUDE},
        {"MIX_OFFSET", AUDIODSP_GRAPH_MIX_OFFSET},
        {"VCA_GAIN", AUDIODSP_GRAPH_VCA_GAIN},
        {"VCA_SELECT", AUDIODSP_GRAPH_VCA_SELECT},
        {"VCA_CURVE", AUDIODSP_GRAPH_VCA_CURVE},
        {"VCA_LINEAR", AUDIODSP_GRAPH_VCA_LINEAR},
        {"VCA_EXPONENTIAL", AUDIODSP_GRAPH_VCA_EXPONENTIAL},
        {"LFO_SHAPE", AUDIODSP_GRAPH_LFO_SHAPE},
        {"LFO_TRIANGLE", AUDIODSP_GRAPH_LFO_TRIANGLE},
        {"LFO_SINE", AUDIODSP_GRAPH_LFO_SINE},
        {"LFO_SAW", AUDIODSP_GRAPH_LFO_SAW},
        {"LFO_SQUARE", AUDIODSP_GRAPH_LFO_SQUARE},
        {"LFO_RANDOM", AUDIODSP_GRAPH_LFO_RANDOM},
        {"LAG_TIME", AUDIODSP_GRAPH_LAG_TIME},
        {"LAG_MODE", AUDIODSP_GRAPH_LAG_MODE},
        {"LAG_SHAPE", AUDIODSP_GRAPH_LAG_SHAPE},
        {"LAG_OFF", AUDIODSP_GRAPH_LAG_OFF},
        {"LAG_ON", AUDIODSP_GRAPH_LAG_ON},
        {"LAG_AUTO", AUDIODSP_GRAPH_LAG_AUTO},
        {"LAG_LINEAR", AUDIODSP_GRAPH_LAG_LINEAR},
        {"LAG_ONE_POLE", AUDIODSP_GRAPH_LAG_ONE_POLE},
        {"SH_LAG", AUDIODSP_GRAPH_SH_LAG},
        {"SH_RATE", AUDIODSP_GRAPH_SH_RATE},
        {"TWOPOLE_TYPE", AUDIODSP_GRAPH_TWOPOLE_TYPE},
        {"TWOPOLE_CUTOFF_HZ", AUDIODSP_GRAPH_TWOPOLE_CUTOFF_HZ},
        {"TWOPOLE_DEPTH", AUDIODSP_GRAPH_TWOPOLE_DEPTH},
        {"TWOPOLE_RESONANCE", AUDIODSP_GRAPH_TWOPOLE_RESONANCE},
        {"TWOPOLE_DRIVE", AUDIODSP_GRAPH_TWOPOLE_DRIVE},
        {"TWOPOLE_SK_LOWPASS", AUDIODSP_GRAPH_TWOPOLE_SK_LOWPASS},
        {"TWOPOLE_SK_HIGHPASS", AUDIODSP_GRAPH_TWOPOLE_SK_HIGHPASS},
        {"TWOPOLE_CMOS_LOWPASS", AUDIODSP_GRAPH_TWOPOLE_CMOS_LOWPASS},
        {"TWOPOLE_CMOS_BANDPASS", AUDIODSP_GRAPH_TWOPOLE_CMOS_BANDPASS},
        {"TWOPOLE_CMOS_HIGHPASS", AUDIODSP_GRAPH_TWOPOLE_CMOS_HIGHPASS},
        {"OVERDRIVE_DRIVE", AUDIODSP_GRAPH_OVERDRIVE_DRIVE},
        {"OVERDRIVE_TONE", AUDIODSP_GRAPH_OVERDRIVE_TONE},
        {"OVERDRIVE_LEVEL", AUDIODSP_GRAPH_OVERDRIVE_LEVEL},
        {"TANH_DRIVE", AUDIODSP_GRAPH_TANH_DRIVE},
        {"TANH_LEVEL", AUDIODSP_GRAPH_TANH_LEVEL},
        {"PAN_PAN", AUDIODSP_GRAPH_PAN_PAN},
        {"OSC_FREQUENCY", AUDIODSP_OSCILLATOR_OPT_FREQUENCY},
        {"OSC_WAVEFORM", AUDIODSP_OSCILLATOR_OPT_WAVEFORM},
        {"OSC_PULSE_WIDTH", AUDIODSP_OSCILLATOR_OPT_PULSE_WIDTH},
        {"OSC_LEVEL", AUDIODSP_OSCILLATOR_OPT_LEVEL},
        {"OSC_PITCH_OCTAVES", AUDIODSP_OSCILLATOR_OPT_PITCH_OCTAVES},
        {"OSC_FM_DEPTH", AUDIODSP_OSCILLATOR_OPT_FM_DEPTH},
        {"OSC_PWM_DEPTH", AUDIODSP_OSCILLATOR_OPT_PWM_DEPTH},
        {"LADDER_CUTOFF_HZ", AUDIODSP_LADDER_OPT_CUTOFF_HZ},
        {"LADDER_RESONANCE", AUDIODSP_LADDER_OPT_RESONANCE},
        {"LADDER_DRIVE", AUDIODSP_LADDER_OPT_DRIVE},
        {"LADDER_POLES", AUDIODSP_LADDER_OPT_POLES},
        {"LADDER_PASSBAND_COMP", AUDIODSP_LADDER_OPT_PASSBAND_COMP},
        {"LADDER_OVERSAMPLE", AUDIODSP_LADDER_OPT_OVERSAMPLE},
        {"LADDER_MIX", AUDIODSP_LADDER_OPT_MIX},
        {"LADDER_CUTOFF_OCTAVES", AUDIODSP_LADDER_OPT_CUTOFF_OCTAVES},
        {"BIQUAD_MODE", AUDIODSP_BIQUAD_F32_OPT_MODE},
        {"BIQUAD_FREQUENCY", AUDIODSP_BIQUAD_F32_OPT_FREQUENCY},
        {"BIQUAD_Q", AUDIODSP_BIQUAD_F32_OPT_Q},
        {"BIQUAD_GAIN_DB", AUDIODSP_BIQUAD_F32_OPT_GAIN_DB},
        {"BIQUAD_MIX", AUDIODSP_BIQUAD_F32_OPT_MIX},
        {"DELAY_MS", AUDIODSP_FEEDBACK_DELAY_OPT_DELAY_MS},
        {"DELAY_FEEDBACK", AUDIODSP_FEEDBACK_DELAY_OPT_FEEDBACK},
        {"DELAY_MIX", AUDIODSP_FEEDBACK_DELAY_OPT_MIX},
        {"DELAY_DAMPING_HZ", AUDIODSP_FEEDBACK_DELAY_OPT_DAMPING_HZ},
        {"DELAY_WOW_HZ", AUDIODSP_FEEDBACK_DELAY_OPT_WOW_HZ},
        {"DELAY_WOW_DEPTH_MS", AUDIODSP_FEEDBACK_DELAY_OPT_WOW_DEPTH_MS},
        {"PHASER_FREQUENCY", AUDIODSP_ALLPASS_F32_OPT_FREQUENCY},
        {"PHASER_FEEDBACK", AUDIODSP_ALLPASS_F32_OPT_FEEDBACK},
        {"PHASER_MIX", AUDIODSP_ALLPASS_F32_OPT_MIX},
    };
    PyObject *table = PyDict_New();
    if (table == NULL) {
        return -1;
    }
    for (size_t i = 0; i < sizeof(constants) / sizeof(constants[0]); ++i) {
        PyObject *value = PyLong_FromLong(constants[i].value);
        if (value == NULL ||
            PyDict_SetItemString(table, constants[i].name, value) < 0) {
            Py_XDECREF(value);
            Py_DECREF(table);
            return -1;
        }
        Py_DECREF(value);
    }
    int result = PyModule_AddObjectRef(module, "GRAPH_CONSTANTS", table);
    Py_DECREF(table);
    return result;
}

// --- VorbisStream: audiovorbis's decoder (shared/audiodsp_vorbis.c) --------
//
// The decoder reads the Python file object through three callbacks. A read
// that raises is kept and raised again once Tremor has returned, so no
// Python exception is ever pending inside it.

typedef struct {
    PyObject_HEAD
    audiodsp_vorbis_t *v;
    PyObject *file;
    PyObject *pending;   // the exception a read raised, until it's raised again
    int seekable;
} audiodsp_vorbis_object_t;

static void vorbis_keep_error(audiodsp_vorbis_object_t *self) {
    if (self->pending != NULL) {
        PyErr_Clear();
        return;
    }
#if PY_VERSION_HEX >= 0x030C0000
    self->pending = PyErr_GetRaisedException();
#else
    PyObject *type, *value, *tb;
    PyErr_Fetch(&type, &value, &tb);
    PyErr_NormalizeException(&type, &value, &tb);
    if (tb != NULL && value != NULL) PyException_SetTraceback(value, tb);
    Py_XDECREF(type);
    Py_XDECREF(tb);
    self->pending = value;
#endif
}

static int vorbis_raise_pending(audiodsp_vorbis_object_t *self) {
    if (self->pending == NULL) return 0;
    PyObject *exc = self->pending;
    self->pending = NULL;
#if PY_VERSION_HEX >= 0x030C0000
    PyErr_SetRaisedException(exc);
#else
    PyErr_SetObject((PyObject *)Py_TYPE(exc), exc);
    Py_DECREF(exc);
#endif
    return 1;
}

static long vorbis_py_read(void *ds, void *buf, size_t len) {
    audiodsp_vorbis_object_t *self = ds;
    PyObject *data = PyObject_CallMethod(self->file, "read", "n", (Py_ssize_t)len);
    if (data == NULL) {
        vorbis_keep_error(self);
        return -1;
    }
    Py_buffer view;
    if (PyObject_GetBuffer(data, &view, PyBUF_SIMPLE) < 0) {
        Py_DECREF(data);
        vorbis_keep_error(self);
        return -1;
    }
    Py_ssize_t got = view.len < (Py_ssize_t)len ? view.len : (Py_ssize_t)len;
    memcpy(buf, view.buf, (size_t)got);
    PyBuffer_Release(&view);
    Py_DECREF(data);
    return (long)got;
}

static int vorbis_py_seek(void *ds, int64_t offset, int whence) {
    audiodsp_vorbis_object_t *self = ds;
    if (!self->seekable) return -1;
    PyObject *r = PyObject_CallMethod(self->file, "seek", "Li", (long long)offset, whence);
    if (r == NULL) {
        PyErr_Clear();
        return -1;
    }
    Py_DECREF(r);
    return 0;
}

static long vorbis_py_tell(void *ds) {
    audiodsp_vorbis_object_t *self = ds;
    PyObject *r = PyObject_CallMethod(self->file, "tell", NULL);
    if (r == NULL) {
        PyErr_Clear();
        return -1;
    }
    long at = PyLong_AsLong(r);
    Py_DECREF(r);
    if (at == -1 && PyErr_Occurred()) {
        PyErr_Clear();
        return -1;
    }
    return at;
}

static const audiodsp_vorbis_io_t vorbis_py_io = {
    vorbis_py_read, vorbis_py_seek, vorbis_py_tell,
};

static int vorbis_raise(audiodsp_vorbis_object_t *self, int err, const char *what) {
    if (vorbis_raise_pending(self)) return -1;
    if (err == AUDIODSP_VORBIS_ENOMEM) {
        PyErr_NoMemory();
    } else {
        PyErr_Format(PyExc_RuntimeError, "%s: %s", what, audiodsp_vorbis_strerror(err));
    }
    return -1;
}

static int vorbis_stream_init(audiodsp_vorbis_object_t *self, PyObject *args, PyObject *kwargs) {
    PyObject *file;
    if (!PyArg_ParseTuple(args, "O:VorbisStream", &file)) return -1;
    if (self->v != NULL) {
        audiodsp_vorbis_close(self->v);
        self->v = NULL;
    }
    Py_INCREF(file);
    Py_XSETREF(self->file, file);
    self->seekable = 0;
    PyObject *r = PyObject_CallMethod(file, "seekable", NULL);
    if (r == NULL) {
        PyErr_Clear();
    } else {
        self->seekable = PyObject_IsTrue(r) == 1;
        Py_DECREF(r);
    }
    int err = 0;
    self->v = audiodsp_vorbis_open(&vorbis_py_io, self, &err);
    if (self->v == NULL) {
        return vorbis_raise(self, err, "Failed to parse Vorbis file");
    }
    return 0;
}

static void vorbis_stream_dealloc(audiodsp_vorbis_object_t *self) {
    PyTypeObject *type = Py_TYPE(self);
    audiodsp_vorbis_close(self->v);
    self->v = NULL;
    Py_CLEAR(self->file);
    Py_CLEAR(self->pending);
    type->tp_free((PyObject *)self);
    Py_DECREF(type);
}

static int vorbis_check_open(audiodsp_vorbis_object_t *self) {
    if (self->v == NULL) {
        PyErr_SetString(PyExc_ValueError, "the Vorbis stream is closed");
        return -1;
    }
    return 0;
}

static PyObject *vorbis_stream_info(audiodsp_vorbis_object_t *self, PyObject *unused) {
    if (vorbis_check_open(self) < 0) return NULL;
    return Py_BuildValue("(liL)", audiodsp_vorbis_sample_rate(self->v),
        audiodsp_vorbis_channels(self->v), (long long)audiodsp_vorbis_frames(self->v));
}

// read(frames) -> bytes of interleaved native-endian s16, short or empty at
// the end of the stream.
static PyObject *vorbis_stream_read(audiodsp_vorbis_object_t *self, PyObject *args) {
    Py_ssize_t frames;
    if (!PyArg_ParseTuple(args, "n:read", &frames)) return NULL;
    if (vorbis_check_open(self) < 0) return NULL;
    if (frames < 0) {
        PyErr_SetString(PyExc_ValueError, "frames must be at least 0");
        return NULL;
    }
    const Py_ssize_t frame_bytes = (Py_ssize_t)audiodsp_vorbis_channels(self->v) * 2;
    PyObject *out = PyBytes_FromStringAndSize(NULL, frames * frame_bytes);
    if (out == NULL) return NULL;
    int16_t *pcm = (int16_t *)PyBytes_AS_STRING(out);
    Py_ssize_t have = 0;
    while (have < frames) {
        long got = audiodsp_vorbis_read(self->v, pcm + have * (frame_bytes / 2),
            (size_t)(frames - have));
        if (got < 0) {
            Py_DECREF(out);
            vorbis_raise(self, (int)got, "Vorbis decoding failed");
            return NULL;
        }
        if (got == 0) break;
        have += got;
    }
    if (have != frames && _PyBytes_Resize(&out, have * frame_bytes) < 0) return NULL;
    return out;
}

static PyObject *vorbis_stream_seek(audiodsp_vorbis_object_t *self, PyObject *args) {
    long long frame;
    if (!PyArg_ParseTuple(args, "L:seek", &frame)) return NULL;
    if (vorbis_check_open(self) < 0) return NULL;
    int err = audiodsp_vorbis_seek(self->v, (int64_t)frame);
    if (err < 0) {
        vorbis_raise(self, err, "Vorbis seek failed");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *vorbis_stream_close(audiodsp_vorbis_object_t *self, PyObject *unused) {
    audiodsp_vorbis_close(self->v);
    self->v = NULL;
    Py_CLEAR(self->file);
    Py_RETURN_NONE;
}

AUDIODSP_LOCKED_ARGS(vorbis_stream_info, audiodsp_vorbis_object_t)
AUDIODSP_LOCKED_ARGS(vorbis_stream_read, audiodsp_vorbis_object_t)
AUDIODSP_LOCKED_ARGS(vorbis_stream_seek, audiodsp_vorbis_object_t)
AUDIODSP_LOCKED_ARGS(vorbis_stream_close, audiodsp_vorbis_object_t)
static PyMethodDef vorbis_stream_methods[] = {
    {"info", (PyCFunction)vorbis_stream_info_locked, METH_NOARGS, NULL},
    {"read", (PyCFunction)vorbis_stream_read_locked, METH_VARARGS, NULL},
    {"seek", (PyCFunction)vorbis_stream_seek_locked, METH_VARARGS, NULL},
    {"close", (PyCFunction)vorbis_stream_close_locked, METH_NOARGS, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_INIT(vorbis_stream_init, audiodsp_vorbis_object_t)
static PyType_Slot vorbis_stream_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, vorbis_stream_init_locked},
    {Py_tp_dealloc, vorbis_stream_dealloc},
    {Py_tp_methods, vorbis_stream_methods},
    {0, NULL},
};

static PyType_Spec vorbis_stream_spec = {
    .name = "_audiodsp.VorbisStream",
    .basicsize = sizeof(audiodsp_vorbis_object_t),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE,
    .slots = vorbis_stream_slots,
};

static PyObject *audiodsp_meter_db_byte_py(PyObject *module, PyObject *args) {
    double power;
    if (!PyArg_ParseTuple(args, "d:meter_db_byte", &power)) return NULL;
    return PyLong_FromLong(audiodsp_meter_db_byte((float)power));
}

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

// A buffer's struct format names one native item: "h", or "@h", "=h", and on a
// little-endian host "<h" (numpy and memoryview spell the same thing several
// ways). Returns the item's code, or 0 for anything else.
static char audiodsp_native_format(const char *format) {
    if (format == NULL) {
        return 'B';
    }
    if (format[0] == '@' || format[0] == '=') {
        format++;
    }
    #if PY_LITTLE_ENDIAN
    else if (format[0] == '<') {
        format++;
    }
    #else
    else if (format[0] == '>' || format[0] == '!') {
        format++;
    }
    #endif
    if (format[0] == 0 || format[1] != 0) {
        return 0;
    }
    return format[0];
}

// cast_into(dst, src, scale): shared/audiodsp_cast.h into a buffer the caller
// owns. audiomath.cast_into gives scale its default.
static PyObject *audiodsp_py_cast_into(PyObject *module, PyObject *args) {
    PyObject *dst_obj;
    PyObject *src_obj;
    double scale;
    if (!PyArg_ParseTuple(args, "OOd:cast_into", &dst_obj, &src_obj,
        &scale)) {
        return NULL;
    }
    Py_buffer dst = {0};
    if (PyObject_GetBuffer(dst_obj, &dst,
        PyBUF_WRITABLE | PyBUF_C_CONTIGUOUS | PyBUF_FORMAT) < 0) {
        return NULL;
    }
    char dst_code = audiodsp_native_format(dst.format);
    if (dst_code != 'h' && dst_code != 'b' && dst_code != 'B'
        && dst_code != 'c') {
        PyBuffer_Release(&dst);
        PyErr_SetString(PyExc_TypeError, "dst must be int16");
        return NULL;
    }
    Py_buffer src = {0};
    if (PyObject_GetBuffer(src_obj, &src,
        PyBUF_C_CONTIGUOUS | PyBUF_FORMAT) < 0) {
        PyBuffer_Release(&dst);
        return NULL;
    }
    char src_code = audiodsp_native_format(src.format);
    Py_ssize_t width = src_code == 'f' ? (Py_ssize_t)sizeof(float)
        : src_code == 'd' ? (Py_ssize_t)sizeof(double) : 0;
    if (width == 0) {
        PyBuffer_Release(&dst);
        PyBuffer_Release(&src);
        PyErr_SetString(PyExc_TypeError, "src must be float32 or float64");
        return NULL;
    }
    Py_ssize_t count = src.len / width;
    if (dst.len != count * (Py_ssize_t)sizeof(int16_t)) {
        PyBuffer_Release(&dst);
        PyBuffer_Release(&src);
        PyErr_SetString(PyExc_ValueError,
            "dst and src must have the same length");
        return NULL;
    }
    if (width == (Py_ssize_t)sizeof(float)) {
        audiodsp_cast_s16_f32((int16_t *)dst.buf, (const float *)src.buf,
            (size_t)count, (float)scale);
    } else {
        audiodsp_cast_s16_f64((int16_t *)dst.buf, (const double *)src.buf,
            (size_t)count, scale);
    }
    PyBuffer_Release(&dst);
    PyBuffer_Release(&src);
    Py_RETURN_NONE;
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

AUDIODSP_LOCKED_ARGS(suboctave_state_configure, audiodsp_suboctave_object_t)
AUDIODSP_LOCKED_ARGS(suboctave_state_finish, audiodsp_suboctave_object_t)
AUDIODSP_LOCKED_ARGS(suboctave_state_reset, audiodsp_suboctave_object_t)
AUDIODSP_LOCKED_ARGS(suboctave_state_process, audiodsp_suboctave_object_t)
static PyMethodDef suboctave_state_methods[] = {
    {"configure", (PyCFunction)suboctave_state_configure_locked, METH_VARARGS, NULL},
    {"finish", (PyCFunction)suboctave_state_finish_locked, METH_NOARGS, NULL},
    {"reset", (PyCFunction)suboctave_state_reset_locked, METH_NOARGS, NULL},
    {"process", (PyCFunction)suboctave_state_process_locked, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

AUDIODSP_LOCKED_INIT(suboctave_state_init, audiodsp_suboctave_object_t)
static PyType_Slot suboctave_state_slots[] = {
    {Py_tp_new, PyType_GenericNew},
    {Py_tp_init, suboctave_state_init_locked},
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

/* Scale a block of interleaved int16 pairs by a voice's per-channel level, the
 * loop `Mixer._get_buffer` runs in Python for any voice that is not at
 * unity: `(sample * level) >> 15`, clamp to int16. A level
 * move (`pending`) lands at the first zero crossing, or is forced at each
 * index in `forced` (ascending sample indices, where a source stretch ends),
 * exactly as the Python loop does. */
static PyObject *audiodsp_scale_s16_levels(PyObject *module, PyObject *args) {
    Py_buffer data = {0};
    long active_lo, active_hi, pending_lo, pending_hi;
    PyObject *forced_object;
    if (!PyArg_ParseTuple(args, "y*llllO:scale_s16_levels", &data, &active_lo,
        &active_hi, &pending_lo, &pending_hi, &forced_object)) {
        return NULL;
    }
    if (data.len % 2) {
        PyBuffer_Release(&data);
        PyErr_SetString(PyExc_ValueError, "data must be a whole number of int16 samples");
        return NULL;
    }
    PyObject *forced_seq = PySequence_Fast(forced_object, "forced must be a sequence");
    if (forced_seq == NULL) {
        PyBuffer_Release(&data);
        return NULL;
    }
    const Py_ssize_t forced_count = PySequence_Fast_GET_SIZE(forced_seq);
    PyObject *result = PyBytes_FromStringAndSize(NULL, data.len);
    if (result == NULL) {
        Py_DECREF(forced_seq);
        PyBuffer_Release(&data);
        return NULL;
    }
    const int16_t *in = (const int16_t *)data.buf;
    int16_t *out = (int16_t *)PyBytes_AS_STRING(result);
    const Py_ssize_t count = data.len / 2;
    long active[2] = {active_lo, active_hi};
    long mul_lo = active[0];
    long mul_hi = active[1];
    int32_t last_lo = 0, last_hi = 0;
    Py_ssize_t next_forced = 0;
    for (Py_ssize_t index = 0; index < count; index += 2) {
        while (next_forced < forced_count) {
            Py_ssize_t at = PyLong_AsSsize_t(
                PySequence_Fast_GET_ITEM(forced_seq, next_forced));
            if (at == -1 && PyErr_Occurred()) {
                Py_DECREF(result);
                Py_DECREF(forced_seq);
                PyBuffer_Release(&data);
                return NULL;
            }
            if (at >= index) {
                if (at == index && index) {
                    active[0] = pending_lo;
                    active[1] = pending_hi;
                    mul_lo = active[0];
                    mul_hi = active[1];
                    last_lo = last_hi = 0;
                }
                break;
            }
            next_forced++;
        }
        const int32_t lo = in[index];
        const int32_t hi = index + 1 < count ? in[index + 1] : 0;
        if (active[0] != pending_lo || active[1] != pending_hi) {
            if (lo == 0 || hi == 0
                || ((last_lo || last_hi)
                    && ((last_lo < 0) != (lo < 0) || (last_hi < 0) != (hi < 0)))) {
                active[0] = pending_lo;
                active[1] = pending_hi;
                mul_lo = active[0];
                mul_hi = active[1];
            } else {
                last_lo = lo;
                last_hi = hi;
            }
        }
        int32_t scaled = (int32_t)((lo * mul_lo) >> 15);
        out[index] = (int16_t)(scaled > INT16_MAX ? INT16_MAX
                               : scaled < INT16_MIN ? INT16_MIN : scaled);
        if (index + 1 < count) {
            scaled = (int32_t)((hi * mul_hi) >> 15);
            out[index + 1] = (int16_t)(scaled > INT16_MAX ? INT16_MAX
                                       : scaled < INT16_MIN ? INT16_MIN : scaled);
        }
    }
    Py_DECREF(forced_seq);
    PyBuffer_Release(&data);
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
    if (voice.len % sizeof(int32_t) ||
        voice.len / sizeof(int32_t) > UINT16_MAX) {
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

    // The same shared stage the MicroPython usermod's synth_note_into_buffer()
    // calls. A ring past its own Nyquist leaves the voice and the accumulator
    // as they were, as the usermod does.
    uint32_t accum = accumulator;
    (void)audiodsp_ring_modulate((int32_t *)PyBytes_AS_STRING(data),
        (const int16_t *)waveform.buf, waveform_start, waveform_end, dds_rate,
        &accum, (uint16_t)duration, 16);
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

// FNV-1a 64 over a buffer, continuing from `seed`: the digest the pump keeps
// over every byte it pulls and every byte it drains. audiopump.py calls this
// once per block; a Python loop over the bytes would cost more than the pull.
static PyObject *audiodsp_fnv1a64(PyObject *module, PyObject *args) {
    Py_buffer input = {0};
    unsigned long long digest = 0xcbf29ce484222325ULL;
    if (!PyArg_ParseTuple(args, "y*|K:fnv1a64", &input, &digest)) return NULL;
    const uint8_t *bytes = (const uint8_t *)input.buf;
    uint64_t value = (uint64_t)digest;
    for (Py_ssize_t i = 0; i < input.len; i++) {
        value ^= bytes[i];
        value *= 0x100000001b3ULL;
    }
    PyBuffer_Release(&input);
    return PyLong_FromUnsignedLongLong((unsigned long long)value);
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
    unsigned int position, delay_samples, channels, first_channel = 0;
    double decay, mix;
    if (!PyArg_ParseTuple(args, "y*w*IIIw*w*dd|I:multitap_s16", &input,
        &delay, &position, &delay_samples, &channels, &offsets, &levels,
        &decay, &mix, &first_channel)) return NULL;
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
        (uint8_t)channels, (uint8_t)(first_channel % channels), offsets.buf,
        levels.buf, tap_count, decay, mix);
    PyBuffer_Release(&input); PyBuffer_Release(&delay);
    PyBuffer_Release(&offsets); PyBuffer_Release(&levels);
    return Py_BuildValue("(NI)", result, position);
}

static PyObject *audiodsp_pitchshift_s16(PyObject *module, PyObject *args) {
    Py_buffer input = {0}, window = {0}, overlap = {0};
    unsigned int window_samples, overlap_samples, channels, read_rate;
    unsigned int window_index, overlap_index, read_index;
    double mix;
    int freeze = 0;
    unsigned int align_span = 0, align = 0;
    int fading = 0, fading_rising = 0;
    if (!PyArg_ParseTuple(args, "y*w*w*IIIIIIId|pIIpp:pitchshift_s16", &input,
        &window, &overlap, &window_samples, &overlap_samples, &channels,
        &read_rate, &window_index, &overlap_index, &read_index, &mix,
        &freeze, &align_span, &align, &fading, &fading_rising)) return NULL;
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
        window_index, overlap_index, read_index, align, fading != 0,
        fading_rising != 0};
    audiodsp_pitchshift_process_s16((int16_t *)PyBytes_AS_STRING(result),
        input.buf, input.len / sizeof(int16_t), window.buf, window_samples,
        overlap.buf, overlap_samples, (uint8_t)channels, read_rate, mix,
        freeze != 0, align_span, &positions);
    PyBuffer_Release(&input); PyBuffer_Release(&window);
    PyBuffer_Release(&overlap);
    return Py_BuildValue("(NIIIIOO)", result, positions.window_index,
        positions.overlap_index, positions.read_index, positions.align,
        positions.fading ? Py_True : Py_False,
        positions.fading_rising ? Py_True : Py_False);
}

static PyObject *audiodsp_freeverb_s16(PyObject *module, PyObject *args) {
    Py_buffer input = {0}, comb = {0}, comb_indices = {0}, filters = {0};
    Py_buffer allpass = {0}, allpass_indices = {0};
    double roomsize, damp, mix;
    unsigned int channel_count = 1;
    if (!PyArg_ParseTuple(args, "y*w*w*w*w*w*ddd|I:freeverb_s16", &input,
        &comb, &comb_indices, &filters, &allpass, &allpass_indices,
        &roomsize, &damp, &mix, &channel_count)) return NULL;
    // One bank of combs and all-passes per channel (6dddbda87).
    const Py_ssize_t banks = channel_count == 2u ? 2 : 1;
    if (input.len % sizeof(int16_t) ||
        comb.len < banks * AUDIODSP_FREEVERB_COMB_SAMPLES *
            (Py_ssize_t)sizeof(int16_t) ||
        comb_indices.len < banks * 8 * (Py_ssize_t)sizeof(uint32_t) ||
        filters.len < banks * 8 * (Py_ssize_t)sizeof(int16_t) ||
        allpass.len < banks * AUDIODSP_FREEVERB_ALLPASS_SAMPLES *
            (Py_ssize_t)sizeof(int16_t) ||
        allpass_indices.len < banks * 4 * (Py_ssize_t)sizeof(uint32_t)) {
        PyBuffer_Release(&input); PyBuffer_Release(&comb);
        PyBuffer_Release(&comb_indices); PyBuffer_Release(&filters);
        PyBuffer_Release(&allpass); PyBuffer_Release(&allpass_indices);
        PyErr_SetString(PyExc_ValueError, "invalid freeverb state");
        return NULL;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, input.len);
    if (result != NULL) {
        audiodsp_freeverb_process_s16((int16_t *)PyBytes_AS_STRING(result),
            input.buf, input.len / sizeof(int16_t), (uint32_t)banks, comb.buf,
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
    {"scale_s16_levels", audiodsp_scale_s16_levels, METH_VARARGS, PyDoc_STR("Scale interleaved int16 pairs by a voice's float32 Q15 levels, with a pending level move landing at a zero crossing or a forced index.")},
    {"oscillator_i32", audiodsp_oscillator_i32, METH_VARARGS, NULL},
    {"oscillator_raw_i32", audiodsp_oscillator_raw_i32, METH_VARARGS, NULL},
    {"ring_multiply_i32", audiodsp_ring_multiply_i32, METH_VARARGS, NULL},
    {"apply_loudness_i32", audiodsp_apply_loudness_i32, METH_VARARGS, NULL},
    {"mixdown_i32", audiodsp_mixdown_i32, METH_VARARGS, NULL},
    {"pitch_bend", audiodsp_pitch_bend_value, METH_VARARGS, NULL},
    {"fnv1a64", audiodsp_fnv1a64, METH_VARARGS, NULL},
    {"distortion_s16", audiodsp_distortion_s16, METH_VARARGS, NULL},
    {"echo_s16", audiodsp_echo_s16, METH_VARARGS, NULL},
    {"phaser_s16", audiodsp_phaser_s16, METH_VARARGS, NULL},
    {"chorus_s16", audiodsp_chorus_s16, METH_VARARGS, NULL},
    {"multitap_s16", audiodsp_multitap_s16, METH_VARARGS, NULL},
    {"pitchshift_s16", audiodsp_pitchshift_s16, METH_VARARGS, NULL},
    {"freeverb_s16", audiodsp_freeverb_s16, METH_VARARGS, NULL},
    {"multiply_s16", audiodsp_multiply_s16, METH_VARARGS, NULL},
    {"midside_s16", audiodsp_midside_s16, METH_VARARGS, NULL},
    {"meter_db_byte", audiodsp_meter_db_byte_py, METH_VARARGS, NULL},
    {"remix_s16", audiodsp_py_remix_s16, METH_VARARGS, PyDoc_STR("Interleaved s16 native-endian channel convert between 1 and 2 channels. Optional writable dest.")},
    {"cast_into", audiodsp_py_cast_into, METH_VARARGS, PyDoc_STR("cast_into(dst, src, scale): float32 or float64 src * scale into int16 dst, rounded half away from zero and saturated. Allocates nothing.")},
    {NULL, NULL, 0, NULL},
};

static int audiodsp_exec(PyObject *module) {
    audiodsp_state_t *state = PyModule_GetState(module);
    // Before any thread can meter: the shared level table is otherwise filled
    // on first use, unguarded.
    audiodsp_meter_init();
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
    state->drift_state_type = PyType_FromModuleAndSpec(module,
        &drift_state_spec, NULL);
    if (state->drift_state_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "DriftState",
        state->drift_state_type) < 0) return -1;
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
    state->oscillator_state_type = PyType_FromModuleAndSpec(module,
        &oscillator_state_spec, NULL);
    if (state->oscillator_state_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "OscillatorState",
        state->oscillator_state_type) < 0) return -1;
    if (PyModule_AddIntConstant(module, "OSCILLATOR_FRAMES",
        AUDIODSP_OSCILLATOR_FRAMES) < 0) return -1;
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
    state->meter_state_type = PyType_FromModuleAndSpec(module,
        &meter_state_spec, NULL);
    if (state->meter_state_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "MeterState",
        state->meter_state_type) < 0) return -1;
    state->vorbis_stream_type = PyType_FromModuleAndSpec(module,
        &vorbis_stream_spec, NULL);
    if (state->vorbis_stream_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "VorbisStream",
        state->vorbis_stream_type) < 0) return -1;
    if (PyModule_AddIntConstant(module, "METER_MAX_BANDS",
        AUDIODSP_METER_MAX_BANDS) < 0) return -1;
    state->features_state_type = PyType_FromModuleAndSpec(module,
        &features_state_spec, NULL);
    if (state->features_state_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "FeaturesState",
        state->features_state_type) < 0) return -1;
    if (PyModule_AddIntConstant(module, "FEATURES_SIZE", AUDIODSP_FEATURES_SIZE) < 0) return -1;
    if (PyModule_AddIntConstant(module, "FEATURES_RATE", AUDIODSP_FEATURES_RATE) < 0) return -1;
    if (PyModule_AddIntConstant(module, "FEATURES_STEP_MS", AUDIODSP_FEATURES_STEP_MS) < 0) return -1;
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
    state->sampler_state_type = PyType_FromModuleAndSpec(module,
        &sampler_state_spec, NULL);
    if (state->sampler_state_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "SamplerState",
        state->sampler_state_type) < 0) return -1;
    if (PyModule_AddIntConstant(module, "SAMPLER_FRAMES",
        AUDIODSP_SAMPLER_FRAMES) < 0) return -1;
    if (PyModule_AddIntConstant(module, "SAMPLER_MAX_FRAMES",
        AUDIODSP_SAMPLER_MAX_FRAMES) < 0) return -1;
    if (PyModule_AddIntConstant(module, "SAMPLER_MAX_VOICES",
        AUDIODSP_SAMPLER_MAX_VOICES) < 0) return -1;
    if (PyModule_AddIntConstant(module, "SAMPLER_LOOP_NONE",
        AUDIODSP_SAMPLER_LOOP_NONE) < 0) return -1;
    if (PyModule_AddIntConstant(module, "SAMPLER_LOOP_FOREVER",
        AUDIODSP_SAMPLER_LOOP_FOREVER) < 0) return -1;
    if (PyModule_AddIntConstant(module, "SAMPLER_LOOP_UNTIL_RELEASE",
        AUDIODSP_SAMPLER_LOOP_UNTIL_RELEASE) < 0) return -1;
    if (PyModule_AddIntConstant(module, "TANK_FRAMES",
        AUDIODSP_TANK_FRAMES) < 0) return -1;
    if (PyModule_AddIntConstant(module, "TANK_LINES",
        AUDIODSP_TANK_LINES) < 0) return -1;
    if (PyModule_AddIntConstant(module, "TANK_MAX_TAPS",
        AUDIODSP_TANK_MAX_TAPS) < 0) return -1;
    state->graph_type = PyType_FromModuleAndSpec(module,
        &graph_state_spec, NULL);
    if (state->graph_type == NULL) return -1;
    if (PyModule_AddObjectRef(module, "Graph", state->graph_type) < 0) return -1;
    if (graph_add_constants(module) < 0) return -1;
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
    Py_VISIT(state->drift_state_type);
    Py_VISIT(state->ladder_state_type);
    Py_VISIT(state->oscillator_state_type);
    Py_VISIT(state->biquad_f32_state_type);
    Py_VISIT(state->allpass_f32_state_type);
    Py_VISIT(state->suboctave_state_type);
    Py_VISIT(state->convolver_state_type);
    Py_VISIT(state->meter_state_type);
    Py_VISIT(state->features_state_type);
    Py_VISIT(state->vorbis_stream_type);
    Py_VISIT(state->tank_state_type);
    Py_VISIT(state->modal_state_type);
    Py_VISIT(state->sampler_state_type);
    Py_VISIT(state->graph_type);
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
    Py_CLEAR(state->drift_state_type);
    Py_CLEAR(state->ladder_state_type);
    Py_CLEAR(state->oscillator_state_type);
    Py_CLEAR(state->biquad_f32_state_type);
    Py_CLEAR(state->allpass_f32_state_type);
    Py_CLEAR(state->suboctave_state_type);
    Py_CLEAR(state->convolver_state_type);
    Py_CLEAR(state->meter_state_type);
    Py_CLEAR(state->features_state_type);
    Py_CLEAR(state->vorbis_stream_type);
    Py_CLEAR(state->tank_state_type);
    Py_CLEAR(state->modal_state_type);
    Py_CLEAR(state->sampler_state_type);
    Py_CLEAR(state->graph_type);
    return 0;
}

static PyModuleDef_Slot audiodsp_slots[] = {
    {Py_mod_exec, audiodsp_exec},
#ifdef Py_mod_gil
    // A free-threaded build keeps the GIL off with this module loaded. Each
    // node call holds its own node's critical section (AUDIODSP_LOCKED_*
    // above) and the module's shared tables are filled at exec, so threads
    // rendering different nodes need nothing more. One node used from two
    // threads at once is unsupported, as it always was.
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
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
