// audioroute.Splitter bindings for CircuitPython.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "shared-bindings/audioroute/Splitter.h"
#include "shared-bindings/audioroute/SplitterTap.h"

#include "shared/runtime/context_manager_helpers.h"
#include "shared-bindings/audiocore/__init__.h"
#include "shared-bindings/audioroute/SplitterTap.h"
#include "shared-bindings/util.h"
#include "py/runtime.h"

//| class Splitter:
//|     """Fan one audio stream out to several parallel branches.
//|
//|     Every tap reads the same stream at its own pace over a shared ring.
//|     Whichever one is pulled first refills the ring; the others read what it
//|     wrote. A branch that nobody reads must not wedge the ring, so writing
//|     past a laggard's cursor drags it forward: that branch skips ahead rather
//|     than stalling the graph."""
//|
//|     def __init__(
//|         self, source: circuitpython_typing.AudioSample, taps: int = 2
//|     ) -> None:
//|         """Split ``source`` into ``taps`` branches, 1 to 4. Signed 16-bit
//|         stereo only.
//|
//|         Every tap exists from the moment the Splitter does, whether or not
//|         anything asks for it: the ring drops what an unread tap never
//|         collects, so a branch built later would begin mid-stream."""
//|         ...
static mp_obj_t audioroute_splitter_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_source, ARG_taps };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_source, MP_ARG_REQUIRED | MP_ARG_OBJ, {} },
        { MP_QSTR_taps, MP_ARG_INT, {.u_int = 2} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_obj_t source = args[ARG_source].u_obj;
    audiosample_base_t *sample = audiosample_check(source);
    const mp_int_t taps = args[ARG_taps].u_int;
    if (taps < 1 || taps > (mp_int_t)AUDIOIF_SPLITTER_MAX_TAPS) {
        mp_raise_ValueError(MP_ERROR_TEXT("taps must be 1..4"));
    }

    audioroute_splitter_obj_t *self =
        mp_obj_malloc(audioroute_splitter_obj_t, type);
    self->deinited = false;
    self->source = source;
    self->pending = NULL;
    self->pending_frames = 0;
    audioif_splitter_init(&self->state, (uint32_t)taps);
    if (sample->channel_count < 1 || sample->channel_count > 2) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "source channel_count must be 1 or 2"));
    }
    audioif_splitter_set_channel_count(&self->state, sample->channel_count);
    for (uint32_t index = 0; index < AUDIOIF_SPLITTER_MAX_TAPS; ++index) {
        self->taps[index] = MP_OBJ_NULL;
    }
    for (uint32_t index = 0; index < (uint32_t)taps; ++index) {
        audioroute_splitter_tap_obj_t *tap =
            mp_obj_malloc(audioroute_splitter_tap_obj_t,
                &audioroute_splitter_tap_type);
        tap->base.sample_rate = sample->sample_rate;
        tap->base.max_buffer_length = AUDIOIF_SPLITTER_CHUNK_FRAMES * 2u *
            sample->channel_count;
        tap->base.bits_per_sample = 16;
        tap->base.channel_count = sample->channel_count;
        tap->base.samples_signed = 1;
        tap->base.single_buffer = false;
        tap->owner = MP_OBJ_FROM_PTR(self);
        tap->index = index;
        self->taps[index] = MP_OBJ_FROM_PTR(tap);
    }
    return MP_OBJ_FROM_PTR(self);
}

//|     def tap(self, index: int) -> SplitterTap:
//|         """Return branch ``index``. The same object every time."""
//|         ...
//|
//|
static mp_obj_t audioroute_splitter_tap(mp_obj_t self_in, mp_obj_t index_in) {
    audioroute_splitter_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->deinited) {
        raise_deinited_error();
    }
    const mp_int_t index = mp_obj_get_int(index_in);
    if (index < 0 || (uint32_t)index >= self->state.tap_count) {
        mp_raise_ValueError(MP_ERROR_TEXT("tap index out of range"));
    }
    return self->taps[index];
}
MP_DEFINE_CONST_FUN_OBJ_2(audioroute_splitter_tap_obj,
    audioroute_splitter_tap);

// `deinit()` releases the branch: it drops the upstream chain and releases every
// tap, because the ring the taps read belongs to this object and a tap outliving
// it would be reading a ring nothing refills. The ring itself is inline here, so
// it goes when the last reference to the object does. Mirrors the MicroPython
// binding, which gained this on 2026-09-09 (audioif#58); this copy did not, and
// that gap is audioif#75.
static mp_obj_t audioroute_splitter_deinit(mp_obj_t self_in) {
    audioroute_splitter_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->deinited) {
        return mp_const_none;
    }
    self->deinited = true;
    for (uint32_t index = 0; index < AUDIOIF_SPLITTER_MAX_TAPS; ++index) {
        if (self->taps[index] != MP_OBJ_NULL) {
            audioroute_splitter_tap_obj_t *tap =
                MP_OBJ_TO_PTR(self->taps[index]);
            audiosample_mark_deinit(&tap->base);
            tap->owner = mp_const_none;
            self->taps[index] = MP_OBJ_NULL;
        }
    }
    self->source = MP_OBJ_NULL;
    // The remainder points into the released source's buffer.
    self->pending = NULL;
    self->pending_frames = 0;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(audioroute_splitter_deinit_obj,
    audioroute_splitter_deinit);

static const mp_rom_map_elem_t audioroute_splitter_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit),
      MP_ROM_PTR(&audioroute_splitter_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_tap), MP_ROM_PTR(&audioroute_splitter_tap_obj) },
};
static MP_DEFINE_CONST_DICT(audioroute_splitter_locals_dict,
    audioroute_splitter_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    audioroute_splitter_type,
    MP_QSTR_Splitter,
    MP_TYPE_FLAG_NONE,
    make_new, audioroute_splitter_make_new,
    locals_dict, &audioroute_splitter_locals_dict
    );
