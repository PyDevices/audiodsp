// Runtime-neutral sample playback. See audiodsp_sampler.h.
// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_sampler.h"

#include <string.h>

#include "shared/audiodsp_exp2.h"
#include "shared/audiodsp_hot.h"

#define Q32_ONE 4294967296.0

// Every field of a voice is paid twice per voice by every sampler, whether
// its notes use it or not, so the voice stays as small as it was before the
// filter and the LFOs: 112 bytes on a 32-bit board, 120 on a 64-bit desktop.
// What only a modulated voice needs goes in audiodsp_sampler_mod_t.
// A voice that grows past that fails to compile here, as an array of -1.
typedef char audiodsp_sampler_voice_grew[
    sizeof(audiodsp_sampler_voice_t) <= (sizeof(void *) == 8 ? 120u : 112u)
    ? 1 : -1];

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
    self->fade_samples = seconds_to_samples(AUDIODSP_SAMPLER_FADE, sample_rate);
    if (self->fade_samples == 0u) {
        self->fade_samples = 1u;
    }
    self->voices = voices;
    self->mix = mix;
    const uint32_t slots = AUDIODSP_SAMPLER_SLOTS(voice_count);
    memset(voices, 0, slots * sizeof(*voices));
    for (uint32_t i = 0; i < slots; ++i) {
        voices[i].level = 1.0f;
        voices[i].pitch = 1.0f;
        voices[i].rate_ratio = 1.0f;
        voices[i].sustain = 1.0f;
    }
}

bool audiodsp_sampler_note_modulated(const audiodsp_sampler_note_t *note) {
    audiodsp_sampler_note_t plain;
    audiodsp_sampler_note_init(&plain);
    return note->cutoff != plain.cutoff ||
           note->resonance != plain.resonance ||
           note->mod_delay != plain.mod_delay ||
           note->mod_attack != plain.mod_attack ||
           note->mod_hold != plain.mod_hold ||
           note->mod_decay != plain.mod_decay ||
           note->mod_release != plain.mod_release ||
           note->mod_sustain != plain.mod_sustain ||
           note->mod_to_pitch != plain.mod_to_pitch ||
           note->mod_to_cutoff != plain.mod_to_cutoff ||
           note->vibrato_delay != plain.vibrato_delay ||
           note->vibrato_rate != plain.vibrato_rate ||
           note->vibrato_depth != plain.vibrato_depth ||
           note->lfo_delay != plain.lfo_delay ||
           note->lfo_rate != plain.lfo_rate ||
           note->lfo_to_pitch != plain.lfo_to_pitch ||
           note->lfo_to_cutoff != plain.lfo_to_cutoff ||
           note->lfo_to_volume != plain.lfo_to_volume;
}

bool audiodsp_sampler_has_mod(const audiodsp_sampler_t *self, uint32_t index) {
    return index < self->voice_count && self->voices[index].mod != NULL;
}

void audiodsp_sampler_note_init(audiodsp_sampler_note_t *note) {
    memset(note, 0, sizeof(*note));
    note->pitch = 1.0f;
    note->level = 1.0f;
    note->sustain = 1.0f;
    note->mod_sustain = 1.0f;
}

// --- the envelope ------------------------------------------------------------

static void enter_stage(audiodsp_sampler_voice_t *voice, uint32_t stage);

