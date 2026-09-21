// Runtime-neutral synth envelope state machine.
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AUDIODSP_ENVELOPE_ATTACK = 0,
    AUDIODSP_ENVELOPE_DECAY = 1,
    AUDIODSP_ENVELOPE_SUSTAIN = 2,
    AUDIODSP_ENVELOPE_RELEASE = 3,
} audiodsp_envelope_kind_t;

typedef struct {
    int16_t attack_step, decay_step, release_step;
    uint16_t attack_level, sustain_level;
} audiodsp_envelope_definition_t;

typedef struct {
    int16_t level;
    uint16_t substep;
    audiodsp_envelope_kind_t state;
} audiodsp_envelope_state_t;

void audiodsp_envelope_definition_init(audiodsp_envelope_definition_t *definition,
    uint32_t sample_rate, bool enabled, double attack_time,
    double decay_time, double release_time, double attack_level,
    double sustain_level);
void audiodsp_envelope_state_init(audiodsp_envelope_state_t *state,
    const audiodsp_envelope_definition_t *definition);
void audiodsp_envelope_state_release(audiodsp_envelope_state_t *state);
void audiodsp_envelope_state_reattack(audiodsp_envelope_state_t *state);
void audiodsp_envelope_state_step(audiodsp_envelope_state_t *state,
    const audiodsp_envelope_definition_t *definition, size_t sample_count);

