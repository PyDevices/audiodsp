// Runtime-neutral flanger sample processing.
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t write_pos[2];
    uint32_t lfo_phase[2];
    // Not upstream: the mix as the output stands, in Q15, walking from
    // `mix_from` to `mix_to` over `ramp_frames` frames when `mix_ramp` is
    // set (audiodsp_flanger_process_s16). `ramp_done` frames of the walk
    // have gone; `ramp_frames` 0 is no walk. Until `mix_primed`, the next
    // mix lands at once, as a fresh node's first does; a reset clears it.
    int32_t mix_now;
    int32_t mix_from;
    int32_t mix_to;
    uint32_t ramp_done;
    uint32_t ramp_frames;
    bool mix_primed;
} audiodsp_flanger_state_t;

//: The most frames `Flanger.mix_ramp` takes: one second at 96 kHz.
#define AUDIODSP_FLANGER_MIX_RAMP_MAX 96000u

//: Forget a mix walk: the next block's mix lands at once.
void audiodsp_flanger_mix_ramp_clear(audiodsp_flanger_state_t *state);

uint32_t audiodsp_flanger_ms_to_frames_q16(double ms, uint32_t sample_rate,
    uint32_t frames);

uint32_t audiodsp_flanger_triangle(uint32_t phase);

void audiodsp_flanger_process_s16(int16_t *output, const int16_t *input,
    size_t sample_count, int16_t *delay_line, uint32_t frames,
    uint8_t channel_count, audiodsp_flanger_state_t *state,
    uint32_t delay_min, uint32_t delay_span, uint32_t phase_inc,
    int32_t feedback, int32_t mix, bool invert, uint32_t mix_ramp);