// A stage of zero length is stepped over, so `attack=0` is a note that starts
// at full level and `release=0` is a note that stops the instant it is let
// go. Each case sets the level the stage begins at exactly, so rounding in
// the one before cannot accumulate across the envelope.
static void enter_stage(audiodsp_sampler_voice_t *voice, uint32_t stage) {
    for (;;) {
        voice->stage = (uint8_t)stage;
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

// --- the modulation envelope ---------------------------------------------------

// The same six stages as the amplitude envelope, linear in its own units (a
// fraction of full scale), and run once a chunk rather than once a sample.
static void mod_enter(audiodsp_sampler_voice_t *voice, uint32_t stage) {
    audiodsp_sampler_mod_t *m = voice->mod;
    for (;;) {
        m->mod_stage = stage;
        switch (stage) {
            case AUDIODSP_SAMPLER_STAGE_DELAY:
                m->mod_env = 0.0f;
                m->mod_step = 0.0f;
                if (m->mod_delay_samples == 0u) {
                    stage = AUDIODSP_SAMPLER_STAGE_ATTACK;
                    continue;
                }
                m->mod_left = m->mod_delay_samples;
                return;
            case AUDIODSP_SAMPLER_STAGE_ATTACK:
                m->mod_env = 0.0f;
                if (m->mod_attack_samples == 0u) {
                    stage = AUDIODSP_SAMPLER_STAGE_HOLD;
                    continue;
                }
                m->mod_left = m->mod_attack_samples;
                m->mod_step = 1.0f / (float)m->mod_attack_samples;
                return;
            case AUDIODSP_SAMPLER_STAGE_HOLD:
                m->mod_env = 1.0f;
                m->mod_step = 0.0f;
                if (m->mod_hold_samples == 0u) {
                    stage = AUDIODSP_SAMPLER_STAGE_DECAY;
                    continue;
                }
                m->mod_left = m->mod_hold_samples;
                return;
            case AUDIODSP_SAMPLER_STAGE_DECAY:
                m->mod_env = 1.0f;
                if (m->mod_decay_samples == 0u) {
                    stage = AUDIODSP_SAMPLER_STAGE_SUSTAIN;
                    continue;
                }
                m->mod_left = m->mod_decay_samples;
                m->mod_step = (m->mod_sustain - 1.0f) /
                    (float)m->mod_decay_samples;
                return;
            case AUDIODSP_SAMPLER_STAGE_SUSTAIN:
                m->mod_env = m->mod_sustain;
                m->mod_step = 0.0f;
                m->mod_left = 0;
                return;
            case AUDIODSP_SAMPLER_STAGE_RELEASE:
                if (m->mod_release_samples == 0u ||
                    m->mod_env <= 0.0f) {
                    stage = AUDIODSP_SAMPLER_STAGE_IDLE;
                    continue;
                }
                m->mod_left = m->mod_release_samples;
                m->mod_step = -m->mod_env /
                    (float)m->mod_release_samples;
                return;
            case AUDIODSP_SAMPLER_STAGE_IDLE:
            default:
                m->mod_stage = AUDIODSP_SAMPLER_STAGE_IDLE;
                m->mod_env = 0.0f;
                m->mod_step = 0.0f;
                m->mod_left = 0;
                return;
        }
    }
}

static void mod_advance(audiodsp_sampler_voice_t *voice, uint32_t frames) {
    audiodsp_sampler_mod_t *m = voice->mod;
    while (frames > 0u) {
        const uint32_t stage = m->mod_stage;
        if (stage == AUDIODSP_SAMPLER_STAGE_SUSTAIN ||
            stage == AUDIODSP_SAMPLER_STAGE_IDLE) {
            return;
        }
        uint32_t take = frames < m->mod_left ? frames : m->mod_left;
        m->mod_env += m->mod_step * (float)take;
        m->mod_left -= take;
        frames -= take;
        if (m->mod_left == 0u) {
            switch (stage) {
                case AUDIODSP_SAMPLER_STAGE_DELAY:
                    mod_enter(voice, AUDIODSP_SAMPLER_STAGE_ATTACK);
                    break;
                case AUDIODSP_SAMPLER_STAGE_ATTACK:
                    mod_enter(voice, AUDIODSP_SAMPLER_STAGE_HOLD);
                    break;
                case AUDIODSP_SAMPLER_STAGE_HOLD:
                    mod_enter(voice, AUDIODSP_SAMPLER_STAGE_DECAY);
                    break;
                case AUDIODSP_SAMPLER_STAGE_DECAY:
                    mod_enter(voice, AUDIODSP_SAMPLER_STAGE_SUSTAIN);
                    break;
                default:
                    mod_enter(voice, AUDIODSP_SAMPLER_STAGE_IDLE);
                    break;
            }
        }
    }
}

// --- the filter and the LFOs -----------------------------------------------------

#define SAMPLER_PI 3.14159265f

// tan(x) for 0 <= x < pi/2, as sine over cosine, each a Taylor series: the
// cutoff never goes past 0.45 of the output rate, so x stays under 1.42,
// where the dropped terms are below 2e-8. libm's tanf would differ in the
// last place between targets.
static float tan_poly(float x) {
    const float x2 = x * x;
    const float s = x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f
        + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f
        + x2 * (-1.0f / 39916800.0f))))));
    const float c = 1.0f + x2 * (-0.5f + x2 * (1.0f / 24.0f
        + x2 * (-1.0f / 720.0f + x2 * (1.0f / 40320.0f
        + x2 * (-1.0f / 3628800.0f + x2 * (1.0f / 479001600.0f))))));
    return s / c;
}

