// audioroute.Port. See Port.h for what it is for.
// SPDX-License-Identifier: MIT

#include "audioroute/Port.h"

#include "cp_compat/context_manager_helpers.h"
#include "py/runtime.h"
#include "shared/audiodsp_pump_lock.h"

// Copy the format the port advertises off whatever it is about to play.
//
// A consumer reads these through `audiosample_get_buffer_structure`, and the
// port hands back the SOURCE's buffer rather than one of its own, so
// `max_buffer_length` has to be the source's or a consumer sizing scratch
// from it would be short. `play()` refuses a source whose rate, channel
// count, depth or signedness differ, so those four never actually move --
// they are copied here for the construction case only.
static void audioroute_port_adopt_format(audioroute_port_obj_t *self,
    const audiosample_base_t *sample) {
    self->base.sample_rate = sample->sample_rate;
    self->base.max_buffer_length = sample->max_buffer_length;
    self->base.bits_per_sample = sample->bits_per_sample;
    self->base.channel_count = sample->channel_count;
    self->base.samples_signed = sample->samples_signed;
    self->base.single_buffer = sample->single_buffer;
}

static mp_obj_t audioroute_port_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_source };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_source, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // A port with nothing behind it has no format to advertise and nothing to
    // hand back, and there is no case for one: it is built around the node a
    // class has just made. Refused at the door rather than carrying a "not
    // playing yet" state into the pull.
    if (args[ARG_source].u_obj == mp_const_none) {
        mp_raise_ValueError(MP_ERROR_TEXT("a Port needs a source"));
    }
    audiosample_base_t *sample = audiosample_check(args[ARG_source].u_obj);
    audiosample_check_for_deinit(sample);

    audioroute_port_obj_t *self = mp_obj_malloc(audioroute_port_obj_t, type);
    audioroute_port_adopt_format(self, sample);
    self->source = args[ARG_source].u_obj;
    // mp_obj_malloc does not zero, and a garbage byte here would be a port
    // that refuses its first pull for no reason anybody could reproduce.
    self->in_pull = false;
    return MP_OBJ_FROM_PTR(self);
}

// Re-point the port. This is the whole node.
//
// Everything that can raise happens FIRST, on the interpreter thread where
// raising is free -- the protocol lookup, the deinit check and the format
// match. Then the lock, then the stores, then the unlock. That is the pump
// lock's contract verbatim (shared/audiodsp_pump_lock.h), and the reason a
// re-point does not need a park: a pull in flight sees the whole old source
// or the whole new one.
//
// `play` rather than `retarget` or `source =` because that is the verb the
// whole palette already uses for "this is what you play now" -- every node
// audiodsp ports and every node it wrote has one.
static mp_obj_t audioroute_port_play(mp_obj_t self_in, mp_obj_t sample_in) {
    audioroute_port_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    if (sample_in == mp_const_none) {
        mp_raise_ValueError(MP_ERROR_TEXT("a Port needs a source"));
    }
    // Refuses a rate, channel count, depth or signedness that differs. A
    // Component's output already has to match its source for the constructor
    // to accept it, so the Mix-0 bypass -- the port pointed straight at the
    // borrowed source -- passes; anything that would have been a different
    // format downstream is caught here instead of being heard.
    audiosample_must_match(&self->base, sample_in, false);
    const audiosample_base_t *sample = MP_OBJ_TO_PTR(sample_in);
    const uint32_t max_buffer_length = sample->max_buffer_length;
    const bool single_buffer = sample->single_buffer;

    audiodsp_pump_lock_acquire();
    self->source = sample_in;
    self->base.max_buffer_length = max_buffer_length;
    self->base.single_buffer = single_buffer;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audioroute_port_play_obj,
    audioroute_port_play);

