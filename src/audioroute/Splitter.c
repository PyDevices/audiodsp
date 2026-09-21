// audioroute.Splitter. See Splitter.h for provenance.
// SPDX-License-Identifier: MIT

#include "audioroute/Splitter.h"

#include "audioroute/SplitterTap.h"

#include "cp_compat/context_manager_helpers.h"
#include "cp_compat/util.h"
#include "py/runtime.h"
#include "shared/audiodsp_pump_lock.h"

void audioroute_splitter_pull(audioroute_splitter_obj_t *self) {
    if (self->deinited || self->source == MP_OBJ_NULL) {
        return;
    }
    const uint32_t width = 2u * self->state.channel_count;
    // WHAT THE LAST PULL COULD NOT FIT COMES FIRST. A source hands back what
    // it has -- a RawSample over a 9600-frame table returns all 9600 in one
    // call -- and the ring holds 8192. Writing the lot laps every cursor
    // including the one about to read, so the head is destroyed unseen and the
    // stream has a seam at 8192. The ring takes one ring's worth, this holds
    // the rest, and the source is not asked again until it is gone. Same shape
    // as MixerVoice's remaining_buffer. audiodsp#87.
    if (self->pending_frames == 0) {
        uint8_t *raw = NULL;
        uint32_t raw_bytes = 0;
        audioio_get_buffer_result_t result = audiosample_get_buffer(
            self->source, false, 0, &raw, &raw_bytes);
        if (result == GET_BUFFER_ERROR || raw == NULL) {
            return;
        }
        if (raw_bytes % width != 0) {
            return;
        }
        self->pending = raw;
        self->pending_frames = raw_bytes / width;
        if (self->pending_frames == 0) {
            return;
        }
    }
    const uint32_t taken = audiodsp_splitter_write(&self->state,
        (const int16_t *)self->pending, self->pending_frames);
    self->pending += (size_t)taken * width;
    self->pending_frames -= taken;
}

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
    if (taps < 1 || taps > (mp_int_t)AUDIODSP_SPLITTER_MAX_TAPS) {
        mp_raise_ValueError(MP_ERROR_TEXT("taps must be 1..4"));
    }

    audioroute_splitter_obj_t *self =
        mp_obj_malloc(audioroute_splitter_obj_t, type);
    self->deinited = false;
    self->source = source;
    self->pending = NULL;
    self->pending_frames = 0;
    audiodsp_splitter_init(&self->state, (uint32_t)taps);
    if (sample->channel_count < 1 || sample->channel_count > 2) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "source channel_count must be 1 or 2"));
    }
    audiodsp_splitter_set_channel_count(&self->state, sample->channel_count);
    for (uint32_t index = 0; index < AUDIODSP_SPLITTER_MAX_TAPS; ++index) {
        self->taps[index] = MP_OBJ_NULL;
    }
    // Every tap exists from the start, whether or not anything asks for it:
    // the ring drops what an unread tap never collects, so a branch built
    // late would begin mid-stream rather than at the beginning.
    for (uint32_t index = 0; index < (uint32_t)taps; ++index) {
        audioroute_splitter_tap_obj_t *tap =
            mp_obj_malloc(audioroute_splitter_tap_obj_t,
                &audioroute_splitter_tap_type);
        tap->base.sample_rate = sample->sample_rate;
        tap->base.max_buffer_length = AUDIODSP_SPLITTER_CHUNK_FRAMES * 2u *
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
static MP_DEFINE_CONST_FUN_OBJ_2(audioroute_splitter_tap_obj,
    audioroute_splitter_tap);

// `deinit()` releases the branch. Every class in `audioeffects` that fans a
// source out for a wet/dry mix builds one of these, and until now none of
// them could release it: the type's whole Python surface was `tap()`, so a
// class had to register the node as one it declines to release and its Tier 1
// row could not be measured (audiodsp#58).
//
// What it does and does not do, said plainly. The ring is
// `AUDIODSP_SPLITTER_RING_FRAMES` of int16 *inline in this object* -- the 32 KB
// the Phase 2 cost table attributes to a Splitter is the object, not a
// separate allocation -- so this cannot hand that memory back; dropping the
// last reference to the object is what does, and the GC does it. What this
// does do is release the **upstream chain** by dropping the source, and stop
// every tap, because the ring the taps read belongs to this object and
// outliving it would mean reading a ring nothing refills. A tap is marked
// released the way every other sample is, so the guard in
// `audiosample_get_buffer` raises on a pull rather than handing back stale
// audio.
static mp_obj_t audioroute_splitter_deinit(mp_obj_t self_in) {
    audioroute_splitter_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->deinited) {
        return mp_const_none;
    }
    // Up to sixteen words, and every one of them is read by a pull: the
    // splitter's own, and each tap's. One lock over the lot -- it is a dozen
    // stores of constants, which is a shorter stop than the arithmetic of a
    // single block.
    audiodsp_pump_lock_acquire();
    self->deinited = true;
    for (uint32_t index = 0; index < AUDIODSP_SPLITTER_MAX_TAPS; ++index) {
        if (self->taps[index] != MP_OBJ_NULL) {
            audioroute_splitter_tap_obj_t *tap =
                MP_OBJ_TO_PTR(self->taps[index]);
            audiosample_mark_deinit(&tap->base);
            tap->owner = mp_const_none;
            self->taps[index] = MP_OBJ_NULL;
        }
    }
    // The remainder points into the released source's buffer.
    self->source = MP_OBJ_NULL;
    self->pending = NULL;
    self->pending_frames = 0;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioroute_splitter_deinit_obj,
    audioroute_splitter_deinit);

static const mp_rom_map_elem_t audioroute_splitter_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit),
      MP_ROM_PTR(&audioroute_splitter_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_tap), MP_ROM_PTR(&audioroute_splitter_tap_obj) },
};
static MP_DEFINE_CONST_DICT(audioroute_splitter_locals,
    audioroute_splitter_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    audioroute_splitter_type,
    MP_QSTR_Splitter,
    MP_TYPE_FLAG_NONE,
    make_new, audioroute_splitter_make_new,
    locals_dict, &audioroute_splitter_locals
    );
