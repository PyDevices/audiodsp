// Runtime-neutral sample playback. See audiodsp_sampler.h.
// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_sampler.h"

#include <string.h>

#include "shared/audiodsp_hot.h"

#define Q32_ONE 4294967296.0

static float clampf(float value, float low, float high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static uint32_t seconds_to_samples(float seconds, uint32_t sample_rate) {
    seconds = clampf(seconds, 0.0f, AUDIODSP_SAMPLER_MAX_STAGE);
    return (uint32_t)(seconds * (float)sample_rate + 0.5f);
}

static uint64_t step_for(const audiodsp_sampler_voice_t *voice) {
    // The one place a double appears, once per play() or pitch change: a
    // float step has 24 bits and the position has 32 of fraction, and a
    // sampler loop a thousand frames long played for a minute would drift
    // the missing eight bits into audible tuning.
    double step = (double)voice->pitch * (double)voice->rate_ratio;
    return (uint64_t)(step * Q32_ONE);
}

// Where a stereo voice's two gains sit for its level and pan: a linear
// balance law, `audiomixer.MixerVoice`'s convention, so pan -1 is silence on
// the right and pan 0 is the level on both. Mono output has one gain and
// ignores pan.
static void target_gains(const audiodsp_sampler_voice_t *voice,
    uint32_t channels, float gains[2]) {
    if (channels == 1u) {
        gains[0] = voice->level;
        gains[1] = 0.0f;
        return;
    }
    gains[0] = voice->level * (voice->pan > 0.0f ? 1.0f - voice->pan : 1.0f);
    gains[1] = voice->level * (voice->pan < 0.0f ? 1.0f + voice->pan : 1.0f);
}

void audiodsp_sampler_init(audiodsp_sampler_t *self, uint32_t sample_rate,
    uint32_t channel_count, uint32_t frames, uint32_t voice_count,
    audiodsp_sampler_voice_t *voices, float *mix) {
    self->sample_rate = sample_rate;
    self->channel_count = channel_count;
    self->frames = frames;
    self->voice_count = voice_count;
    self->voices = voices;
    self->mix = mix;
    memset(voices, 0, voice_count * sizeof(*voices));
    for (uint32_t i = 0; i < voice_count; ++i) {
        voices[i].level = 1.0f;
        voices[i].pitch = 1.0f;
        voices[i].rate_ratio = 1.0f;
        voices[i].sustain = 1.0f;
    }
}

void audiodsp_sampler_note_init(audiodsp_sampler_note_t *note) {
    memset(note, 0, sizeof(*note));
    note->pitch = 1.0f;
    note->level = 1.0f;
    note->sustain = 1.0f;
}

// --- the envelope ------------------------------------------------------------

static void enter_stage(audiodsp_sampler_voice_t *voice, uint32_t stage);

// A stage of zero length is stepped over, so `attack=0` is a note that starts
// at full level and `release=0` is a note that stops the instant it is let
// go. Each case sets the level the stage begins at exactly, so rounding in
// the one before cannot accumulate across the envelope.
static void enter_stage(audiodsp_sampler_voice_t *voice, uint32_t stage) {
    for (;;) {
        voice->stage = stage;
        switch (stage) {
            case AUDIODSP_SAMPLER_STAGE_ATTACK:
                voice->env = 0.0f;
                if (voice->attack_samples == 0u) {
                    stage = AUDIODSP_SAMPLER_STAGE_HOLD;
                    continue;
                }
                voice->stage_left = voice->attack_samples;
                voice->env_step = 1.0f / (float)voice->attack_samples;
                return;
            case AUDIODSP_SAMPLER_STAGE_HOLD:
                voice->env = 1.0f;
                voice->env_step = 0.0f;
                if (voice->hold_samples == 0u) {
                    stage = AUDIODSP_SAMPLER_STAGE_DECAY;
                    continue;
                }
                voice->stage_left = voice->hold_samples;
                return;
            case AUDIODSP_SAMPLER_STAGE_DECAY:
                voice->env = 1.0f;
                if (voice->decay_samples == 0u) {
                    stage = AUDIODSP_SAMPLER_STAGE_SUSTAIN;
                    continue;
                }
                voice->stage_left = voice->decay_samples;
                voice->env_step = (voice->sustain - 1.0f) /
                    (float)voice->decay_samples;
                return;
            case AUDIODSP_SAMPLER_STAGE_SUSTAIN:
                voice->env = voice->sustain;
                voice->env_step = 0.0f;
                voice->stage_left = 0;
                return;
            case AUDIODSP_SAMPLER_STAGE_RELEASE:
                // From wherever the envelope is, not from the sustain level:
                // a note let go in its attack fades from where it got to.
                if (voice->release_samples == 0u || voice->env <= 0.0f) {
                    stage = AUDIODSP_SAMPLER_STAGE_IDLE;
                    continue;
                }
                voice->stage_left = voice->release_samples;
                voice->env_step = -voice->env / (float)voice->release_samples;
                return;
            case AUDIODSP_SAMPLER_STAGE_IDLE:
            default:
                voice->env = 0.0f;
                voice->env_step = 0.0f;
                voice->stage_left = 0;
                voice->data = NULL;
                voice->looping = false;
                return;
        }
    }
}

// What follows a timed stage when its samples run out.
static void stage_done(audiodsp_sampler_voice_t *voice) {
    switch (voice->stage) {
        case AUDIODSP_SAMPLER_STAGE_ATTACK:
            enter_stage(voice, AUDIODSP_SAMPLER_STAGE_HOLD);
            break;
        case AUDIODSP_SAMPLER_STAGE_HOLD:
            enter_stage(voice, AUDIODSP_SAMPLER_STAGE_DECAY);
            break;
        case AUDIODSP_SAMPLER_STAGE_DECAY:
            enter_stage(voice, AUDIODSP_SAMPLER_STAGE_SUSTAIN);
            break;
        case AUDIODSP_SAMPLER_STAGE_RELEASE:
        default:
            enter_stage(voice, AUDIODSP_SAMPLER_STAGE_IDLE);
            break;
    }
}

// --- the voice table -----------------------------------------------------------

bool audiodsp_sampler_play(audiodsp_sampler_t *self, uint32_t index,
    const int16_t *data, uint32_t length,
    const audiodsp_sampler_note_t *note) {
    if (index >= self->voice_count || data == NULL || length == 0u) {
        return false;
    }
    if (note->loop_mode != AUDIODSP_SAMPLER_LOOP_NONE &&
        note->loop_mode != AUDIODSP_SAMPLER_LOOP_FOREVER &&
        note->loop_mode != AUDIODSP_SAMPLER_LOOP_UNTIL_RELEASE) {
        return false;
    }
    uint32_t loop_end = note->loop_end == 0u ? length : note->loop_end;
    uint32_t loop_start = note->loop_start;
    if (note->loop_mode != AUDIODSP_SAMPLER_LOOP_NONE &&
        (loop_end > length || loop_start >= loop_end)) {
        return false;
    }

    audiodsp_sampler_voice_t *voice = &self->voices[index];
    voice->data = data;
    voice->length = length;
    voice->loop_start = loop_start;
    voice->loop_end = loop_end;
    voice->loop_mode = note->loop_mode;
    voice->looping = note->loop_mode != AUDIODSP_SAMPLER_LOOP_NONE;
    voice->position = 0;
    float rate = note->rate > 0.0f ? note->rate : (float)self->sample_rate;
    voice->rate_ratio = rate / (float)self->sample_rate;
    voice->pitch = clampf(note->pitch, AUDIODSP_SAMPLER_MIN_PITCH,
        AUDIODSP_SAMPLER_MAX_PITCH);
    voice->step = step_for(voice);

    voice->attack_samples = seconds_to_samples(note->attack, self->sample_rate);
    voice->hold_samples = seconds_to_samples(note->hold, self->sample_rate);
    voice->decay_samples = seconds_to_samples(note->decay, self->sample_rate);
    voice->release_samples = seconds_to_samples(note->release,
        self->sample_rate);
    voice->sustain = clampf(note->sustain, 0.0f, 1.0f);

    voice->level = clampf(note->level, 0.0f, 1.0f);
    voice->pan = clampf(note->pan, -1.0f, 1.0f);
    // A fresh note opens at its own level: the envelope is what fades it in,
    // and a ramp from the last note's gain would be a second, unasked one.
    target_gains(voice, self->channel_count, voice->gain);

    uint32_t delay = seconds_to_samples(note->delay, self->sample_rate);
    if (delay == 0u) {
        enter_stage(voice, AUDIODSP_SAMPLER_STAGE_ATTACK);
    } else {
        voice->stage = AUDIODSP_SAMPLER_STAGE_DELAY;
        voice->stage_left = delay;
        voice->env = 0.0f;
        voice->env_step = 0.0f;
    }
    return true;
}

void audiodsp_sampler_release(audiodsp_sampler_t *self, uint32_t index) {
    if (index >= self->voice_count) {
        return;
    }
    audiodsp_sampler_voice_t *voice = &self->voices[index];
    switch (voice->stage) {
        case AUDIODSP_SAMPLER_STAGE_IDLE:
        case AUDIODSP_SAMPLER_STAGE_RELEASE:
            return;
        case AUDIODSP_SAMPLER_STAGE_DELAY:
            // Let go before it was heard: there is nothing to fade.
            enter_stage(voice, AUDIODSP_SAMPLER_STAGE_IDLE);
            return;
        default:
            break;
    }
    if (voice->loop_mode == AUDIODSP_SAMPLER_LOOP_UNTIL_RELEASE) {
        voice->looping = false;
    }
    enter_stage(voice, AUDIODSP_SAMPLER_STAGE_RELEASE);
}

void audiodsp_sampler_stop(audiodsp_sampler_t *self, uint32_t index) {
    if (index >= self->voice_count) {
        return;
    }
    enter_stage(&self->voices[index], AUDIODSP_SAMPLER_STAGE_IDLE);
}

bool audiodsp_sampler_playing(const audiodsp_sampler_t *self, uint32_t index) {
    if (index >= self->voice_count) {
        return false;
    }
    return self->voices[index].stage != AUDIODSP_SAMPLER_STAGE_IDLE;
}

void audiodsp_sampler_set_level(audiodsp_sampler_t *self, uint32_t index,
    float level) {
    if (index < self->voice_count) {
        self->voices[index].level = clampf(level, 0.0f, 1.0f);
    }
}

void audiodsp_sampler_set_pan(audiodsp_sampler_t *self, uint32_t index,
    float pan) {
    if (index < self->voice_count) {
        self->voices[index].pan = clampf(pan, -1.0f, 1.0f);
    }
}

void audiodsp_sampler_set_pitch(audiodsp_sampler_t *self, uint32_t index,
    float pitch) {
    if (index < self->voice_count) {
        audiodsp_sampler_voice_t *voice = &self->voices[index];
        voice->pitch = clampf(pitch, AUDIODSP_SAMPLER_MIN_PITCH,
            AUDIODSP_SAMPLER_MAX_PITCH);
        voice->step = step_for(voice);
    }
}

// --- the render ----------------------------------------------------------------

// One buffer value by frame index, with the voice's wrap rules: past the
// loop end while looping comes round to the loop start, before frame 0 while
// looping comes round to the loop end (a single-cycle loop's previous sample
// is its last), and anything else outside the buffer is silence.
static inline float tap(const audiodsp_sampler_voice_t *voice, int64_t i) {
    if (voice->looping) {
        int64_t length = (int64_t)voice->loop_end - (int64_t)voice->loop_start;
        if (i >= (int64_t)voice->loop_end) {
            i -= length;
            if (i >= (int64_t)voice->loop_end) {
                i = (int64_t)voice->loop_start +
                    (i - (int64_t)voice->loop_start) % length;
            }
        } else if (i < 0) {
            i += length;
        }
    }
    if (i < 0 || i >= (int64_t)voice->length) {
        return 0.0f;
    }
    return (float)voice->data[i];
}

// 4-point, 3rd-order Hermite, x-form: passes through y0 at x = 0 and y1 at
// x = 1, so an integer position reads the buffer's own value.
static inline float hermite(float ym1, float y0, float y1, float y2, float x) {
    float c1 = 0.5f * (y1 - ym1);
    float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
    return ((c3 * x + c2) * x + c1) * x + y0;
}

static inline void wrap(audiodsp_sampler_voice_t *voice) {
    uint64_t end = (uint64_t)voice->loop_end << 32;
    if (voice->position < end) {
        return;
    }
    uint64_t start = (uint64_t)voice->loop_start << 32;
    uint64_t length = end - start;
    voice->position -= length;
    if (voice->position >= end) {
        voice->position = start + (voice->position - start) % length;
    }
}

AUDIODSP_HOT
static void render_voice(audiodsp_sampler_t *self,
    audiodsp_sampler_voice_t *voice) {
    const uint32_t frames = self->frames;
    const uint32_t channels = self->channel_count;
    float *mix = self->mix;

    float target[2];
    target_gains(voice, channels, target);
    float g0 = voice->gain[0];
    float g1 = voice->gain[1];
    const float per_frame = 1.0f / (float)frames;
    const float dg0 = (target[0] - g0) * per_frame;
    const float dg1 = (target[1] - g1) * per_frame;

    uint32_t i = 0;
    // The delay stage is silence that does not read the buffer: swallow it
    // in one step rather than a sample at a time.
    while (i < frames && voice->stage == AUDIODSP_SAMPLER_STAGE_DELAY) {
        uint32_t run = frames - i;
        if (run > voice->stage_left) {
            run = voice->stage_left;
        }
        i += run;
        voice->stage_left -= run;
        g0 += dg0 * (float)run;
        g1 += dg1 * (float)run;
        if (voice->stage_left == 0u) {
            enter_stage(voice, AUDIODSP_SAMPLER_STAGE_ATTACK);
        }
    }

    const int16_t *data = voice->data;
    for (; i < frames; ++i) {
        const uint32_t index = (uint32_t)(voice->position >> 32);
        const float x = (float)(uint32_t)voice->position *
            (1.0f / 4294967296.0f);
        const uint32_t limit = voice->looping ? voice->loop_end : voice->length;
        float sample;
        if (index >= 1u && index + 2u < limit) {
            sample = hermite((float)data[index - 1u], (float)data[index],
                (float)data[index + 1u], (float)data[index + 2u], x);
        } else {
            sample = hermite(tap(voice, (int64_t)index - 1),
                tap(voice, (int64_t)index), tap(voice, (int64_t)index + 1),
                tap(voice, (int64_t)index + 2), x);
        }
        sample *= voice->env;
        if (channels == 2u) {
            mix[2u * i] += sample * g0;
            mix[2u * i + 1u] += sample * g1;
        } else {
            mix[i] += sample * g0;
        }
        g0 += dg0;
        g1 += dg1;

        if (voice->stage != AUDIODSP_SAMPLER_STAGE_SUSTAIN) {
            voice->env += voice->env_step;
            if (--voice->stage_left == 0u) {
                stage_done(voice);
                if (voice->stage == AUDIODSP_SAMPLER_STAGE_IDLE) {
                    break;
                }
            }
        }

        voice->position += voice->step;
        if (voice->looping) {
            wrap(voice);
        } else if ((voice->position >> 32) >= voice->length) {
            enter_stage(voice, AUDIODSP_SAMPLER_STAGE_IDLE);
            break;
        }
    }
    // The ramp lands exactly where it was asked to, whatever the rounding of
    // `frames` increments did on the way.
    voice->gain[0] = target[0];
    voice->gain[1] = target[1];
}

AUDIODSP_HOT
uint32_t audiodsp_sampler_render_s16(audiodsp_sampler_t *self, int16_t *out) {
    const uint32_t words = self->frames * self->channel_count;
    uint32_t sounding = 0;
    bool any = false;
    for (uint32_t v = 0; v < self->voice_count; ++v) {
        audiodsp_sampler_voice_t *voice = &self->voices[v];
        if (voice->stage == AUDIODSP_SAMPLER_STAGE_IDLE) {
            continue;
        }
        if (!any) {
            memset(self->mix, 0, words * sizeof(float));
            any = true;
        }
        render_voice(self, voice);
        if (voice->stage != AUDIODSP_SAMPLER_STAGE_IDLE) {
            ++sounding;
        }
    }
    if (!any) {
        memset(out, 0, words * sizeof(int16_t));
        return 0;
    }
    const float *mix = self->mix;
    for (uint32_t n = 0; n < words; ++n) {
        float x = mix[n];
        int32_t q = x >= 0.0f ? (int32_t)(x + 0.5f) : (int32_t)(x - 0.5f);
        if (q > 32767) {
            q = 32767;
        } else if (q < -32768) {
            q = -32768;
        }
        out[n] = (int16_t)q;
    }
    return sounding;
}
