// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_convolve.h"

#include <math.h>
#include <stddef.h>

#include "shared/audiodsp_trig.h"

#define FRAMES AUDIODSP_CONVOLVE_FRAMES
#define FFTN AUDIODSP_CONVOLVE_FFT
#define BINS AUDIODSP_CONVOLVE_BINS
#define SPECTRUM (2u * BINS)

static float clampf(float value, float low, float high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

// exp() for the small negative arguments this file asks for: an envelope step
// per sample and a one-pole coefficient. Both are within a few thousandths of
// zero, where nine terms are exact to float and then some.
//
// Not libm's, for the reason in audiodsp_trig.h -- these values multiply
// themselves tens of thousands of times down an impulse response, so a
// last-place difference between two libms would not stay in the last place.
static double exp_small(double x) {
    if (x < -1.0) x = -1.0;
    if (x > 0.0) x = 0.0;
    double term = 1.0, sum = 1.0;
    for (int k = 1; k <= 9; k++) {
        term *= x / (double)k;
        sum += term;
    }
    return sum;
}

// xorshift32. The point is not statistical quality -- a reverb tail is
// forgiving -- but that the same seed gives the same room on CPython,
// MicroPython and CircuitPython.
static float noise_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    // Top 24 bits into [-1, 1), which keeps the mantissa exact.
    return (float)((int32_t)(x >> 8) - 8388608) * (1.0f / 8388608.0f);
}

uint32_t audiodsp_convolve_float_count(const audiodsp_convolve_config_t *config) {
    uint32_t parts = config->partitions;
    return audiodsp_rfft_table_floats(FFTN)
        + 2u * FFTN                        // window
        + 2u * FRAMES                      // pending
        + 2u * FRAMES                      // emitted
        + 2u * parts * SPECTRUM            // frequency-delay line
        + config->ir_channels * parts * SPECTRUM
        + SPECTRUM                         // accumulator
        + FFTN                             // scratch
        + FFTN;                            // block
}

void audiodsp_convolve_config_init(audiodsp_convolve_config_t *config,
    uint32_t sample_rate, uint32_t partitions, uint32_t ir_channels) {
    config->sample_rate = sample_rate ? sample_rate : 48000u;
    config->channel_count = 2u;
    if (partitions < 1u) partitions = 1u;
    if (partitions > AUDIODSP_CONVOLVE_MAX_PARTITIONS) {
        partitions = AUDIODSP_CONVOLVE_MAX_PARTITIONS;
    }
    config->partitions = partitions;
    config->ir_channels = ir_channels >= 2u ? 2u : 1u;
    config->mix = 1.0f;   // 0.5 asked for, doubled -- full wet beside full dry
    config->wet_gain = 1.0f;
}

void audiodsp_convolve_set_channel_count(audiodsp_convolve_config_t *config,
    uint32_t channel_count) {
    config->channel_count = channel_count == 1u ? 1u : 2u;
}

void audiodsp_convolve_configure(audiodsp_convolve_config_t *config,
    audiodsp_convolve_option_t option, float value) {
    switch (option) {
        case AUDIODSP_CONVOLVE_OPT_MIX:
            config->mix = clampf(value, 0.0f, 1.0f) * 2.0f;
            break;
        case AUDIODSP_CONVOLVE_OPT_WET_GAIN:
            // NaN and below zero are no wet, past 4 is 4.
            if (!(value >= 0.0f)) value = 0.0f;
            config->wet_gain = value < 4.0f ? value : 4.0f;
            break;
        default:
            break;
    }
}

uint32_t audiodsp_convolve_float_count_apart(
    const audiodsp_convolve_config_t *config) {
    return audiodsp_convolve_float_count(config)
        - audiodsp_convolve_impulse_floats(config);
}

void audiodsp_convolve_state_init(audiodsp_convolve_state_t *state,
    const audiodsp_convolve_config_t *config, float *storage) {
    audiodsp_convolve_state_init_apart(state, config, storage, NULL);
}

