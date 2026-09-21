// audioroute.SplitterTap. See SplitterTap.h for provenance.
// SPDX-License-Identifier: MIT

#include "audioroute/SplitterTap.h"

#include <string.h>

#include "cp_compat/context_manager_helpers.h"
#include "py/runtime.h"
#include "shared/audiodsp_pump_lock.h"

static audioio_get_buffer_result_t audioroute_splitter_tap_get_buffer(
    mp_obj_t self_in, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;
    audioroute_splitter_tap_obj_t *tap = MP_OBJ_TO_PTR(self_in);
    audioroute_splitter_obj_t *self = MP_OBJ_TO_PTR(tap->owner);
    if (audiodsp_splitter_starved(&self->state, tap->index)) {
        audioroute_splitter_pull(self);
    }
    uint32_t start = 0;
    const uint32_t run = audiodsp_splitter_take(&self->state, tap->index,
        &start);
    if (run == 0) {
        // Still nothing: the source is dry, or another tap has already read
        // ahead of what one pull could supply. Hand out silence and let the
        // branch stay in step rather than stalling the graph.
        memset(self->silence, 0, sizeof(self->silence));
        *buffer = (uint8_t *)self->silence;
        *buffer_length = AUDIODSP_SPLITTER_CHUNK_FRAMES * 2u *
            tap->base.channel_count;
        return GET_BUFFER_MORE_DATA;
    }
    if (tap->base.channel_count == 2u) {
        *buffer = (uint8_t *)&self->state.ring[start * 2u];
        *buffer_length = run * 4u;
        return GET_BUFFER_MORE_DATA;
    }
    for (uint32_t frame = 0; frame < run; ++frame) {
        tap->mono[frame] = self->state.ring[
            ((start + frame) % AUDIODSP_SPLITTER_RING_FRAMES) * 2u];
    }
    *buffer = (uint8_t *)tap->mono;
    *buffer_length = run * 2u;
    return GET_BUFFER_MORE_DATA;
}

static void audioroute_splitter_tap_reset_buffer(mp_obj_t self_in,
    bool single_channel_output, uint8_t channel) {
    // Deliberately nothing. The cursors belong to the Splitter and the other
    // taps are still reading from them; rewinding one branch mid-stream would
    // desynchronise the rest.
    (void)self_in;
    (void)single_channel_output;
    (void)channel;
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
static mp_obj_t audioroute_splitter_tap_deinit(mp_obj_t self_in) {
    audioroute_splitter_tap_obj_t *self = MP_OBJ_TO_PTR(self_in);
    // Two stores, and the window between them is a wild dereference: the
    // funnel's deinit guard has already let the pull in, and get_buffer then
    // does MP_OBJ_TO_PTR(tap->owner) on a value that is about to become
    // mp_const_none. Marking deinited and dropping the owner is one act.
    audiodsp_pump_lock_acquire();
    audiosample_mark_deinit(&self->base);
    self->owner = mp_const_none;
    audiodsp_pump_lock_release();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioroute_splitter_tap_deinit_obj, audioroute_splitter_tap_deinit);

static const mp_rom_map_elem_t audioroute_splitter_tap_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audioroute_splitter_tap_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    AUDIOSAMPLE_FIELDS,
};
static MP_DEFINE_CONST_DICT(audioroute_splitter_tap_locals,
    audioroute_splitter_tap_locals_table);

static const audiosample_p_t audioroute_splitter_tap_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = audioroute_splitter_tap_reset_buffer,
    .get_buffer = audioroute_splitter_tap_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audioroute_splitter_tap_type,
    MP_QSTR_SplitterTap,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    attr, cp_compat_attr,
    locals_dict, &audioroute_splitter_tap_locals,
    protocol, &audioroute_splitter_tap_proto
    );
