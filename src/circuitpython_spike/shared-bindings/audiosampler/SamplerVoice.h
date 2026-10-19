// SPDX-License-Identifier: MIT

#pragma once

#include "shared-module/audiosampler/Sampler.h"

extern const mp_obj_type_t audiosampler_samplervoice_type;

mp_obj_t audiosampler_samplervoice_new(audiosampler_sampler_obj_t *parent,
    uint32_t index);