void audiodsp_convolve_state_init_apart(audiodsp_convolve_state_t *state,
    const audiodsp_convolve_config_t *config, float *storage,
    float *impulse) {
    uint32_t parts = config->partitions;
    float *cursor = storage;

    audiodsp_rfft_init(&state->rfft, FFTN, cursor);
    cursor += audiodsp_rfft_table_floats(FFTN);

    state->window = cursor;      cursor += 2u * FFTN;
    state->pending = cursor;     cursor += 2u * FRAMES;
    state->emitted = cursor;     cursor += 2u * FRAMES;
    state->fdl = cursor;         cursor += 2u * parts * SPECTRUM;
    if (impulse != NULL) {
        state->impulse = impulse;
    } else {
        state->impulse = cursor; cursor += config->ir_channels * parts * SPECTRUM;
    }
    state->accumulator = cursor; cursor += SPECTRUM;
    state->scratch = cursor;     cursor += FFTN;
    state->block = cursor;

    for (uint32_t i = 0; i < config->ir_channels * parts * SPECTRUM; i++) {
        state->impulse[i] = 0.0f;
    }
    state->loaded = 0;
    audiodsp_convolve_reset(state, config);
}

void audiodsp_convolve_reset(audiodsp_convolve_state_t *state,
    const audiodsp_convolve_config_t *config) {
    for (uint32_t i = 0; i < 2u * FFTN; i++) state->window[i] = 0.0f;
    for (uint32_t i = 0; i < 2u * FRAMES; i++) state->pending[i] = 0.0f;
    for (uint32_t i = 0; i < 2u * FRAMES; i++) state->emitted[i] = 0.0f;
    for (uint32_t i = 0; i < 2u * config->partitions * SPECTRUM; i++) {
        state->fdl[i] = 0.0f;
    }
    state->phase = 0;
    state->cursor = 0;
    state->emitted_mix = config->mix;
    state->emitted_wet_gain = config->wet_gain;
    state->resting = false;
    state->quiet_frames = 0;
}

// Transforms one partition's worth of taps sitting in state->block[0..FRAMES)
// into slot `part` of impulse channel `channel`. The upper half of the
// transform's input is the zero padding overlap-save needs.
static void wet_block(const audiodsp_convolve_config_t *config,
    audiodsp_convolve_state_t *state, uint32_t ch);
static float dry_of(float mix);
static float wet_of(float mix);

static void store_partition(const audiodsp_convolve_state_t *state,
    const audiodsp_convolve_config_t *config, float *impulse, float *block,
    float *scratch, uint32_t channel, uint32_t part) {
    for (uint32_t i = FRAMES; i < FFTN; i++) block[i] = 0.0f;
    audiodsp_rfft_forward(&state->rfft, block,
        impulse + (channel * config->partitions + part) * SPECTRUM, scratch);
}

// Partitions `tap_frames` of impulse fill, which is what `loaded` becomes
// once they are installed. Known before a build, so a swap can decide
// whether there is a room to fade to before anything is overwritten.
static uint32_t partitions_of(const audiodsp_convolve_config_t *config,
    uint32_t tap_frames) {
    uint32_t loaded = (tap_frames + FRAMES - 1u) / FRAMES;
    return loaded > config->partitions ? config->partitions : loaded;
}

uint32_t audiodsp_convolve_impulse_floats(
    const audiodsp_convolve_config_t *config) {
    return config->ir_channels * config->partitions * SPECTRUM;
}