// 2^(cents / 1200).
static inline float cents_ratio(float cents) {
    return audiodsp_exp2_poly(cents * (1.0f / 1200.0f));
}

// The trapezoidal state-variable low-pass's three coefficients, for the
// voice's cutoff moved by `cents`.
static void filter_coefficients(const audiodsp_sampler_t *self,
    audiodsp_sampler_voice_t *voice, float cents) {
    audiodsp_sampler_mod_t *m = voice->mod;
    float fc = m->cutoff;
    if (cents != 0.0f) {
        fc *= cents_ratio(cents);
    }
    const float top = 0.45f * (float)self->sample_rate;
    if (fc > top) {
        fc = top;
    } else if (fc < 5.0f) {
        fc = 5.0f;
    }
    const float g = tan_poly(SAMPLER_PI * fc / (float)self->sample_rate);
    m->fa1 = 1.0f / (1.0f + g * (g + m->filter_k));
    m->fa2 = g * m->fa1;
    m->fa3 = g * m->fa2;
    m->filter_cents = cents;
    m->filter_dirty = false;
}

// Works out what the voice needs from its settings, and puts the plain
// path's step back when nothing moves the pitch any more.
static void update_mods(audiodsp_sampler_voice_t *voice) {
    audiodsp_sampler_mod_t *m = voice->mod;
    uint32_t mods = 0;
    if (m->mod_to_pitch != 0.0f || m->vibrato_depth != 0.0f ||
        m->lfo_to_pitch != 0.0f) {
        mods |= AUDIODSP_SAMPLER_MOD_PITCH;
    }
    if (m->mod_to_cutoff != 0.0f || m->lfo_to_cutoff != 0.0f) {
        mods |= AUDIODSP_SAMPLER_MOD_CUTOFF;
    }
    if (m->lfo_to_volume != 0.0f) {
        mods |= AUDIODSP_SAMPLER_MOD_VOLUME;
    }
    if (m->mod_to_pitch != 0.0f || m->mod_to_cutoff != 0.0f) {
        mods |= AUDIODSP_SAMPLER_MOD_ENV;
    }
    if (m->cutoff > 0.0f && (m->cutoff < AUDIODSP_SAMPLER_OPEN_CUTOFF ||
        m->resonance > 0.0f || (mods & AUDIODSP_SAMPLER_MOD_CUTOFF))) {
        mods |= AUDIODSP_SAMPLER_MOD_FILTER;
    } else {
        mods &= ~AUDIODSP_SAMPLER_MOD_CUTOFF;
    }
    if ((mods & AUDIODSP_SAMPLER_MOD_FILTER) &&
        !(voice->mods & AUDIODSP_SAMPLER_MOD_FILTER)) {
        m->ic1 = 0.0f;
        m->ic2 = 0.0f;
        m->filter_dirty = true;
    }
    if (!(mods & AUDIODSP_SAMPLER_MOD_PITCH)) {
        voice->step = m->step_base;
    }
    m->filter_gain = (mods & AUDIODSP_SAMPLER_MOD_FILTER) ?
        audiodsp_exp2_poly(-m->resonance * (0.5f * 0.16609640f)) : 1.0f;
    voice->mods = (uint8_t)mods;
}

// The resonance as the filter's 1/Q. Q's response at the cutoff is Q itself,
// so Q = 10^(dB/20) would be the specification read literally; that is
// 1.0 at 0 dB, a small bump, where the specification says 0 dB is not
// resonant. sqrt(10^(dB/10) - 1/2) is Butterworth (0.707) at 0 dB, within
// 0.1 dB of the literal reading from 10 dB up, and smooth between.
static float resonance_k(float db) {
    const float power = audiodsp_exp2_poly(db * 0.33219281f) - 0.5f;
    // sqrt by Newton's method from a guess that is never short, so the
    // result is the same bits everywhere.
    float q = power > 1.0f ? power : 1.0f;
    for (int n = 0; n < 24; ++n) {
        q = 0.5f * (q + power / q);
    }
    return 1.0f / q;
}

// A triangle from 0, rising, at the phase of `elapsed` frames.
static inline float triangle(uint32_t elapsed, uint32_t inc) {
    const uint32_t phase = elapsed * inc;
    const float t = (float)phase * (1.0f / 4294967296.0f);
    if (t < 0.25f) {
        return 4.0f * t;
    }
    if (t < 0.75f) {
        return 2.0f - 4.0f * t;
    }
    return 4.0f * t - 4.0f;
}

