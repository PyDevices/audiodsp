// audioconvolve.Convolver. See Convolver.h for provenance.
// SPDX-License-Identifier: MIT

#include "audioconvolve/Convolver.h"

#include <string.h>

#include "py/objarray.h"
#include "cp_compat/context_manager_helpers.h"
#include "py/runtime.h"
#include "shared/audiodsp_pump_lock.h"

// Reads a bytes-like of int16 frames and reports how many whole frames of
// `channels` it holds. Rejects an odd byte count outright: silently dropping
// half a sample turns a mistyped buffer into a subtly wrong impulse.
static const int16_t *impulse_taps(mp_obj_t buffer, uint32_t channels,
    uint32_t *frames) {
    mp_buffer_info_t info;
    mp_get_buffer_raise(buffer, &info, MP_BUFFER_READ);
    if (info.len % (2u * channels) != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "impulse length must be whole int16 frames"));
    }
    *frames = (uint32_t)(info.len / (2u * channels));
    return (const int16_t *)info.buf;
}

static void convolver_allocate(audioconvolve_convolver_obj_t *self,
    uint32_t sample_rate, uint32_t max_taps, uint32_t ir_channels) {
    uint32_t partitions =
        (max_taps + AUDIODSP_CONVOLVE_FRAMES - 1u) / AUDIODSP_CONVOLVE_FRAMES;
    if (partitions < 1u) partitions = 1u;
    if (partitions > AUDIODSP_CONVOLVE_MAX_PARTITIONS) {
        mp_raise_ValueError(MP_ERROR_TEXT("impulse is too long"));
    }
    audiodsp_convolve_config_init(&self->config, sample_rate, partitions,
        ir_channels);
    size_t floats = audiodsp_convolve_float_count(&self->config);
    self->storage = m_malloc(floats * sizeof(float));
    audiodsp_convolve_state_init(&self->state, &self->config, self->storage);
}

static mp_obj_t audioconvolve_convolver_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_impulse, ARG_impulse_channels, ARG_sample_rate, ARG_max_taps,
           ARG_ir_channels, ARG_gain, ARG_mix, ARG_channel_count };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_impulse, MP_ARG_OBJ, { .u_obj = MP_ROM_NONE } },
        { MP_QSTR_impulse_channels, MP_ARG_INT, { .u_int = 1 } },
        { MP_QSTR_sample_rate, MP_ARG_INT, { .u_int = 48000 } },
        { MP_QSTR_max_taps, MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_ir_channels, MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_gain, MP_ARG_OBJ, { .u_obj = MP_ROM_NONE } },
        { MP_QSTR_mix, MP_ARG_OBJ, { .u_obj = MP_ROM_NONE } },
        { MP_QSTR_channel_count, MP_ARG_INT, { .u_int = 2 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed),
        allowed, args);

    uint32_t impulse_channels = (uint32_t)args[ARG_impulse_channels].u_int;
    if (impulse_channels < 1u || impulse_channels > 2u) {
        mp_raise_ValueError(MP_ERROR_TEXT("impulse_channels must be 1 or 2"));
    }
    uint32_t tap_frames = 0;
    const int16_t *taps = NULL;
    if (args[ARG_impulse].u_obj != mp_const_none) {
        taps = impulse_taps(args[ARG_impulse].u_obj, impulse_channels,
            &tap_frames);
    }

    // Capacity comes from `max_taps` when given, from the impulse when not,
    // and otherwise from a default that is a cabinet rather than a hall: a
    // convolver sized for a second of reverb costs a megabyte, and nobody
    // should get that by leaving an argument out.
    uint32_t max_taps = (uint32_t)args[ARG_max_taps].u_int;
    if (max_taps == 0) max_taps = tap_frames ? tap_frames : 4096u;
    if (tap_frames > max_taps) max_taps = tap_frames;

    uint32_t ir_channels = (uint32_t)args[ARG_ir_channels].u_int;
    if (ir_channels == 0) ir_channels = impulse_channels;
    if (ir_channels > 2u) {
        mp_raise_ValueError(MP_ERROR_TEXT("ir_channels must be 1 or 2"));
    }

    audioconvolve_convolver_obj_t *self =
        mp_obj_malloc(audioconvolve_convolver_obj_t, type);
    if (args[ARG_channel_count].u_int < 1 ||
        args[ARG_channel_count].u_int > 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("channel_count must be 1 or 2"));
    }
    self->base.sample_rate = (uint32_t)args[ARG_sample_rate].u_int;
    self->base.max_buffer_length = sizeof(self->buffer);
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)args[ARG_channel_count].u_int;
    self->base.samples_signed = 1;
    self->base.single_buffer = false;
    self->source = MP_OBJ_NULL;
    self->pending = NULL;
    self->pending_frames = 0;
    self->storage = NULL;

    convolver_allocate(self, self->base.sample_rate, max_taps, ir_channels);
    audiodsp_convolve_set_channel_count(&self->config,
        (uint32_t)self->base.channel_count);

    if (args[ARG_mix].u_obj != mp_const_none) {
        audiodsp_convolve_configure(&self->config, AUDIODSP_CONVOLVE_OPT_MIX,
            (float)mp_obj_get_float(args[ARG_mix].u_obj));
    }
    if (taps != NULL) {
        float gain = args[ARG_gain].u_obj != mp_const_none
            ? (float)mp_obj_get_float(args[ARG_gain].u_obj) : 1.0f;
        audiodsp_convolve_load_s16(&self->state, &self->config, taps,
            tap_frames, impulse_channels, gain);
    }
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audioconvolve_convolver_play(mp_obj_t self_in,
    mp_obj_t sample) {
    audioconvolve_convolver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_base_t *base = audiosample_check(sample);
    if (base->channel_count != self->base.channel_count) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "source channel_count does not match Convolver"));
    }
    audiodsp_pump_lock_acquire();
    self->source = sample;
    self->pending = NULL;
    self->pending_frames = 0;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audioconvolve_convolver_play_obj,
    audioconvolve_convolver_play);