uint32_t audiodsp_convolve_build_s16(const audiodsp_convolve_state_t *state,
    const audiodsp_convolve_config_t *config, float *impulse, float *block,
    float *scratch, const int16_t *taps, uint32_t tap_frames,
    uint32_t channels, float gain) {
    uint32_t parts = config->partitions;
    uint32_t capacity = parts * FRAMES;
    if (tap_frames > capacity) tap_frames = capacity;
    if (channels < 1u) channels = 1u;
    if (channels > 2u) channels = 2u;

    float scale = gain * (1.0f / 32768.0f);
    for (uint32_t ch = 0; ch < config->ir_channels; ch++) {
        // A mono impulse into a stereo configuration is the same impulse
        // twice; a stereo impulse into a mono configuration keeps the left.
        uint32_t source = channels == 1u ? 0u : (ch < channels ? ch : channels - 1u);
        for (uint32_t part = 0; part < parts; part++) {
            for (uint32_t i = 0; i < FRAMES; i++) {
                uint32_t frame = part * FRAMES + i;
                block[i] = frame < tap_frames
                    ? (float)taps[frame * channels + source] * scale : 0.0f;
            }
            store_partition(state, config, impulse, block, scratch, ch, part);
        }
    }
    return partitions_of(config, tap_frames);
}

// The block in flight's wet against the room loaded now, parked in the first
// half of each channel's overlap-save window. The next block overwrites that
// half without reading it, so this costs no memory.
static void park_block_in_flight(const audiodsp_convolve_config_t *config,
    audiodsp_convolve_state_t *state) {
    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    for (uint32_t ch = 0; ch < channels; ch++) {
        wet_block(config, state, ch);
        float *old = state->window + ch * FFTN;
        for (uint32_t i = 0; i < FRAMES; i++) {
            old[i] = state->block[FRAMES + i];
        }
    }
}

// The block in flight, recomputed against the room loaded now and crossfaded
// from the parked one across whatever of it has not been played.
static void crossfade_block_in_flight(const audiodsp_convolve_config_t *config,
    audiodsp_convolve_state_t *state) {
    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    // The block in flight at the mix it was computed at, not the mix now: a
    // Mix move acts on the input after it, and a room move must not bring
    // it forward. `pending` from state->phase on still holds the input that
    // block was made from, and the frames before state->phase have been
    // played. A straight line from old to new over what is left, landing on
    // the new room at its last frame.
    // The wet gain is the room's level, so it fades with the room: the old
    // room at the gain it was computed at, the new at the gain set now. At
    // equal gains of 1.0 every product below is a multiply by exactly one.
    float dry = dry_of(state->emitted_mix);
    float wet = wet_of(state->emitted_mix);
    float gain_old = state->emitted_wet_gain;
    float gain_new = config->wet_gain;
    uint32_t start = state->phase;
    float span = (float)(FRAMES - start);
    for (uint32_t ch = 0; ch < channels; ch++) {
        wet_block(config, state, ch);
        const float *old = state->window + ch * FFTN;
        const float *pending = state->pending + ch * FRAMES;
        float *emitted = state->emitted + ch * FRAMES;
        for (uint32_t i = start; i < FRAMES; i++) {
            float g = (float)(i - start + 1u) / span;
            float from = old[i] * gain_old;
            float y = from + g * (state->block[FRAMES + i] * gain_new - from);
            emitted[i] = dry * pending[i] + wet * y;
        }
    }
    state->emitted_wet_gain = gain_new;
}

// The two halves of a room change, either side of whatever writes the new
// impulse. On a node already carrying a room the audio in flight is kept:
// the history, the input gathered into the block being built and the block
// being played out all stay, and only the impulse changes. `begin_swap`
// parks the block in flight's wet against the room going out, before
// anything overwrites that room; `end_swap` recomputes it against the room
// that came in and crossfades between the two. The frequency-delay line
// holds input alone, so from the next block the node is exactly one that
// always had the new room.
//
// A node with nothing loaded has been a bypass with no latency, and starts
// its latency from empty instead, as it always has. So does a node being
// unloaded (`loaded` 0): it becomes that bypass, which holds no audio, so
// there is nothing to fade to. Both halves must agree, so the first returns
// the answer for the second.
static bool begin_swap(const audiodsp_convolve_config_t *config,
    audiodsp_convolve_state_t *state, uint32_t loaded) {
    const bool keep = state->loaded != 0u && loaded != 0u;
    if (keep) {
        park_block_in_flight(config, state);
    }
    return keep;
}

