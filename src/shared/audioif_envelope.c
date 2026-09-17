// SPDX-License-Identifier: MIT

#include "shared/audioif_fp_contract.h"

#include "shared/audioif_envelope.h"

#include <math.h>
#include <stdlib.h>

#define AUDIOIF_ENVELOPE_QUANTUM 256

static int16_t time_to_rate(uint32_t sample_rate, double time,
    int16_t difference) {
    int sample_count = (int)round(time * sample_rate);
    if (sample_count == 0) return 32767;
    int result = abs(difference * AUDIOIF_ENVELOPE_QUANTUM) / sample_count;
    if (result < 1) result = 1;
    if (result > 32767) result = 32767;
    return difference < 0 ? (int16_t)-result : (int16_t)result;
}

void audioif_envelope_definition_init(audioif_envelope_definition_t *definition,
    uint32_t sample_rate, bool enabled, double attack_time,
    double decay_time, double release_time, double attack_level,
    double sustain_level) {
    if (!enabled) {
        definition->attack_level = 32767;
        definition->sustain_level = 32767;
        definition->attack_step = 32767;
        definition->decay_step = -32767;
        definition->release_step = -32767;
        return;
    }
    definition->attack_level = (uint16_t)(32767 * attack_level);
    definition->sustain_level = (uint16_t)(32767 * sustain_level * attack_level);
    definition->attack_step = time_to_rate(sample_rate, attack_time,
        (int16_t)definition->attack_level);
    definition->decay_step = (int16_t)-time_to_rate(sample_rate, decay_time,
        (int16_t)(definition->attack_level - definition->sustain_level));
    definition->release_step = (int16_t)-time_to_rate(sample_rate, release_time,
        (int16_t)(definition->sustain_level ? definition->sustain_level :
            definition->attack_level));
}

void audioif_envelope_state_step(audioif_envelope_state_t *state,
    const audioif_envelope_definition_t *definition, size_t sample_count) {
    state->substep += sample_count;
    while (state->substep >= AUDIOIF_ENVELOPE_QUANTUM) {
        state->substep -= AUDIOIF_ENVELOPE_QUANTUM;
        switch (state->state) {
            case AUDIOIF_ENVELOPE_SUSTAIN:
                break;
            case AUDIOIF_ENVELOPE_ATTACK:
                if ((int)state->level + definition->attack_step >=
                    definition->attack_level) {
                    state->level = (int16_t)definition->attack_level;
                    state->state = AUDIOIF_ENVELOPE_DECAY;
                } else {
                    state->level += definition->attack_step;
                }
                break;
            case AUDIOIF_ENVELOPE_DECAY:
                if ((int)state->level + definition->decay_step <=
                    definition->sustain_level) {
                    state->level = (int16_t)definition->sustain_level;
                    state->state = AUDIOIF_ENVELOPE_SUSTAIN;
                } else {
                    state->level += definition->decay_step;
                }
                break;
            case AUDIOIF_ENVELOPE_RELEASE:
                if ((int)state->level + definition->release_step < 0) {
                    state->level = 0;
                } else {
                    state->level += definition->release_step;
                }
                break;
        }
    }
}

void audioif_envelope_state_init(audioif_envelope_state_t *state,
    const audioif_envelope_definition_t *definition) {
    state->level = 0;
    state->substep = 0;
    state->state = AUDIOIF_ENVELOPE_ATTACK;
    audioif_envelope_state_step(state, definition, AUDIOIF_ENVELOPE_QUANTUM);
}

void audioif_envelope_state_release(audioif_envelope_state_t *state) {
    state->state = AUDIOIF_ENVELOPE_RELEASE;
}

// Re-enter the attack phase from the CURRENT level - CircuitPython's
// re-press semantics (synthio_span_change_note: "note already playing,
// re-enter attack phase" mutates only the state, never the level).
void audioif_envelope_state_reattack(audioif_envelope_state_t *state) {
    state->state = AUDIOIF_ENVELOPE_ATTACK;
}
