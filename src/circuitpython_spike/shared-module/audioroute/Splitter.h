// audioroute.Splitter for CircuitPython. See audiodsp's src/audioroute/ for
// the MicroPython twin; the ring is the same shared/audiodsp_splitter.c in
// both, copied into this tree by audiodsp/apply_cp_patches.sh.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/audiocore/__init__.h"

#include "shared/audiodsp_splitter.h"

typedef struct {
    mp_obj_base_t obj_base;
    mp_obj_t source;
    mp_obj_t taps[AUDIODSP_SPLITTER_MAX_TAPS];
    audiodsp_splitter_state_t state;
    int16_t silence[AUDIODSP_SPLITTER_CHUNK_FRAMES * 2];
    // What one pull from the source did not fit in the ring, offered before
    // the source is asked again. Points into the source's own buffer, which
    // stays alive because `source` holds it, the same way MixerVoice keeps
    // `remaining_buffer`. audiodsp#87.
    uint8_t *pending;
    uint32_t pending_frames;
    // This type carries no `audiosample_base_t` -- it is not a sample, it hands
    // out taps -- so it cannot use audiocore's channel-count-zero convention
    // for "released" and keeps its own flag. Mirrors the MicroPython binding.
    bool deinited;
} audioroute_splitter_obj_t;

// Refill the ring from the source, if there is one. Called by whichever tap
// finds itself out of data first.
void audioroute_splitter_pull(audioroute_splitter_obj_t *self);