static mp_obj_t audioconvolve_convolver_set(size_t n_args,
    const mp_obj_t *args, mp_map_t *kw_args) {
    audioconvolve_convolver_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    (void)n_args;
    for (size_t i = 0; i < kw_args->alloc; ++i) {
        if (!mp_map_slot_is_filled(kw_args, i)) {
            continue;
        }
        qstr name = mp_obj_str_get_qstr(kw_args->table[i].key);
        if (name != MP_QSTR_mix) {
            mp_raise_msg_varg(&mp_type_TypeError,
                MP_ERROR_TEXT("unknown Convolver option '%q'"), name);
        }
        audiodsp_convolve_configure(&self->config, AUDIODSP_CONVOLVE_OPT_MIX,
            (float)mp_obj_get_float(kw_args->table[i].value));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audioconvolve_convolver_set_obj, 1,
    audioconvolve_convolver_set);

static mp_obj_t audioconvolve_convolver_load(size_t n_args,
    const mp_obj_t *args, mp_map_t *kw_args) {
    enum { ARG_impulse, ARG_channels, ARG_gain };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_impulse, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_ROM_NONE } },
        { MP_QSTR_channels, MP_ARG_INT, { .u_int = 1 } },
        { MP_QSTR_gain, MP_ARG_OBJ, { .u_obj = MP_ROM_NONE } },
    };
    audioconvolve_convolver_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args - 1, args + 1, kw_args, MP_ARRAY_SIZE(allowed),
        allowed, parsed);

    uint32_t channels = (uint32_t)parsed[ARG_channels].u_int;
    if (channels < 1u || channels > 2u) {
        mp_raise_ValueError(MP_ERROR_TEXT("channels must be 1 or 2"));
    }
    uint32_t frames = 0;
    const int16_t *taps = impulse_taps(parsed[ARG_impulse].u_obj, channels,
        &frames);
    // Deliberately not reallocating to fit: the capacity was chosen at
    // construction and something downstream may already be pulling. An
    // impulse longer than the room made for it is a sizing mistake worth
    // hearing about, not a truncation to discover later.
    if (frames > self->config.partitions * AUDIODSP_CONVOLVE_FRAMES) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "impulse is longer than max_taps"));
    }
    float gain = parsed[ARG_gain].u_obj != mp_const_none
        ? (float)mp_obj_get_float(parsed[ARG_gain].u_obj) : 1.0f;
    audiodsp_convolve_load_s16(&self->state, &self->config, taps, frames,
        channels, gain);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audioconvolve_convolver_load_obj, 2,
    audioconvolve_convolver_load);