static void end_swap(const audiodsp_convolve_config_t *config,
    audiodsp_convolve_state_t *state, bool keep) {
    if (keep) {
        crossfade_block_in_flight(config, state);
    } else {
        audiodsp_convolve_reset(state, config);
    }
}

// --- the room's peak gain on low material (room_peak_gain) ----------------
//
// 2^(k/8) for k = 0..16: the squares the reading folds the room against run
// from 40 Hz to 160 Hz in eighth-octave steps. Literals, not pow(), for the
// reason exp_small gives.
static const double peak_octave[AUDIODSP_CONVOLVE_PEAK_SQUARES] = {
    1.0, 1.0905077326652577, 1.189207115002721, 1.2968395546510096,
    1.4142135623730951, 1.5422108254079407, 1.681792830507429,
    1.8340080864093424, 2.0, 2.1810154653305154, 2.378414230005442,
    2.5936791093020193, 2.8284271247461903, 3.0844216508158815,
    3.363585661014858, 3.668016172818685, 4.0,
};

// Half the period of square `k`, in frames, never below one.
static uint32_t peak_half_period(const audiodsp_convolve_config_t *config,
    uint32_t k) {
    double half = (double)config->sample_rate
        / (2.0 * AUDIODSP_CONVOLVE_PEAK_LOW_HZ * peak_octave[k]);
    uint32_t frames = (uint32_t)(half + 0.5);
    return frames ? frames : 1u;
}

uint32_t audiodsp_convolve_peak_floats(
    const audiodsp_convolve_config_t *config) {
    uint32_t total = 0;
    for (uint32_t k = 0; k < AUDIODSP_CONVOLVE_PEAK_SQUARES; k++) {
        total += peak_half_period(config, k);
    }
    return total;
}

// One side's folds and step response, fed a partition of taps at a time.
typedef struct {
    float *fold;            // audiodsp_convolve_peak_floats() floats
    uint32_t frame;         // taps fed so far
    float step;             // the running sum of the taps
    float step_peak;        // its largest magnitude
} peak_reading_t;

static void peak_begin(peak_reading_t *r, const audiodsp_convolve_config_t *config,
    float *fold) {
    r->fold = fold;
    r->frame = 0;
    r->step = 0.0f;
    r->step_peak = 0.0f;
    uint32_t n = audiodsp_convolve_peak_floats(config);
    for (uint32_t i = 0; i < n; i++) fold[i] = 0.0f;
}

// A square of half-period H, +1 then -1, convolved with the room settles to
// a waveform that is H-antiperiodic, and it is the room folded onto H frames
// with alternating signs (`fold`), run through one half-period of the square:
// 2 * (running sum of the fold) - (its total). Its largest magnitude is that
// square's steady-state peak, read in O(taps) with H floats.
static void peak_feed(peak_reading_t *r, const audiodsp_convolve_config_t *config,
    const float *taps, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        float v = taps[i];
        r->step += v;
        float m = r->step < 0.0f ? -r->step : r->step;
        if (m > r->step_peak) r->step_peak = m;
    }
    float *fold = r->fold;
    for (uint32_t k = 0; k < AUDIODSP_CONVOLVE_PEAK_SQUARES; k++) {
        uint32_t half = peak_half_period(config, k);
        uint32_t at = r->frame % half;
        bool negative = ((r->frame / half) & 1u) != 0u;
        for (uint32_t i = 0; i < count; i++) {
            fold[at] += negative ? -taps[i] : taps[i];
            if (++at == half) {
                at = 0;
                negative = !negative;
            }
        }
        fold += half;
    }
    r->frame += count;
}