static uint32_t lfo_inc(float hz, uint32_t sample_rate) {
    float cycles = hz / (float)sample_rate;
    if (!(cycles > 0.0f)) {
        return 0;
    }
    if (cycles > 0.5f) {
        cycles = 0.5f;
    }
    return (uint32_t)((double)cycles * Q32_ONE);
}

// `base` times `factor`, in Q32.32: the factor to 24 bits, about 1e-4 cents,
// with no double on the way (a board's FPU is single precision).
static inline uint64_t scale_step(uint64_t base, float factor) {
    const uint64_t fq = (uint64_t)(factor * 16777216.0f + 0.5f);
    return (((base >> 32) * fq) << 8) + (((base & 0xffffffffu) * fq) >> 24);
}

// What play() leaves in a voice's modulation record for a note that asks for
// none of it, which is everything a voice without a record has been playing.
// The envelope's stage is left at rest: with both of its depths zero nothing
// reads it until the next play() sets it.
static void mod_plain(audiodsp_sampler_mod_t *m, uint64_t step) {
    memset(m, 0, sizeof(*m));
    m->step_base = step;
    m->filter_k = resonance_k(0.0f);
    m->filter_gain = 1.0f;
    m->mod_sustain = 1.0f;
    m->mod_gain = 1.0f;
}

void audiodsp_sampler_attach_mod(audiodsp_sampler_t *self, uint32_t index,
    audiodsp_sampler_mod_t *pair) {
    if (index >= self->voice_count || pair == NULL) {
        return;
    }
    audiodsp_sampler_voice_t *voice = &self->voices[index];
    audiodsp_sampler_voice_t *tail = &self->voices[self->voice_count + index];
    if (voice->mod != NULL) {
        return;
    }
    mod_plain(&pair[0], voice->step);
    mod_plain(&pair[1], tail->step);
    voice->mod = &pair[0];
    tail->mod = &pair[1];
}

// --- the voice table -----------------------------------------------------------

static bool audible(const audiodsp_sampler_voice_t *voice) {
    return voice->stage != AUDIODSP_SAMPLER_STAGE_IDLE &&
           voice->stage != AUDIODSP_SAMPLER_STAGE_DELAY &&
           voice->env > 0.0f;
}