static mp_obj_t audioconvolve_convolver_synthesize(size_t n_args,
    const mp_obj_t *args, mp_map_t *kw_args) {
    enum { ARG_decay, ARG_damping_hz, ARG_predelay_ms, ARG_diffusion_ms,
           ARG_seed };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_decay, MP_ARG_OBJ, { .u_obj = MP_ROM_NONE } },
        { MP_QSTR_damping_hz, MP_ARG_OBJ, { .u_obj = MP_ROM_NONE } },
        { MP_QSTR_predelay_ms, MP_ARG_OBJ, { .u_obj = MP_ROM_NONE } },
        { MP_QSTR_diffusion_ms, MP_ARG_OBJ, { .u_obj = MP_ROM_NONE } },
        { MP_QSTR_seed, MP_ARG_INT, { .u_int = 1 } },
    };
    audioconvolve_convolver_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args - 1, args + 1, kw_args, MP_ARRAY_SIZE(allowed),
        allowed, parsed);

    float decay = parsed[ARG_decay].u_obj != mp_const_none
        ? (float)mp_obj_get_float(parsed[ARG_decay].u_obj) : 2.0f;
    float damping = parsed[ARG_damping_hz].u_obj != mp_const_none
        ? (float)mp_obj_get_float(parsed[ARG_damping_hz].u_obj) : 0.0f;
    float predelay = parsed[ARG_predelay_ms].u_obj != mp_const_none
        ? (float)mp_obj_get_float(parsed[ARG_predelay_ms].u_obj) : 0.0f;
    float diffusion = parsed[ARG_diffusion_ms].u_obj != mp_const_none
        ? (float)mp_obj_get_float(parsed[ARG_diffusion_ms].u_obj) : 0.0f;
    audiodsp_convolve_synthesize(&self->state, &self->config, decay, damping,
        predelay, diffusion, (uint32_t)parsed[ARG_seed].u_int);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audioconvolve_convolver_synthesize_obj, 1,
    audioconvolve_convolver_synthesize);

static mp_obj_t audioconvolve_convolver_clear(mp_obj_t self_in) {
    audioconvolve_convolver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_acquire();
    audiodsp_convolve_reset(&self->state, &self->config);
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioconvolve_convolver_clear_obj,
    audioconvolve_convolver_clear);

static mp_obj_t audioconvolve_convolver_get_taps(mp_obj_t self_in) {
    audioconvolve_convolver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->state.loaded * AUDIODSP_CONVOLVE_FRAMES);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioconvolve_convolver_get_taps_obj,
    audioconvolve_convolver_get_taps);
static MP_PROPERTY_GETTER(audioconvolve_convolver_taps_obj,
    (mp_obj_t)&audioconvolve_convolver_get_taps_obj);

