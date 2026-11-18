// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
// SPDX-License-Identifier: MIT

#pragma once

#include "shared-module/audiovorbis/VorbisDecoder.h"

extern const mp_obj_type_t audiovorbis_vorbisdecoder_type;

void common_hal_audiovorbis_vorbisdecoder_construct(audiovorbis_vorbisdecoder_obj_t *self,
    mp_obj_t stream, uint8_t *buffer, size_t buffer_size);
void common_hal_audiovorbis_vorbisdecoder_set_file(audiovorbis_vorbisdecoder_obj_t *self,
    mp_obj_t stream);
void common_hal_audiovorbis_vorbisdecoder_deinit(audiovorbis_vorbisdecoder_obj_t *self);
mp_float_t common_hal_audiovorbis_vorbisdecoder_get_rms_level(audiovorbis_vorbisdecoder_obj_t *self);
uint32_t common_hal_audiovorbis_vorbisdecoder_get_samples_decoded(audiovorbis_vorbisdecoder_obj_t *self);