// Zero copy, and that is the point: the source's own pointer, its own length
// and its own result, handed straight back. A port that copied a block would
// cost 1 KB of memmove per block on every class in the palette, and would
// have to own a buffer big enough for the biggest source anyone ever points
// it at.
static audioio_get_buffer_result_t audioroute_port_get_buffer(
    mp_obj_t self_in, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    audioroute_port_obj_t *self = MP_OBJ_TO_PTR(self_in);
    // One read of one word. Whatever `play()` does to it, this sees one of
    // the two values and never half of either.
    mp_obj_t source = self->source;
    if (source == mp_const_none) {
        *buffer = NULL;
        *buffer_length = 0;
        return GET_BUFFER_ERROR;
    }
    // The loop guard. See reset_buffer below for the other door into it --
    // and the two share one flag, because a port cannot legitimately be
    // inside itself by either route.
    //
    // Refusing costs one byte and one branch. What comes back is
    // GET_BUFFER_ERROR with no buffer, which every node in the palette
    // already turns into zeros -- so the loop falls silent at the port, the
    // fault register says why, and the pump stops on the next block with a
    // reason instead of dying in the middle of one.
    //
    // Published unconditionally, unlike the deinit fault in audiocore's
    // funnel, which publishes only from the pump's own thread. The
    // distinction is real: the control path legitimately pulls a released
    // node (a Rack's deinit stops each child in turn), and it never
    // legitimately closes a loop.
    if (self->in_pull) {
        audiodsp_pump_fault_set(AUDIODSP_PUMP_FAULT_LOOP);
        *buffer = NULL;
        *buffer_length = 0;
        return GET_BUFFER_ERROR;
    }
    // Through the funnel, not through the protocol directly: the funnel is
    // where the deinit guard and the fault register live, so a class that
    // releases the node behind its port stops the pump with a published
    // reason instead of reading freed buffers.
    self->in_pull = true;
    const audioio_get_buffer_result_t got = audiosample_get_buffer(source,
        single_channel_output, channel, buffer, buffer_length);
    self->in_pull = false;
    return got;
}

// Forwarded, and it has to be.
//
// Before the port, a consumer held the tail node itself and `MixerVoice.play()`
// rewound THAT. If the port swallowed the rewind, a delay line or a filter
// would start a playback with the last take still in it, and the bytes would
// move -- which is the one thing a pass-through is not allowed to do.
//
// What this does NOT do is walk into a component's borrowed source on its own
// account: the port forwards one step, to whatever it is playing, and if that
// is the borrowed source then the consumer asked for exactly what it would
// have got by holding the source directly.
static void audioroute_port_reset_buffer(mp_obj_t self_in,
    bool single_channel_output, uint8_t channel) {
    audioroute_port_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t source = self->source;
    if (source == mp_const_none) {
        return;
    }
    // The loop guard, again, and THIS is the door the common mistake comes
    // through. `self._output = Wrap(self._output)` reads like "put a wrapper
    // on the end"; what it builds is a port whose source leads back to the
    // port. The pull finds it only when a node in the ring runs out of
    // buffer, but a LOOPING mixer voice rewinds its source first -- so
    // `Mixer -> port -> Mixer` recurses through reset_buffer four blocks
    // before get_buffer ever sees it, and that recursion is the crash: on a
    // desktop a stack overflow, on a board the pump thread never returning.
    // Guarding only the pull leaves that one in place.
    if (self->in_pull) {
        audiodsp_pump_fault_set(AUDIODSP_PUMP_FAULT_LOOP);
        return;
    }
    self->in_pull = true;
    audiosample_reset_buffer(source, single_channel_output, channel);
    self->in_pull = false;
}

// What the port is playing, for a caller that needs to tell "pointed at the
// component's own tail" from "pointed straight at the borrowed source" -- a
// Rack's reset() is the one in the tree that does.
static mp_obj_t audioroute_port_obj_get_source(mp_obj_t self_in) {
    audioroute_port_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return self->source;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioroute_port_get_source_obj,
    audioroute_port_obj_get_source);
MP_PROPERTY_GETTER(audioroute_port_source_obj,
    (mp_obj_t)&audioroute_port_get_source_obj);

// Releasing a port releases the reference, not the graph: the nodes behind it
// belong to the class that built them and are released by that class's own
// walk. Marking deinited first and dropping the source second is one act
// under the lock, for the same reason `SplitterTap.deinit` is -- the window
// between the two stores is a pull that has already passed the guard.
static mp_obj_t audioroute_port_deinit(mp_obj_t self_in) {
    audioroute_port_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiodsp_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    self->source = mp_const_none;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioroute_port_deinit_obj,
    audioroute_port_deinit);

static const mp_rom_map_elem_t audioroute_port_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audioroute_port_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audioroute_port_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_source), MP_ROM_PTR(&audioroute_port_source_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audioroute_port_locals,
    audioroute_port_locals_table);

static const audiosample_p_t audioroute_port_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = audioroute_port_reset_buffer,
    .get_buffer = audioroute_port_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audioroute_port_type,
    MP_QSTR_Port,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audioroute_port_make_new,
    attr, cp_compat_attr,
    locals_dict, &audioroute_port_locals,
    protocol, &audioroute_port_proto
    );