static float peak_end(const peak_reading_t *r,
    const audiodsp_convolve_config_t *config) {
    float peak = r->step_peak;
    const float *fold = r->fold;
    for (uint32_t k = 0; k < AUDIODSP_CONVOLVE_PEAK_SQUARES; k++) {
        uint32_t half = peak_half_period(config, k);
        float total = 0.0f;
        for (uint32_t i = 0; i < half; i++) total += fold[i];
        float run = 0.0f;
        for (uint32_t i = 0; i < half; i++) {
            run += fold[i];
            float y = 2.0f * run - total;
            if (y < 0.0f) y = -y;
            if (y > peak) peak = y;
        }
        fold += half;
    }
    return peak;
}

uint32_t audiodsp_convolve_build_room(const audiodsp_convolve_state_t *state,
    const audiodsp_convolve_config_t *config, float *impulse, float *block,
    float *scratch, float decay_seconds, float damping_hz, float predelay_ms,
    float diffusion_ms, uint32_t seed) {
    return audiodsp_convolve_build_room_peak(state, config, impulse, block,
        scratch, decay_seconds, damping_hz, predelay_ms, diffusion_ms, seed,
        NULL, NULL);
}

uint32_t audiodsp_convolve_build_room_peak(
    const audiodsp_convolve_state_t *state,
    const audiodsp_convolve_config_t *config, float *impulse, float *block,
    float *scratch, float decay_seconds, float damping_hz, float predelay_ms,
    float diffusion_ms, uint32_t seed, float *peak_fold, float *peak_gain) {
    float peak = 0.0f;
    uint32_t parts = config->partitions;
    uint32_t capacity = parts * FRAMES;
    double rate = (double)config->sample_rate;

    decay_seconds = clampf(decay_seconds, 0.05f, 30.0f);
    predelay_ms = clampf(predelay_ms, 0.0f, 200.0f);
    diffusion_ms = clampf(diffusion_ms, 0.0f, 500.0f);

    // -60 dB over decay_seconds, as a per-sample multiplier.
    double envelope_step = exp_small(-3.0 * 2.302585092994046
        / ((double)decay_seconds * rate));
    // A one-pole rolling the tail's top off: a real tail loses its top faster
    // than its level, and a flat-spectrum decay is the giveaway that an
    // impulse was generated rather than measured. Above a quarter of the
    // sample rate a one-pole is not a filter any more, so anything up there
    // means "no damping" rather than a coefficient nobody can use.
    double damping = 1.0;
    if (damping_hz > 0.0f && (double)damping_hz < rate * 0.25) {
        damping = 1.0 - exp_small(-2.0 * AUDIODSP_PI * (double)damping_hz / rate);
    }

    uint32_t predelay = (uint32_t)((double)predelay_ms * 0.001 * rate);
    uint32_t diffusion = (uint32_t)((double)diffusion_ms * 0.001 * rate);
    if (predelay > capacity) predelay = capacity;
    double rise = diffusion ? 1.0 / (double)diffusion : 0.0;

    // Two passes over the same deterministic sequence: the first measures the
    // impulse's energy, the second writes it scaled so that energy is one --
    // each side's own energy, one scale per side. The two sides are one
    // envelope over two noises, so their energies differ by the luck of the
    // draw and by what the damping makes of each, and one scale from their
    // mean left a stereo room leaning by up to 5.3 dB (audiodsp#164). With a
    // scale each, both sides are unit energy, the two together are the same
    // level as before, and the room keeps its two noises: the stereo image
    // is the decorrelation between them, which a scale does not touch. A mono
    // room is the first side alone, the same arithmetic as ever.
    //
    // Normalizing matters more here than it looks. A tail of unit-amplitude
    // noise convolved with anything is enormous -- 48000 taps near full scale
    // sum to tens of thousands of times the input -- so an unnormalized
    // synthetic room is not a quiet room, it is a clipped one. Unit energy
    // means noise-like material comes back at the level it went in, which is
    // the convention a measured impulse is normalized to as well.
    //
    // Two passes rather than one and a rescale because the partitions are
    // transformed as they are generated and there is nowhere to keep the taps.
    // The second pass costs only the noise, not the transforms.
    double scale[2] = {1.0, 1.0};
    for (int pass = 0; pass < 2; pass++) {
        double energy[2] = {0.0, 0.0};
        for (uint32_t ch = 0; ch < config->ir_channels; ch++) {
            peak_reading_t reading = {NULL, 0u, 0.0f, 0.0f};
            const bool read_peak = pass == 1 && peak_fold != NULL;
            if (read_peak) {
                peak_begin(&reading, config, peak_fold);
            }
            // Same envelope, different noise: two channels of one room, not
            // two rooms. A shared seed would make the impulse mono and
            // collapse the stereo image the whole exercise is for.
            uint32_t rng = seed + ch * 0x9e3779b9u;
            if (rng == 0) rng = 1;
            double envelope = 1.0;
            double lowpass = 0.0;
            for (uint32_t part = 0; part < parts; part++) {
                for (uint32_t i = 0; i < FRAMES; i++) {
                    uint32_t frame = part * FRAMES + i;
                    if (frame < predelay) { block[i] = 0.0f; continue; }
                    double sample = (double)noise_next(&rng) * envelope;
                    lowpass += damping * (sample - lowpass);
                    double shaped = lowpass;
                    uint32_t since = frame - predelay;
                    if (since < diffusion) {
                        // Linear fade-in. Anything smoother is inaudible
                        // against noise, and the reciprocal is hoisted.
                        shaped *= (double)since * rise;
                    }
                    envelope *= envelope_step;
                    if (pass == 0) {
                        energy[ch] += shaped * shaped;
                    } else {
                        block[i] = (float)(shaped * scale[ch]);
                    }
                }
                if (pass) {
                    if (read_peak) {
                        peak_feed(&reading, config, block, FRAMES);
                    }
                    if (impulse != NULL) {
                        store_partition(state, config, impulse, block,
                            scratch, ch, part);
                    }
                }
            }
            if (read_peak) {
                float side = peak_end(&reading, config);
                if (side > peak) peak = side;
            }
        }
        if (pass == 0) {
            for (uint32_t ch = 0; ch < config->ir_channels; ch++) {
                scale[ch] = energy[ch] > 0.0 ? 1.0 / sqrt(energy[ch]) : 0.0;
            }
        }
    }
    if (peak_gain != NULL) {
        *peak_gain = peak;
    }
    return parts;
}

