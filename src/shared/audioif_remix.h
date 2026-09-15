// Runtime-neutral interleaved s16 channel convert: 1↔2.
//
// New code -- not a CircuitPython port. audiomixer.Mixer already mixes voices
// to a chosen channel_count, but that is a pull-graph node whose source must
// already match. A push path (PCMOutput.write, a Connect ring) needs the
// same arithmetic as a buffer primitive: stereo frames to (L+R)/2 mono, or
// a mono sample duplicated to both slots.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

// dst[] is frames * dst_ch samples; src[] is frames * src_ch samples.
// src_ch and dst_ch are 1 or 2. dst may alias src only when the channel
// counts match (a copy, or a no-op).
void audioif_remix_s16(int16_t *dst, const int16_t *src, uint32_t frames,
    uint32_t src_ch, uint32_t dst_ch);
