// Runtime-neutral octave divider. See audioif_suboctave.h for provenance.
// SPDX-License-Identifier: MIT

#include "shared/audioif_fp_contract.h"

#include <stddef.h>

#include "shared/audioif_suboctave.h"

// A guitar's top note is around 1.3 kHz and a bass guitar's open E is 41 Hz,
// so the lockout has to be shorter than the shortest period the node will be
// asked to track. 1 ms is a period of 1 kHz: below that the divider counts
// every fundamental cycle, and it still swallows the second-harmonic
// crossings that ride on a low note, which is the whole point of having it.
#define AUDIOIF_SUBOCTAVE_DEFAULT_HOLD_MS 1.0f

// About -40 dBFS. Low enough that a note's decay keeps dividing well past the
// point it stops being useful, high enough that converter noise and the hum
// under a single-coil pickup never clock the chain.
#define AUDIOIF_SUBOCTAVE_DEFAULT_THRESHOLD 328

void audioif_suboctave_config_init(audioif_suboctave_config_t *config,
    uint32_t sample_rate) {
    config->sample_rate = sample_rate < 1u ? 1u : sample_rate;
    config->channel_count = 2;
    config->order = 1;
    config->mix = 32768;
    config->threshold = AUDIOIF_SUBOCTAVE_DEFAULT_THRESHOLD;
    config->hold_ms = AUDIOIF_SUBOCTAVE_DEFAULT_HOLD_MS;
    config->hold_frames = 0;
    audioif_suboctave_config_finish(config);
}

void audioif_suboctave_configure(audioif_suboctave_config_t *config,
    audioif_suboctave_option_t option, float value) {
    switch (option) {
        case AUDIOIF_SUBOCTAVE_OPT_ORDER:
            config->order = value >= 2.0f ? 2u : 1u;
            break;
        case AUDIOIF_SUBOCTAVE_OPT_MIX:
            if (value <= 0.0f) {
                config->mix = 0;
            } else if (value >= 1.0f) {
                config->mix = 32768;
            } else {
                // Scaling by 32768 rather than 32767 keeps a full wet mix a
                // gain of one to within an LSB and keeps the shift a shift,
                // the same trade audioif_multiply.c makes.
                config->mix = (int32_t)(value * 32768.0f + 0.5f);
            }
            break;
        case AUDIOIF_SUBOCTAVE_OPT_THRESHOLD:
            if (value <= 0.0f) {
                // Zero is a bare zero-crossing detector, with no hysteresis
                // at all. It is reachable on purpose: a class that has
                // already filtered the input down to a fundamental wants it.
                config->threshold = 0;
            } else if (value >= 1.0f) {
                config->threshold = 32767;
            } else {
                config->threshold = (int32_t)(value * 32768.0f + 0.5f);
            }
            break;
        case AUDIOIF_SUBOCTAVE_OPT_HOLD_MS:
            config->hold_ms = value <= 0.0f ? 0.0f : value;
            audioif_suboctave_config_finish(config);
            break;
        default:
            break;
    }
}

void audioif_suboctave_config_finish(audioif_suboctave_config_t *config) {
    float frames = config->hold_ms * (float)config->sample_rate / 1000.0f;
    if (frames <= 0.0f) {
        config->hold_frames = 0;
    } else {
        // Capped at a tenth of a second. Past that the lockout stops being a
        // debounce and starts being a note gate, and a caller who wrote 1000
        // where they meant 1 would hear one flip per second rather than a
        // divider.
        const float cap = (float)config->sample_rate / 10.0f;
        if (frames > cap) {
            frames = cap;
        }
        config->hold_frames = (uint32_t)(frames + 0.5f);
    }
}

void audioif_suboctave_set_channel_count(audioif_suboctave_config_t *config,
    uint32_t channel_count) {
    config->channel_count = channel_count == 1u ? 1u : 2u;
}

void audioif_suboctave_state_init(audioif_suboctave_state_t *state) {
    audioif_suboctave_reset(state);
}

void audioif_suboctave_reset(audioif_suboctave_state_t *state) {
    state->comparator = false;
    state->flip1 = true;
    state->flip2 = true;
    state->hold = 0;
}

void audioif_suboctave_process_s16(const audioif_suboctave_config_t *config,
    audioif_suboctave_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames) {
    const int32_t wet = config->mix;
    const int32_t dry = 32768 - wet;
    const int32_t threshold = config->threshold;
    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    const bool second = config->order >= 2u;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        const int16_t *source = in + (size_t)frame * channels;
        // One divider for the whole frame, clocked by the mean of its
        // channels. Two dividers, one per channel, would count differently
        // the moment the channels differ, and the pair would come apart --
        // the two halves of a stereo signal would then be an octave down in
        // opposite polarities, which cancels in mono.
        const int32_t level = channels == 2u
            ? (((int32_t)source[0] + (int32_t)source[1]) >> 1)
            : (int32_t)source[0];
        if (state->hold > 0) {
            state->hold--;
        }
        if (!state->comparator) {
            if (level > threshold) {
                // The comparator latches whether or not the clock accepts the
                // edge: a rejected edge still has to fall past -threshold
                // before it can be offered again, or one loud cycle would
                // count twice as soon as the lockout expired mid-cycle.
                state->comparator = true;
                if (state->hold == 0) {
                    state->hold = config->hold_frames;
                    const bool before = state->flip1;
                    state->flip1 = !before;
                    if (!before) {
                        // FF1's own rising edge clocks FF2, which is what
                        // makes the second stage f/4 rather than a second
                        // copy of f/2.
                        state->flip2 = !state->flip2;
                    }
                }
            }
        } else if (level < -threshold) {
            state->comparator = false;
        }
        const bool positive = second ? state->flip2 : state->flip1;
        int16_t *destination = out + (size_t)frame * channels;
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const int32_t signal = source[channel];
            // The square is +/-1, so multiplying by it is a negate. That is
            // one multiply fewer than audioif_multiply.c's blend, and the one
            // product that lands outside int16 -- negating -32768 -- is what
            // the clamp below is for.
            const int32_t divided = positive ? signal : -signal;
            int32_t value = (dry * signal + wet * divided) >> 15;
            if (value > 32767) {
                value = 32767;
            } else if (value < -32768) {
                value = -32768;
            }
            destination[channel] = (int16_t)value;
        }
    }
}