float *audiodsp_convolve_install(audiodsp_convolve_state_t *state,
    const audiodsp_convolve_config_t *config, float *impulse,
    uint32_t loaded) {
    const bool keep = begin_swap(config, state, loaded);
    float *previous = NULL;
    if (impulse != NULL && impulse != state->impulse) {
        previous = state->impulse;
        state->impulse = impulse;
    }
    state->loaded = loaded;
    end_swap(config, state, keep);
    return previous;
}

// In place: the impulse is built straight into the node's own, with the
// node's own scratch, so nothing may pull the node until it returns. A
// binding whose pump runs on another thread builds into a buffer of its own
// with audiodsp_convolve_build_s16 and installs it under the lock instead.
// Either way the audio in flight is kept and the block being played
// crossfades, as begin_swap and end_swap say; the room that fills the whole
// capacity below takes the same path.
void audiodsp_convolve_load_s16(audiodsp_convolve_state_t *state,
    const audiodsp_convolve_config_t *config, const int16_t *taps,
    uint32_t tap_frames, uint32_t channels, float gain) {
    const bool keep = begin_swap(config, state,
        partitions_of(config, tap_frames));
    state->loaded = audiodsp_convolve_build_s16(state, config,
        state->impulse, state->block, state->scratch, taps, tap_frames,
        channels, gain);
    end_swap(config, state, keep);
}

float audiodsp_convolve_room_peak(const audiodsp_convolve_config_t *config,
    float *block, float decay_seconds, float damping_hz, float predelay_ms,
    float diffusion_ms, uint32_t seed, float *peak_fold) {
    // The taps only: no impulse is written and no transform taken, so it
    // reads nothing of a node's state and needs no lock.
    float peak = 0.0f;
    audiodsp_convolve_build_room_peak(NULL, config, NULL, block, NULL,
        decay_seconds, damping_hz, predelay_ms, diffusion_ms, seed,
        peak_fold, &peak);
    return peak;
}

