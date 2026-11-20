// audiofm.Synth -- MicroPython bindings over the six-operator FM kernel in
// shared/audiodsp_fm.c. A source with no input: notes go in through
// press() and release(), and audio comes out through the audiosample
// protocol, like synthio.Synthesizer.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "audiocore/__init__.h"
#include "audiofm/Patch.h"
#include "py/obj.h"
#include "shared/audiodsp_fm.h"

typedef struct {
    audiosample_base_t base;
    audiodsp_fm_synth_t synth;
    // Allocated by this binding and borrowed by the kernel.
    audiodsp_fm_voice_t *voices;
    audiofm_patch_obj_t *patch;
    mp_float_t bend;
    mp_float_t mod_wheel;
    uint8_t bend_range;
    bool sustain;
    int16_t buffer[AUDIODSP_FM_FRAMES * 2];
} audiofm_synth_obj_t;

extern const mp_obj_type_t audiofm_synth_type;