// Moves what voice `index` is sounding to its tail, as a release that reaches
// zero within the fade, and leaves the voice idle. A release already due to
// finish sooner keeps its own slope. A tail still fading an older note is
// overwritten: that is a third note on one voice inside 5 ms.
static void hand_to_tail(audiodsp_sampler_t *self, uint32_t index) {
    audiodsp_sampler_voice_t *voice = &self->voices[index];
    if (audible(voice)) {
        audiodsp_sampler_voice_t *tail =
            &self->voices[self->voice_count + index];
        const uint32_t fade = self->fade_samples;
        bool keep = voice->stage == AUDIODSP_SAMPLER_STAGE_RELEASE &&
            voice->stage_left <= fade;
        // The tail keeps its own record; the voice's state is copied into it.
        audiodsp_sampler_mod_t *mod = tail->mod;
        *tail = *voice;
        tail->mod = mod;
        if (mod != NULL) {
            *mod = *voice->mod;
        }
        tail->stage = AUDIODSP_SAMPLER_STAGE_RELEASE;
        if (!keep) {
            tail->stage_left = fade;
            tail->env_step = -tail->env / (float)fade;
        }
    }
    enter_stage(voice, AUDIODSP_SAMPLER_STAGE_IDLE);
}

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

    hand_to_tail(self, index);
    audiodsp_sampler_voice_t *voice = &self->voices[index];
    voice->data = data;
    voice->length = length;
    voice->loop_start = loop_start;
    voice->loop_end = loop_end;
    voice->loop_mode = (uint8_t)note->loop_mode;
    voice->looping = note->loop_mode != AUDIODSP_SAMPLER_LOOP_NONE;
    voice->position = 0;
    float rate = note->rate > 0.0f ? note->rate : (float)self->sample_rate;
    voice->rate_ratio = rate / (float)self->sample_rate;
    voice->pitch = clampf(note->pitch, AUDIODSP_SAMPLER_MIN_PITCH,
        AUDIODSP_SAMPLER_MAX_PITCH);
    voice->step = step_for(voice);
    voice->age = 0;
    voice->mods = 0;
    // A voice with no record yet has only ever been asked for the plain
    // path, and the binding attaches one before a note that asks for more.
    if (voice->mod != NULL) {
        audiodsp_sampler_mod_t *m = voice->mod;
        m->step_base = voice->step;

        m->cutoff = note->cutoff > 0.0f ? note->cutoff : 0.0f;
        m->resonance = clampf(note->resonance, 0.0f,
            AUDIODSP_SAMPLER_MAX_RESONANCE);
        m->filter_k = resonance_k(m->resonance);
        m->mod_delay_samples = seconds_to_samples(note->mod_delay,
            self->sample_rate);
        m->mod_attack_samples = seconds_to_samples(note->mod_attack,
            self->sample_rate);
        m->mod_hold_samples = seconds_to_samples(note->mod_hold,
            self->sample_rate);
        m->mod_decay_samples = seconds_to_samples(note->mod_decay,
            self->sample_rate);
        m->mod_release_samples = seconds_to_samples(note->mod_release,
            self->sample_rate);
        m->mod_sustain = clampf(note->mod_sustain, 0.0f, 1.0f);
        const float most = AUDIODSP_SAMPLER_MAX_CENTS;
        m->mod_to_pitch = clampf(note->mod_to_pitch, -most, most);
        m->mod_to_cutoff = clampf(note->mod_to_cutoff, -most, most);
        m->vibrato_delay = seconds_to_samples(note->vibrato_delay,
            self->sample_rate);
        m->vibrato_inc = lfo_inc(note->vibrato_rate, self->sample_rate);
        m->vibrato_depth = clampf(note->vibrato_depth, -most, most);
        m->lfo_delay = seconds_to_samples(note->lfo_delay, self->sample_rate);
        m->lfo_inc = lfo_inc(note->lfo_rate, self->sample_rate);
        m->lfo_to_pitch = clampf(note->lfo_to_pitch, -most, most);
        m->lfo_to_cutoff = clampf(note->lfo_to_cutoff, -most, most);
        m->lfo_to_volume = clampf(note->lfo_to_volume, -96.0f, 96.0f);
        voice->mods = 0;
        update_mods(voice);
        m->mod_gain = m->filter_gain;
        mod_enter(voice, AUDIODSP_SAMPLER_STAGE_DELAY);
    }

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
    if (voice->mod != NULL &&
        voice->mod->mod_stage != AUDIODSP_SAMPLER_STAGE_IDLE) {
        mod_enter(voice, AUDIODSP_SAMPLER_STAGE_RELEASE);
    }
}

void audiodsp_sampler_stop(audiodsp_sampler_t *self, uint32_t index) {
    if (index >= self->voice_count) {
        return;
    }
    hand_to_tail(self, index);
}

void audiodsp_sampler_cut(audiodsp_sampler_t *self, uint32_t index) {
    if (index >= self->voice_count) {
        return;
    }
    enter_stage(&self->voices[index], AUDIODSP_SAMPLER_STAGE_IDLE);
    enter_stage(&self->voices[self->voice_count + index],
        AUDIODSP_SAMPLER_STAGE_IDLE);
}

bool audiodsp_sampler_audible(const audiodsp_sampler_t *self, uint32_t index) {
    return index < self->voice_count && audible(&self->voices[index]);
}

bool audiodsp_sampler_fading(const audiodsp_sampler_t *self, uint32_t index) {
    return index < self->voice_count &&
           self->voices[self->voice_count + index].stage !=
           AUDIODSP_SAMPLER_STAGE_IDLE;
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
        if (voice->mod != NULL) {
            voice->mod->step_base = voice->step;
        }
    }
}

void audiodsp_sampler_set_cutoff(audiodsp_sampler_t *self, uint32_t index,
    float cutoff) {
    if (index < self->voice_count && self->voices[index].mod != NULL) {
        audiodsp_sampler_voice_t *voice = &self->voices[index];
        voice->mod->cutoff = cutoff > 0.0f ? cutoff : 0.0f;
        voice->mod->filter_dirty = true;
        update_mods(voice);
    }
}