void audiodsp_convolve_synthesize(audiodsp_convolve_state_t *state,
    const audiodsp_convolve_config_t *config, float decay_seconds,
    float damping_hz, float predelay_ms, float diffusion_ms, uint32_t seed) {
    audiodsp_convolve_synthesize_peak(state, config, decay_seconds,
        damping_hz, predelay_ms, diffusion_ms, seed, NULL, NULL);
}

void audiodsp_convolve_synthesize_peak(audiodsp_convolve_state_t *state,
    const audiodsp_convolve_config_t *config, float decay_seconds,
    float damping_hz, float predelay_ms, float diffusion_ms, uint32_t seed,
    float *peak_fold, float *peak_gain) {
    // A room fills every partition, so there is always one to fade to.
    const bool keep = begin_swap(config, state, config->partitions);
    state->loaded = audiodsp_convolve_build_room_peak(state, config,
        state->impulse, state->block, state->scratch, decay_seconds,
        damping_hz, predelay_ms, diffusion_ms, seed, peak_fold, peak_gain);
    end_swap(config, state, keep);
}

// The frequency-delay line of audio channel `ch`, as it stands at
// state->cursor, against the impulse loaded now: the wet of one block,
// left in state->block[FRAMES..FFTN). process_block computes each block's
// wet with it, and a re-synthesis computes the block in flight's twice
// with it, once against the old room and once against the new.
static void wet_block(const audiodsp_convolve_config_t *config,
    audiodsp_convolve_state_t *state, uint32_t ch) {
    uint32_t parts = config->partitions;
    for (uint32_t i = 0; i < SPECTRUM; i++) state->accumulator[i] = 0.0f;
    uint32_t ir_channel = config->ir_channels == 2u ? ch : 0u;
    for (uint32_t part = 0; part < state->loaded; part++) {
        uint32_t age = state->cursor >= part
            ? state->cursor - part : state->cursor + parts - part;
        const float *x = state->fdl + (ch * parts + age) * SPECTRUM;
        const float *h = state->impulse
            + (ir_channel * parts + part) * SPECTRUM;
        for (uint32_t bin = 0; bin < BINS; bin++) {
            float xr = x[2 * bin], xi = x[2 * bin + 1];
            float hr = h[2 * bin], hi = h[2 * bin + 1];
            state->accumulator[2 * bin] += xr * hr - xi * hi;
            state->accumulator[2 * bin + 1] += xr * hi + xi * hr;
        }
    }
    audiodsp_rfft_inverse(&state->rfft, state->accumulator, state->block,
        state->scratch);
}

static float dry_of(float mix) {
    return 2.0f - mix < 1.0f ? 2.0f - mix : 1.0f;
}

static float wet_of(float mix) {
    return mix < 1.0f ? mix : 1.0f;
}

// One partition's worth of gathered input, per channel, through the
// convolution and out into state->emitted.
static void process_block(const audiodsp_convolve_config_t *config,
    audiodsp_convolve_state_t *state) {
    uint32_t parts = config->partitions;
    float mix = config->mix;
    float dry = dry_of(mix);
    float wet = wet_of(mix) * config->wet_gain;
    state->emitted_mix = mix;
    state->emitted_wet_gain = config->wet_gain;

    state->cursor = state->cursor + 1u < parts ? state->cursor + 1u : 0u;

    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    for (uint32_t ch = 0; ch < channels; ch++) {
        float *window = state->window + ch * FFTN;
        float *pending = state->pending + ch * FRAMES;

        // Overlap-save: the transform sees the previous block and this one,
        // and only the second half of the result is free of the wrap-around.
        for (uint32_t i = 0; i < FRAMES; i++) window[i] = window[i + FRAMES];
        for (uint32_t i = 0; i < FRAMES; i++) window[FRAMES + i] = pending[i];

        float *slot = state->fdl + (ch * parts + state->cursor) * SPECTRUM;
        audiodsp_rfft_forward(&state->rfft, window, slot, state->scratch);

        wet_block(config, state, ch);

        float *emitted = state->emitted + ch * FRAMES;
        for (uint32_t i = 0; i < FRAMES; i++) {
            emitted[i] = dry * pending[i] + wet * state->block[FRAMES + i];
        }
    }
}

