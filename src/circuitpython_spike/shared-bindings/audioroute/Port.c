// audioroute.Port bindings for CircuitPython.
//
// The MicroPython twin is audiodsp's src/audioroute/Port.c. Same class, same
// methods, same argument checks, in the same order -- deliberately, because
// the two bindings are hand-written and nothing holds them to each other
// (audiodsp#75 is what that costs when it slips). The one difference is that
// this copy takes no pump lock: see shared-module/audioroute/Port.h.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "shared-bindings/audioroute/Port.h"
#include "shared-bindings/audiocore/__init__.h"

#include "py/objproperty.h"
#include "py/runtime.h"
#include "shared/runtime/context_manager_helpers.h"

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

//| class Port:
//|     """A wire whose identity never changes.
//|
//|     Everything downstream of an effect takes its output once - a mixer
//|     voice, an audio output, a `Rack` reading ``child.output`` - and about
//|     twenty classes in ``audioeffects`` replace the node at the end of
//|     their graph when a Mix macro reaches 0 or leaves it. Nothing that
//|     already took the old one ever hears about it. So a component ends in a
//|     ``Port`` and hands THAT out: re-pointing the port with `play` is how
//|     the class changes what it plays, and the object the consumer holds is
//|     the same object forever.
//|
//|     The pull is zero copy - the source's own pointer, length and result,
//|     handed straight back - so a port in a chain does not move a byte and
//|     costs one function call per block."""
//|
//|     def __init__(self, source: circuitpython_typing.AudioSample) -> None:
//|         """Make a wire that plays ``source``.
//|
//|         The port adopts ``source``'s format, and `play` will then refuse
//|         anything whose sample rate, channel count, bit depth or
//|         signedness differs."""
//|         ...
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
    self->switch_to = MP_OBJ_NULL;
    self->switch_left = 0;
    self->pulled = false;
    return MP_OBJ_FROM_PTR(self);
}

//|     def play(self, sample: circuitpython_typing.AudioSample, *,
//|              loop: bool = False, after: int = 0) -> None:
//|         """Re-point the wire. Whatever holds this ``Port`` now hears
//|         ``sample``, without being told and without changing what it
//|         holds.
//|
//|         With ``after`` a number of frames, the port plays what it plays
//|         now for that many more frames and switches at the start of the
//|         next pull: time for a level the old source is walking to land
//|         before the new one takes over. Until that pull ``source`` is still
//|         the old one, and a ``play()`` replaces the switch that was
//|         waiting. A port nothing has pulled since it was pointed at what
//|         it plays now switches at once: that has not started, so there is
//|         nothing to finish. Pointed at the same source again, the port has
//|         not started anything new."""
//|         ...
//
// Re-point the port. This is the whole node.
//
// The order is the MicroPython twin's, minus the lock: the protocol lookup,
// the deinit check and the format match come first, then the stores. On that
// build the lock is what makes the three stores one act against a pump thread
// mid-pull. Here there is no second thread to be mid-pull, so the stores
// stand on their own -- and the checks still come first, because a raise
// halfway through a re-point would leave the port advertising a format it is
// not playing.
//
// `play` rather than `retarget` or `source =` because that is the verb the
// whole palette already uses for "this is what you play now".
static mp_obj_t audioroute_port_play(size_t n_args, const mp_obj_t *pos_args,
    mp_map_t *kw_args) {
    enum { ARG_sample, ARG_loop, ARG_after };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sample, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_ROM_NONE} },
        { MP_QSTR_loop, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_after, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
    };
    audioroute_port_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    audiosample_check_for_deinit(&self->base);
    mp_obj_t sample_in = args[ARG_sample].u_obj;
    if (sample_in == mp_const_none) {
        mp_raise_ValueError(MP_ERROR_TEXT("a Port needs a source"));
    }
    const mp_int_t after = args[ARG_after].u_int;
    if (after < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("after must be 0 or more"));
    }
    // Refuses a rate, channel count, depth or signedness that differs. A
    // Component's output already has to match its source for the constructor
    // to accept it, so the Mix-0 bypass -- the port pointed straight at the
    // borrowed source -- passes; anything that would have been a different
    // format downstream is caught here instead of being heard.
    audiosample_must_match(&self->base, sample_in, false);
    if (after > 0 && self->pulled) {
        self->switch_to = sample_in;
        self->switch_left = (uint32_t)after;
        return mp_const_none;
    }
    self->switch_to = MP_OBJ_NULL;
    self->switch_left = 0;
    audioroute_port_switch(self, sample_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(audioroute_port_play_obj, 1, audioroute_port_play);

//|     source: circuitpython_typing.AudioSample
//|     """What the port is playing right now. Read-only: `play` is how it
//|     moves. A caller that has to tell "pointed at the component's own tail"
//|     from "pointed straight at the borrowed source" asks this - a `Rack`'s
//|     ``reset()`` is the one in the tree that does."""
//|
static mp_obj_t audioroute_port_obj_get_source(mp_obj_t self_in) {
    audioroute_port_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return self->source;
}
MP_DEFINE_CONST_FUN_OBJ_1(audioroute_port_get_source_obj,
    audioroute_port_obj_get_source);
MP_PROPERTY_GETTER(audioroute_port_source_obj,
    (mp_obj_t)&audioroute_port_get_source_obj);

//|     def deinit(self) -> None:
//|         """Release the wire. The nodes behind it belong to whatever built
//|         them and are released by that."""
//|         ...
//|
//|
// Releasing a port releases the reference, not the graph. Marking deinited
// first and dropping the source second is the twin's order; there it is one
// act under the lock, and here the two stores need no lock because nothing
// else is reading them.
static mp_obj_t audioroute_port_deinit(mp_obj_t self_in) {
    audioroute_port_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    self->source = mp_const_none;
    self->switch_to = MP_OBJ_NULL;
    self->switch_left = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audioroute_port_deinit_obj, audioroute_port_deinit);

static const mp_rom_map_elem_t audioroute_port_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audioroute_port_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audioroute_port_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_source), MP_ROM_PTR(&audioroute_port_source_obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audioroute_port_locals_dict,
    audioroute_port_locals_dict_table);

static const audiosample_p_t audioroute_port_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)audioroute_port_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)audioroute_port_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audioroute_port_type,
    MP_QSTR_Port,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audioroute_port_make_new,
    locals_dict, &audioroute_port_locals_dict,
    protocol, &audioroute_port_proto
    );