void audiodsp_sampler_set_vibrato_depth(audiodsp_sampler_t *self,
    uint32_t index, float cents) {
    if (index < self->voice_count && self->voices[index].mod != NULL) {
        audiodsp_sampler_voice_t *voice = &self->voices[index];
        voice->mod->vibrato_depth = clampf(cents, -AUDIODSP_SAMPLER_MAX_CENTS,
            AUDIODSP_SAMPLER_MAX_CENTS);
        update_mods(voice);
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

// How many frames from here, at most `most`, need none of the per-frame
// tests: frame j reads taps [index-1, index+2] all inside the buffer (or the
// loop), its stage does not end on it, and stepping past it neither wraps nor
// runs off the end. Frame j is at position + j * step; asking that
// position + (j + 1) * step stay below (limit - 2) << 32 covers all three
// position tests at once, since a frame at or past that bound is the only
// kind that can reach a tap, a wrap or the end. Zero when the next frame is
// one of those.
static uint32_t clean_run(const audiodsp_sampler_voice_t *voice,
    uint32_t most) {
    const uint64_t position = voice->position;
    if ((position >> 32) < 1u) {
        return 0;
    }
    const uint32_t limit = voice->looping ? voice->loop_end : voice->length;
    if (limit < 3u) {
        return 0;
    }
    const uint64_t bound = (uint64_t)(limit - 2u) << 32;
    const uint64_t step = voice->step;
    if (step == 0u || position + step >= bound) {
        return 0;
    }
    uint64_t run = (bound - 1u - position) / step;
    if (run > most) {
        run = most;
    }
    if (voice->stage != AUDIODSP_SAMPLER_STAGE_SUSTAIN) {
        // The frame that takes stage_left to zero ends the stage: not here.
        if (voice->stage_left <= 1u) {
            return 0;
        }
        if (run > voice->stage_left - 1u) {
            run = voice->stage_left - 1u;
        }
    }
    return (uint32_t)run;
}

// One sample, as the general path makes it: a 4-point Hermite of the taps
// around `position`.
static inline float tap_run(const int16_t *data, uint64_t position) {
    const uint32_t index = (uint32_t)(position >> 32);
    const float x = (float)(uint32_t)position * (1.0f / 4294967296.0f);
    return hermite((float)data[index - 1u], (float)data[index],
        (float)data[index + 1u], (float)data[index + 2u], x);
}

// `run` clean frames from frame `i`. The voice's state lives in locals for
// the run, so the stores into the mix (floats, which the compiler must
// assume can alias the voice's own floats) don't force it back to memory
// every frame; and two frames' Hermites are worked out before either is
// mixed, so an in-order core has the second's arithmetic to do while the
// first's is still in the pipeline. Each frame's numbers are the general
// path's, in the general path's order. Leaves the gains in `gain_run`.
static uint32_t render_run(audiodsp_sampler_t *self,
    audiodsp_sampler_voice_t *voice, uint32_t i, uint32_t run,
    float g0, float g1, float dg0, float dg1) {
    float *mix = self->mix;
    const int16_t *data = voice->data;
    const uint64_t step = voice->step;
    const bool ramp = voice->stage != AUDIODSP_SAMPLER_STAGE_SUSTAIN;
    const float env_step = voice->env_step;
    uint64_t position = voice->position;
    float env = voice->env;
    const uint32_t end = i + run;
    if (self->channel_count == 2u) {
        for (; i + 1u < end; i += 2u) {
            float a = tap_run(data, position);
            float b = tap_run(data, position + step);
            a *= env;
            mix[2u * i] += a * g0;
            mix[2u * i + 1u] += a * g1;
            g0 += dg0;
            g1 += dg1;
            if (ramp) {
                env += env_step;
            }
            b *= env;
            mix[2u * i + 2u] += b * g0;
            mix[2u * i + 3u] += b * g1;
            g0 += dg0;
            g1 += dg1;
            if (ramp) {
                env += env_step;
            }
            position += step + step;
        }
        if (i < end) {
            float a = tap_run(data, position);
            a *= env;
            mix[2u * i] += a * g0;
            mix[2u * i + 1u] += a * g1;
            g0 += dg0;
            g1 += dg1;
            if (ramp) {
                env += env_step;
            }
            position += step;
            ++i;
        }
    } else {
        for (; i < end; ++i) {
            float a = tap_run(data, position);
            a *= env;
            mix[i] += a * g0;
            g0 += dg0;
            g1 += dg1;
            if (ramp) {
                env += env_step;
            }
            position += step;
        }
    }
    voice->position = position;
    voice->env = env;
    if (ramp) {
        voice->stage_left -= run;
    }
    voice->gain_run[0] = g0;
    voice->gain_run[1] = g1;
    return i;
}

static void render_voice_mod(audiodsp_sampler_t *self,
    audiodsp_sampler_voice_t *voice);

AUDIODSP_HOT
static void render_voice(audiodsp_sampler_t *self,
    audiodsp_sampler_voice_t *voice) {
    if (voice->mods != 0u) {
        render_voice_mod(self, voice);
        return;
    }
    voice->age += self->frames;
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
        // Most of a block is frames that need none of the tests below: every
        // tap inside the buffer, no stage ending, no wrap and no end. Those
        // go through the run loop, in registers and two at a time, with the
        // same arithmetic in the same order, so the output is the same bits.
        uint32_t run = clean_run(voice, frames - i);
        if (run >= 2u) {
            i = render_run(self, voice, i, run, g0, g1, dg0, dg1);
            g0 = voice->gain_run[0];
            g1 = voice->gain_run[1];
            if (i >= frames) {
                break;
            }
        }
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

// --- the modulated voice ---------------------------------------------------------

// `run` clean frames from frame `i` into `out`, the plain path's numbers in
// its order, without the gains.
static uint32_t make_run(audiodsp_sampler_voice_t *voice, float *out,
    uint32_t i, uint32_t run) {
    const int16_t *data = voice->data;
    const uint64_t step = voice->step;
    const bool ramp = voice->stage != AUDIODSP_SAMPLER_STAGE_SUSTAIN;
    const float env_step = voice->env_step;
    uint64_t position = voice->position;
    float env = voice->env;
    const uint32_t end = i + run;
    for (; i < end; ++i) {
        out[i] = tap_run(data, position) * env;
        if (ramp) {
            env += env_step;
        }
        position += step;
    }
    voice->position = position;
    voice->env = env;
    if (ramp) {
        voice->stage_left -= run;
    }
    return i;
}

// `frames` frames of the voice, enveloped, into `out`: the plain path's
// per-frame rules (delay, taps, stages, wraps and the end), with silence
// after the voice stops.
static void make_frames(audiodsp_sampler_voice_t *voice, float *out,
    uint32_t frames) {
    uint32_t i = 0;
    while (i < frames && voice->stage == AUDIODSP_SAMPLER_STAGE_DELAY) {
        uint32_t run = frames - i;
        if (run > voice->stage_left) {
            run = voice->stage_left;
        }
        for (uint32_t j = 0; j < run; ++j) {
            out[i + j] = 0.0f;
        }
        i += run;
        voice->stage_left -= run;
        if (voice->stage_left == 0u) {
            enter_stage(voice, AUDIODSP_SAMPLER_STAGE_ATTACK);
        }
    }
    const int16_t *data = voice->data;
    for (; i < frames; ++i) {
        if (voice->stage == AUDIODSP_SAMPLER_STAGE_IDLE) {
            break;
        }
        uint32_t run = clean_run(voice, frames - i);
        if (run >= 2u) {
            i = make_run(voice, out, i, run);
            if (i >= frames) {
                break;
            }
        }
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
        out[i] = sample * voice->env;

        if (voice->stage != AUDIODSP_SAMPLER_STAGE_SUSTAIN) {
            voice->env += voice->env_step;
            if (--voice->stage_left == 0u) {
                stage_done(voice);
                if (voice->stage == AUDIODSP_SAMPLER_STAGE_IDLE) {
                    ++i;
                    break;
                }
            }
        }

        voice->position += voice->step;
        if (voice->looping) {
            wrap(voice);
        } else if ((voice->position >> 32) >= voice->length) {
            enter_stage(voice, AUDIODSP_SAMPLER_STAGE_IDLE);
            ++i;
            break;
        }
    }
    for (; i < frames; ++i) {
        out[i] = 0.0f;
    }
}

// Sets the chunk's step, filter and target volume from the modulators where
// the voice is now, and returns the target volume.
static float modulate(const audiodsp_sampler_t *self,
    audiodsp_sampler_voice_t *voice) {
    const uint32_t mods = voice->mods;
    audiodsp_sampler_mod_t *m = voice->mod;
    const uint32_t age = voice->age;
    const float env = (mods & AUDIODSP_SAMPLER_MOD_ENV) ? m->mod_env : 0.0f;
    const float vibrato = (m->vibrato_inc != 0u &&
        age >= m->vibrato_delay) ?
        triangle(age - m->vibrato_delay, m->vibrato_inc) : 0.0f;
    const float lfo = (m->lfo_inc != 0u && age >= m->lfo_delay) ?
        triangle(age - m->lfo_delay, m->lfo_inc) : 0.0f;
    if (mods & AUDIODSP_SAMPLER_MOD_PITCH) {
        float cents = env * m->mod_to_pitch +
            vibrato * m->vibrato_depth + lfo * m->lfo_to_pitch;
        cents = clampf(cents, -7200.0f, 7200.0f);
        voice->step = cents == 0.0f ? m->step_base :
            scale_step(m->step_base, cents_ratio(cents));
    }
    if (mods & AUDIODSP_SAMPLER_MOD_FILTER) {
        float cents = 0.0f;
        if (mods & AUDIODSP_SAMPLER_MOD_CUTOFF) {
            cents = clampf(env * m->mod_to_cutoff +
                lfo * m->lfo_to_cutoff, -AUDIODSP_SAMPLER_MAX_CENTS,
                AUDIODSP_SAMPLER_MAX_CENTS);
        }
        if (m->filter_dirty || cents != m->filter_cents) {
            filter_coefficients(self, voice, cents);
        }
    }
    float gain = m->filter_gain;
    if (mods & AUDIODSP_SAMPLER_MOD_VOLUME) {
        // dB to a ratio: 2^(dB * log2(10) / 20).
        gain *= audiodsp_exp2_poly(lfo * m->lfo_to_volume * 0.16609640f);
    }
    return gain;
}

AUDIODSP_HOT
static void render_voice_mod(audiodsp_sampler_t *self,
    audiodsp_sampler_voice_t *voice) {
    audiodsp_sampler_mod_t *m = voice->mod;
    const uint32_t frames = self->frames;
    const uint32_t channels = self->channel_count;
    float *mix = self->mix;
    float row[AUDIODSP_SAMPLER_CONTROL];

    float target[2];
    target_gains(voice, channels, target);
    float g0 = voice->gain[0];
    float g1 = voice->gain[1];
    const float per_frame = 1.0f / (float)frames;
    const float dg0 = (target[0] - g0) * per_frame;
    const float dg1 = (target[1] - g1) * per_frame;

    for (uint32_t c = 0; c < frames;) {
        if (voice->stage == AUDIODSP_SAMPLER_STAGE_IDLE) {
            break;
        }
        uint32_t n = frames - c;
        if (n > AUDIODSP_SAMPLER_CONTROL) {
            n = AUDIODSP_SAMPLER_CONTROL;
        }
        const float want = modulate(self, voice);
        make_frames(voice, row, n);
        if (voice->mods & AUDIODSP_SAMPLER_MOD_FILTER) {
            const float a1 = m->fa1, a2 = m->fa2, a3 = m->fa3;
            float ic1 = m->ic1, ic2 = m->ic2;
            for (uint32_t j = 0; j < n; ++j) {
                const float v3 = row[j] - ic2;
                const float v1 = a1 * ic1 + a2 * v3;
                const float v2 = ic2 + a2 * ic1 + a3 * v3;
                ic1 = 2.0f * v1 - ic1;
                ic2 = 2.0f * v2 - ic2;
                row[j] = v2;
            }
            m->ic1 = ic1;
            m->ic2 = ic2;
        }
        float gain = m->mod_gain;
        const float dgain = (want - gain) / (float)n;
        if (channels == 2u) {
            float *out = mix + 2u * c;
            for (uint32_t j = 0; j < n; ++j) {
                const float a = row[j] * gain;
                out[2u * j] += a * g0;
                out[2u * j + 1u] += a * g1;
                g0 += dg0;
                g1 += dg1;
                gain += dgain;
            }
        } else {
            float *out = mix + c;
            for (uint32_t j = 0; j < n; ++j) {
                out[j] += row[j] * gain * g0;
                g0 += dg0;
                gain += dgain;
            }
        }
        m->mod_gain = want;
        voice->age += n;
        mod_advance(voice, n);
        c += n;
    }
    voice->gain[0] = target[0];
    voice->gain[1] = target[1];
}

AUDIODSP_HOT
uint32_t audiodsp_sampler_render_s16(audiodsp_sampler_t *self, int16_t *out) {
    const uint32_t words = self->frames * self->channel_count;
    uint32_t sounding = 0;
    bool any = false;
    const uint32_t slots = AUDIODSP_SAMPLER_SLOTS(self->voice_count);
    for (uint32_t v = 0; v < slots; ++v) {
        audiodsp_sampler_voice_t *voice = &self->voices[v];
        if (voice->stage == AUDIODSP_SAMPLER_STAGE_IDLE) {
            continue;
        }
        if (!any) {
            memset(self->mix, 0, words * sizeof(float));
            any = true;
        }
        render_voice(self, voice);
        if (v < self->voice_count &&
            voice->stage != AUDIODSP_SAMPLER_STAGE_IDLE) {
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