static mp_obj_t audioconvolve_convolver_get_latency(mp_obj_t self_in) {
    // The loaded state, not a constant. A convolver with nothing loaded
    // passes its input through (`shared/audiodsp_convolve.h`), so it adds no
    // latency at all, and reporting a whole partition of it told any class
    // that compensates to compensate for a delay that was not there
    // (audiodsp#44's class-side clause, the half that costs no kernel change).
    audioconvolve_convolver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(
        self->state.loaded != 0 ? AUDIODSP_CONVOLVE_FRAMES : 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioconvolve_convolver_get_latency_obj,
    audioconvolve_convolver_get_latency);
static MP_PROPERTY_GETTER(audioconvolve_convolver_latency_obj,
    (mp_obj_t)&audioconvolve_convolver_get_latency_obj);

static audioio_get_buffer_result_t audioconvolve_convolver_get_buffer(
    mp_obj_t self_in, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    audioconvolve_convolver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint32_t produced = 0;
    while (produced < AUDIODSP_CONVOLVE_FRAMES) {
        if (self->pending_frames == 0) {
            if (self->source == MP_OBJ_NULL) {
                break;
            }
            uint8_t *raw = NULL;
            uint32_t raw_bytes = 0;
            audioio_get_buffer_result_t result = audiosample_get_buffer(
                self->source, false, 0, &raw, &raw_bytes);
            const uint32_t width = 2u * self->base.channel_count;
            if (result == GET_BUFFER_ERROR || raw == NULL || raw_bytes < width) {
                break;
            }
            self->pending = (const int16_t *)raw;
            self->pending_frames = raw_bytes / width;
        }
        uint32_t run = AUDIODSP_CONVOLVE_FRAMES - produced;
        if (run > self->pending_frames) {
            run = self->pending_frames;
        }
        audiodsp_convolve_process_s16(&self->config, &self->state,
            &self->buffer[produced * self->base.channel_count], self->pending,
            run);
        self->pending += run * self->base.channel_count;
        self->pending_frames -= run;
        produced += run;
    }
    // A starved chain gets silence rather than a short block, and the tail
    // stops with the source: only frames that arrive advance the convolution,
    // so a reverb does not ring on into silence after its input ends. Same
    // rule as audioecho and audiodelays, and for the same reason -- a node in
    // the middle of a live graph never reports itself finished.
    if (produced == 0) {
        memset(self->buffer, 0, sizeof(self->buffer));
        produced = AUDIODSP_CONVOLVE_FRAMES;
    }
    *buffer = (uint8_t *)self->buffer;
    *buffer_length = produced * 2u * self->base.channel_count;
    return GET_BUFFER_MORE_DATA;
}

static void audioconvolve_convolver_reset_buffer(mp_obj_t self_in,
    bool single_channel_output, uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    audioconvolve_convolver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->pending = NULL;
    self->pending_frames = 0;
    // The history goes; the impulse stays. One is audio in flight and the
    // other is a setting -- reloading a room because playback restarted would
    // be both wrong and expensive.
    audiodsp_convolve_reset(&self->state, &self->config);
}

// `deinit()` releases what this binding holds and marks the node
// deinitialised, which is what makes every guarded entry point raise
// afterwards -- `audiosample_get_buffer` and `audiosample_reset_buffer` in
// audiocore for the audio path, and the three shared properties. The node
// types audiodsp ported from CircuitPython have had this since they were
// ported; the ones audiodsp wrote itself did not, so no class built on them
// could release one and Tier 1's "deinit() releases every node the class
// built" was unmeasurable on a board (audiodsp#58, #60, #63).
//
// The inline buffers go with the object. What is cleared here is what the
// object holds a *reference* to: the upstream source, so releasing the tail
// of a chain lets the GC reclaim the rest of it, and every borrowed pointer
// into a source's buffer, so nothing dangles.
static mp_obj_t audioconvolve_convolver_deinit(mp_obj_t self_in) {
    audioconvolve_convolver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    self->source = mp_const_none;
    self->pending = NULL;
    self->pending_frames = 0;
    // Inside the lock with the rest: `storage` is the sole GC root for the
    // eight interior pointers the pull holds, and the audit's rule is that a
    // deinit() holds the lock over its whole body rather than over the part
    // someone argued was the dangerous one. audiodsp#116.
    self->storage = NULL;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioconvolve_convolver_deinit_obj, audioconvolve_convolver_deinit);

static const mp_rom_map_elem_t audioconvolve_convolver_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audioconvolve_convolver_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play),
      MP_ROM_PTR(&audioconvolve_convolver_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_set),
      MP_ROM_PTR(&audioconvolve_convolver_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_load),
      MP_ROM_PTR(&audioconvolve_convolver_load_obj) },
    { MP_ROM_QSTR(MP_QSTR_synthesize),
      MP_ROM_PTR(&audioconvolve_convolver_synthesize_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear),
      MP_ROM_PTR(&audioconvolve_convolver_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_taps),
      MP_ROM_PTR(&audioconvolve_convolver_taps_obj) },
    { MP_ROM_QSTR(MP_QSTR_latency),
      MP_ROM_PTR(&audioconvolve_convolver_latency_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audioconvolve_convolver_locals,
    audioconvolve_convolver_locals_table);

static const audiosample_p_t audioconvolve_convolver_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = audioconvolve_convolver_reset_buffer,
    .get_buffer = audioconvolve_convolver_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audioconvolve_convolver_type,
    MP_QSTR_Convolver,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audioconvolve_convolver_make_new,
    attr, cp_compat_attr,
    locals_dict, &audioconvolve_convolver_locals,
    protocol, &audioconvolve_convolver_proto
    );