static void convolve_run(const audiodsp_convolve_config_t *config,
    audiodsp_convolve_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames);

void audiodsp_convolve_process_s16(const audiodsp_convolve_config_t *config,
    audiodsp_convolve_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames) {
    if (frames > 0u) {
        state->resting = false;
        state->quiet_frames = 0;
    }
    convolve_run(config, state, out, in, frames);
}

static bool convolve_all_zero(const float *values, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (values[i] != 0.0f) {
            return false;
        }
    }
    return true;
}

// Exactly zero everywhere audio is held: the overlap-save history, the block
// being gathered, the block being played and every slot of the
// frequency-delay line. Nothing loaded is a bypass, which holds no audio.
static bool convolve_settled(const audiodsp_convolve_config_t *config,
    const audiodsp_convolve_state_t *state) {
    if (state->loaded == 0) {
        return true;
    }
    return convolve_all_zero(state->window, 2u * FFTN) &&
           convolve_all_zero(state->pending, 2u * FRAMES) &&
           convolve_all_zero(state->emitted, 2u * FRAMES) &&
           convolve_all_zero(state->fdl, 2u * config->partitions * SPECTRUM);
}

void audiodsp_convolve_process_silence(
    const audiodsp_convolve_config_t *config,
    audiodsp_convolve_state_t *state, int16_t *out, uint32_t frames) {
    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    const uint32_t samples = frames * channels;
    for (uint32_t i = 0; i < samples; i++) out[i] = 0;
    if (state->resting || frames == 0u) {
        return;
    }
    // `out` may alias `in`, so the zeros just written are the input.
    convolve_run(config, state, out, out, frames);
    for (uint32_t i = 0; i < samples; i++) {
        if (out[i] != 0) {
            state->quiet_frames = 0;
            return;
        }
    }
    state->quiet_frames += frames;
    uint32_t length = (state->loaded + 1u) * FRAMES;
    if (state->quiet_frames < length) {
        return;
    }
    state->quiet_frames = 0;
    if (convolve_settled(config, state)) {
        state->resting = true;
    }
}

static void convolve_run(const audiodsp_convolve_config_t *config,
    audiodsp_convolve_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames) {
    if (state->loaded == 0) {
        // Nothing loaded is a bypass, not a room of zero size -- and it has
        // to bypass without the block latency, or a chain built before its
        // impulse arrives would drift against its neighbours.
        const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
        for (uint32_t i = 0; i < frames * channels; i++) out[i] = in[i];
        return;
    }
    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    for (uint32_t frame = 0; frame < frames; frame++) {
        uint32_t phase = state->phase;
        // Read before write: `emitted` holds the previous block's result, so
        // the output trails the input by exactly one partition however the
        // caller chops up its calls.
        for (uint32_t ch = 0; ch < channels; ch++) {
            float value = state->emitted[ch * FRAMES + phase];
            int32_t rounded = (int32_t)(value < 0.0f ? value - 0.5f : value + 0.5f);
            if (rounded > 32767) rounded = 32767;
            else if (rounded < -32768) rounded = -32768;
            state->pending[ch * FRAMES + phase] =
                (float)in[frame * channels + ch];
            out[frame * channels + ch] = (int16_t)rounded;
        }
        state->phase = phase + 1u;
        if (state->phase >= FRAMES) {
            process_block(config, state);
            state->phase = 0;
        }
    }
}
