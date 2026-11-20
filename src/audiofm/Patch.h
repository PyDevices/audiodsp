// audiofm.Patch and audiofm.Operator: one voice of the classic six-operator
// format, held as its unpacked bytes, with every parameter readable and
// writable by name. The packing itself is the kernel's
// (shared/audiodsp_fm.c), so this binding and the CPython one agree on every
// bank.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "py/obj.h"
#include "shared/audiodsp_fm.h"

typedef struct {
    mp_obj_base_t base;
    uint8_t data[AUDIODSP_FM_PATCH_BYTES];
    mp_obj_t operators;     // a tuple of six audiofm_operator_obj_t
} audiofm_patch_obj_t;

typedef struct {
    mp_obj_base_t base;
    audiofm_patch_obj_t *patch;
    uint8_t number;         // 1..6
} audiofm_operator_obj_t;

extern const mp_obj_type_t audiofm_patch_type;
extern const mp_obj_type_t audiofm_operator_type;

// A new patch holding the initial voice.
audiofm_patch_obj_t *audiofm_patch_new(void);
