// Runtime-neutral feedback delay. See audioif_feedback_delay.h for what is in
// the loop and why none of it could be done outside one.
//
// `float` working precision, deliberately, to match audioif_dynamics.c: this
// runs per sample on parts without an FPU at all, and doubles would cost more
// than the last few bits are worth in a feedback path that is already storing
// its line as int16.
//
// SPDX-License-Identifier: MIT

#include "shared/audioif_fp_contract.h"

#include "shared/audioif_feedback_delay.h"

#include <math.h>
#include <string.h>

#define AUDIOIF_FEEDBACK_DELAY_PI 3.14159265358979323846f

static float clampf(float value, float low, float high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

// 1 - exp(-2*pi*fc/fs): the one-pole coefficient both loop filters use. Zero
// means "no filter", which is why a cutoff of zero has to fall out here
// rather than being special-cased in the loop.
static float one_pole_coefficient(float hz, uint32_t sample_rate) {
    if (hz <= 0.0f || sample_rate == 0) {
        return 0.0f;
    }
    float coefficient =
        1.0f - expf(-2.0f * AUDIOIF_FEEDBACK_DELAY_PI * hz / (float)sample_rate);
    return clampf(coefficient, 0.0f, 1.0f);
}

// Turns per frame in Q32, for a phase accumulator stepped once a frame.
// Folded into one turn first so a rate at or above the sample rate wraps
// instead of overflowing the conversion.
static uint32_t phase_step_for(float hz, float rate) {
    if (hz <= 0.0f || rate <= 0.0f) {
        return 0;
    }
    float turns = hz / rate;
    turns -= floorf(turns);
    const float scaled = turns * 4294967296.0f;
    if (scaled <= 0.0f || scaled >= 4294967296.0f) {
        return 0;
    }
    return (uint32_t)scaled;
}

// The pitch shifter reads the line through two taps half a window apart and
// crossfades between them, so the window is what sets both the grain rate and
// how far the read head may wander from the delay it was asked for. Capped at
// a quarter of the line for that second reason, and floored at 16 frames so a
// tiny line still has something to crossfade.
static void shift_window_finish(audioif_feedback_delay_config_t *config) {
    float window = config->loop_window_ms * (float)config->sample_rate /
        1000.0f;
    const float quarter = (float)config->line_frames * 0.25f;
    if (window > quarter) {
        window = quarter;
    }
    if (window < 16.0f) {
        window = 16.0f;
    }
    config->shift_window_frames = window;
}

// The tap offset has to *shrink* at (ratio - 1) frames per frame for the read
// head to advance at `ratio`, so the phase runs backwards when the pitch goes
// up. One window of tap travel is one turn of the crossfade.
static int32_t shift_step_for(const audioif_feedback_delay_config_t *config) {
    if (config->loop_semitones == 0.0f ||
        config->shift_window_frames < 1.0f) {
        return 0;
    }
    const float ratio = powf(2.0f, config->loop_semitones * (1.0f / 12.0f));
    const float step = (1.0f - ratio) / config->shift_window_frames *
        4294967296.0f;
    if (step >= 2147483647.0f) {
        return 2147483647;
    }
    if (step <= -2147483648.0f) {
        return -2147483647 - 1;
    }
    return (int32_t)step;
}

void audioif_feedback_delay_config_init(
    audioif_feedback_delay_config_t *config, uint32_t sample_rate,
    uint32_t line_frames) {
    config->sample_rate = sample_rate ? sample_rate : 48000;
    config->channel_count = 2;
    config->line_frames = line_frames < 2 ? 2 : line_frames;
    config->delay_frames = (float)config->line_frames * 0.5f;
    config->feedback = 0.4f;
    config->mix = 0.3f;
    config->damping_coef = 0.0f;
    config->cut_coef = 0.0f;
    config->wow_step = 0.0f;
    config->wow_depth_frames = 0.0f;
    config->cross_feed = 0.0f;
    config->loop_drive = 0.0f;
    config->damping_hz = 0.0f;
    config->cut_hz = 0.0f;
    config->wow_hz = 0.0f;
    // Everything below is off, and off has to mean *exactly* what this node
    // did before it gained these: no table, no slew, no wet AM and no shift.
    config->wow_shape = NULL;
    config->wow_shape_shift = 0;
    config->wow_phase_step = 0;
    config->delay_slew_frames = 0.0f;
    config->wow_am_depth = 0.0f;
    config->loop_semitones = 0.0f;
    config->loop_window_ms = 25.0f;
    config->shift_step = 0;
    shift_window_finish(config);
    audioif_feedback_delay_configure(config,
        AUDIOIF_FEEDBACK_DELAY_OPT_INPUT_PAN, 0.0f);
}

void audioif_feedback_delay_set_channel_count(
    audioif_feedback_delay_config_t *config, uint32_t channel_count) {
    config->channel_count = channel_count == 1u ? 1u : 2u;
    if (config->channel_count == 1u) {
        config->feed_own[0] = 1.0f;
        config->feed_other[0] = 0.0f;
        config->feed_own[1] = 0.0f;
        config->feed_other[1] = 0.0f;
    }
}

void audioif_feedback_delay_configure(audioif_feedback_delay_config_t *config,
    audioif_feedback_delay_option_t option, float value) {
    const float rate = (float)config->sample_rate;
    switch (option) {
        case AUDIOIF_FEEDBACK_DELAY_OPT_DELAY_MS: {
            // One frame of headroom below the line's length: the read walks
            // backward from the write cursor and interpolates between two
            // neighbours, so it must not be able to land on the cursor itself.
            float frames = value * rate / 1000.0f;
            config->delay_frames =
                clampf(frames, 1.0f, (float)config->line_frames - 2.0f);
            break;
        }
        case AUDIOIF_FEEDBACK_DELAY_OPT_FEEDBACK:
            // Stops just short of 1: at exactly unity a loop with no filter
            // in it never decays, and with the soft-clip engaged it settles
            // into a self-oscillation that is not what anyone asked for.
            config->feedback = clampf(value, 0.0f, 0.99f);
            break;
        case AUDIOIF_FEEDBACK_DELAY_OPT_MIX:
            // 0..2, and the dry stays at unity until 1 -- `audiodelays.Echo`'s
            // convention, adopted deliberately rather than crossfaded. Six
            // delay classes sit on top of these two modules and `mix` has to
            // mean one thing across all of them; three of them are Echo's and
            // that convention is CircuitPython's to set. So 0.3 is "dry, plus
            // 30 percent wet", 1 is "dry plus all of it", and 2 is wet alone.
            config->mix = clampf(value, 0.0f, 2.0f);
            break;
        case AUDIOIF_FEEDBACK_DELAY_OPT_DAMPING_HZ:
            config->damping_hz = value;
            config->damping_coef = one_pole_coefficient(value,
                config->sample_rate);
            break;
        case AUDIOIF_FEEDBACK_DELAY_OPT_CUT_HZ:
            config->cut_hz = value;
            config->cut_coef = one_pole_coefficient(value,
                config->sample_rate);
            break;
        case AUDIOIF_FEEDBACK_DELAY_OPT_WOW_HZ:
            config->wow_hz = value;
            config->wow_step = value <= 0.0f ? 0.0f :
                2.0f * sinf(AUDIOIF_FEEDBACK_DELAY_PI * value / rate);
            // The shape table's accumulator runs at the same rate as the
            // magic circle, so the two stay in step and `wow_am_depth` reads
            // the same cycle the delay is moving on.
            config->wow_phase_step = phase_step_for(value, rate);
            break;
        case AUDIOIF_FEEDBACK_DELAY_OPT_WOW_DEPTH_MS:
            config->wow_depth_frames =
                clampf(value, 0.0f, 1000.0f) * rate / 1000.0f;
            break;
        case AUDIOIF_FEEDBACK_DELAY_OPT_CROSS_FEED:
            config->cross_feed = clampf(value, 0.0f, 1.0f);
            break;
        case AUDIOIF_FEEDBACK_DELAY_OPT_LOOP_DRIVE:
            config->loop_drive = clampf(value, 0.0f, 1.0f);
            break;
        case AUDIOIF_FEEDBACK_DELAY_OPT_DELAY_SLEW:
            // Delay-seconds per second, so it is the same number at any
            // sample rate: 1 is the read head standing still while the write
            // head runs, which is an octave for as long as the move lasts.
            // Clamped at 64 -- four octaves either way is past anything a
            // tape machine does, and it keeps a single frame from crossing
            // the whole line.
            config->delay_slew_frames = clampf(value, 0.0f, 64.0f);
            break;
        case AUDIOIF_FEEDBACK_DELAY_OPT_WOW_AM_DEPTH:
            config->wow_am_depth = clampf(value, 0.0f, 1.0f);
            break;
        case AUDIOIF_FEEDBACK_DELAY_OPT_LOOP_SEMITONES:
            // Two octaves either way. Past that the crossfade is doing more
            // than the signal is.
            config->loop_semitones = clampf(value, -24.0f, 24.0f);
            config->shift_step = shift_step_for(config);
            break;
        case AUDIOIF_FEEDBACK_DELAY_OPT_LOOP_WINDOW_MS:
            config->loop_window_ms = clampf(value, 1.0f, 250.0f);
            shift_window_finish(config);
            config->shift_step = shift_step_for(config);
            break;
        case AUDIOIF_FEEDBACK_DELAY_OPT_INPUT_PAN: {
            // -1 sends both channels into the left line and nothing into the
            // right, +1 the reverse, 0 leaves each channel on its own line.
            // Hard over is what makes a real ping-pong: the first repeat is
            // on one side only, and the cross-feed alternates it after that.
            //
            // Hard over sends the *average* of the two channels rather than
            // their sum: a mono source panned into one line would otherwise
            // arrive 6 dB hot and clip on the first repeat.
            const float pan = clampf(value, -1.0f, 1.0f);
            const float to_left = pan < 0.0f ? -pan : 0.0f;
            const float to_right = pan > 0.0f ? pan : 0.0f;
            config->feed_own[0] = 1.0f - to_left * 0.5f - to_right;
            config->feed_other[0] = to_left * 0.5f;
            config->feed_own[1] = 1.0f - to_right * 0.5f - to_left;
            config->feed_other[1] = to_right * 0.5f;
            break;
        }
    }
}

void audioif_feedback_delay_config_finish(
    audioif_feedback_delay_config_t *config) {
    audioif_feedback_delay_configure(config,
        AUDIOIF_FEEDBACK_DELAY_OPT_DAMPING_HZ, config->damping_hz);
    audioif_feedback_delay_configure(config,
        AUDIOIF_FEEDBACK_DELAY_OPT_CUT_HZ, config->cut_hz);
    audioif_feedback_delay_configure(config,
        AUDIOIF_FEEDBACK_DELAY_OPT_WOW_HZ, config->wow_hz);
    shift_window_finish(config);
    audioif_feedback_delay_configure(config,
        AUDIOIF_FEEDBACK_DELAY_OPT_LOOP_SEMITONES, config->loop_semitones);
}

bool audioif_feedback_delay_set_wow_shape(
    audioif_feedback_delay_config_t *config, const int16_t *table,
    uint32_t length) {
    if (table == NULL) {
        config->wow_shape = NULL;
        config->wow_shape_shift = 0;
        return true;
    }
    if (length < AUDIOIF_FEEDBACK_DELAY_WOW_SHAPE_MIN ||
        length > AUDIOIF_FEEDBACK_DELAY_WOW_SHAPE_MAX ||
        (length & (length - 1u)) != 0u) {
        return false;
    }
    uint32_t shift = 0;
    while ((1u << shift) < length) {
        ++shift;
    }
    config->wow_shape = table;
    config->wow_shape_shift = shift;
    return true;
}

void audioif_feedback_delay_state_init(audioif_feedback_delay_state_t *state,
    int16_t *line) {
    state->line = line;
    state->write_frame = 0;
    state->damping_state[0] = state->damping_state[1] = 0.0f;
    state->cut_state[0] = state->cut_state[1] = 0.0f;
    state->wow_sine = 0.0f;
    state->wow_cosine = 1.0f;
    state->wow_phase = 0;
    state->shift_phase = 0;
    // Not primed. The first block snaps the read head onto whatever delay is
    // configured; only a change *after* that glides.
    state->delay_current = -1.0f;
}

void audioif_feedback_delay_reset(audioif_feedback_delay_state_t *state,
    const audioif_feedback_delay_config_t *config) {
    if (state->line != NULL) {
        memset(state->line, 0,
            (size_t)config->line_frames * 2u * sizeof(int16_t));
    }
    int16_t *line = state->line;
    audioif_feedback_delay_state_init(state, line);
}

// The one place the whole thing is not linear. A cubic is enough: it is
// odd-symmetric, so it makes third-harmonic thickening and no second, and it
// costs two multiplies where a tanh would cost a library call per sample.
static float soft_clip(float value, float drive) {
    const float normalized = clampf(value * (1.0f / 32768.0f), -1.0f, 1.0f);
    return 32768.0f *
        (normalized - drive * normalized * normalized * normalized / 3.0f);
}

static int16_t to_s16(float value) {
    if (value > 32767.0f) {
        return 32767;
    }
    if (value < -32768.0f) {
        return -32768;
    }
    return (int16_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

// One read position on the line: which two neighbours to interpolate between,
// and how far. Pulled out of the loop body so the pitch shifter's two taps and
// the plain read are the same arithmetic rather than two copies of it.
typedef struct {
    uint32_t near_frame;
    uint32_t far_frame;
    float fraction;
} feedback_delay_tap_t;

static feedback_delay_tap_t tap_for(
    const audioif_feedback_delay_state_t *state, uint32_t length,
    float offset) {
    offset = clampf(offset, 1.0f, (float)length - 2.0f);
    const uint32_t whole = (uint32_t)offset;
    feedback_delay_tap_t tap;
    tap.fraction = offset - (float)whole;
    tap.near_frame = (state->write_frame + length - whole) % length;
    tap.far_frame = (tap.near_frame + length - 1u) % length;
    return tap;
}

static float read_tap(const int16_t *lane, const feedback_delay_tap_t *tap) {
    const float near_sample = (float)lane[tap->near_frame];
    return near_sample +
        tap->fraction * ((float)lane[tap->far_frame] - near_sample);
}

void audioif_feedback_delay_process_s16(
    const audioif_feedback_delay_config_t *config,
    audioif_feedback_delay_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames) {
    const uint32_t length = config->line_frames;
    const float dry = 2.0f - config->mix < 1.0f ? 2.0f - config->mix : 1.0f;
    const float wet = config->mix < 1.0f ? config->mix : 1.0f;
    const float direct = 1.0f - config->cross_feed;
    const float crossed = config->cross_feed;
    const float slew = config->delay_slew_frames;
    const float am_depth = config->wow_am_depth;
    const float window = config->shift_window_frames;
    const bool shifting = config->loop_semitones != 0.0f;
    // With no slew the read head is wherever it was told to be, so `offset`
    // below is the same float it always was and every existing golden holds.
    // A state that has never run is primed the same way, so turning the slew
    // on at construction does not make the first block glide from nowhere.
    if (slew <= 0.0f || state->delay_current < 0.0f) {
        state->delay_current = config->delay_frames;
    }
    feedback_delay_tap_t near_tap = { 0u, 0u, 0.0f };
    feedback_delay_tap_t far_tap = { 0u, 0u, 0.0f };
    float near_gain = 1.0f;
    float far_gain = 0.0f;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        // Rotate the wow oscillator one step. Updating the sine first and
        // feeding the new value back into the cosine is what keeps this
        // stable indefinitely; the naive pair drifts in amplitude. It is
        // stepped whether or not a shape table is in use: `wow_am_depth`
        // reads it, and skipping it would move the default path.
        state->wow_sine += config->wow_step * state->wow_cosine;
        state->wow_cosine -= config->wow_step * state->wow_sine;

        // A bucket brigade's delay is the line over its clock, so the Small
        // Clone's triangle on the clock arrives as a reciprocal on the delay
        // and no sine can be it. With a table the shape is Python's to bake
        // and this only looks it up.
        float wow = state->wow_sine;
        if (config->wow_shape != NULL) {
            state->wow_phase += config->wow_phase_step;
            const uint32_t bits = config->wow_shape_shift;
            const uint32_t index = state->wow_phase >> (32u - bits);
            const uint32_t mask = (1u << bits) - 1u;
            const float fraction =
                (float)(state->wow_phase << bits) * (1.0f / 4294967296.0f);
            const float low = (float)config->wow_shape[index];
            const float high = (float)config->wow_shape[(index + 1u) & mask];
            // Q15, read the way the line is: a table is a waveform, not an
            // index.
            wow = (low + fraction * (high - low)) * (1.0f / 32768.0f);
        }

        // Walk the read head toward the delay it was asked for instead of
        // jumping to it. A constant rate is a constant pitch offset for as
        // long as the move lasts, which is what a tape machine's capstan and
        // a bucket brigade's clock both do, and it is why a delay time can
        // change without a click.
        if (slew > 0.0f) {
            const float target = config->delay_frames;
            float current = state->delay_current;
            if (current < target) {
                current += slew;
                if (current > target) {
                    current = target;
                }
            } else if (current > target) {
                current -= slew;
                if (current < target) {
                    current = target;
                }
            }
            state->delay_current = current;
        }

        const float offset = state->delay_current +
            config->wow_depth_frames * wow;
        if (shifting) {
            // Two taps half a window apart, each walking one window per
            // crossfade turn, mixed with a triangular fade whose two halves
            // sum to exactly one. That is a resampled read: the head advances
            // at the pitch ratio while the line is written at unity, and the
            // shift is inside the loop, so every pass rises again.
            state->shift_phase += (uint32_t)config->shift_step;
            const float turn =
                (float)state->shift_phase * (1.0f / 4294967296.0f);
            const float opposite =
                (float)(state->shift_phase + 0x80000000u) *
                (1.0f / 4294967296.0f);
            near_gain = 2.0f * turn;
            if (near_gain > 1.0f) {
                near_gain = 2.0f - near_gain;
            }
            far_gain = 1.0f - near_gain;
            near_tap = tap_for(state, length, offset + turn * window);
            far_tap = tap_for(state, length, offset + opposite * window);
        } else {
            near_tap = tap_for(state, length, offset);
        }

        float loop[2];
        const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const int16_t *lane = state->line + (size_t)channel * length;
            float delayed = read_tap(lane, &near_tap);
            if (shifting) {
                delayed = delayed * near_gain +
                    read_tap(lane, &far_tap) * far_gain;
            }

            float value = delayed;
            if (config->damping_coef > 0.0f) {
                state->damping_state[channel] += config->damping_coef *
                    (value - state->damping_state[channel]);
                value = state->damping_state[channel];
            }
            if (config->cut_coef > 0.0f) {
                state->cut_state[channel] += config->cut_coef *
                    (value - state->cut_state[channel]);
                value -= state->cut_state[channel];
            }
            if (config->loop_drive > 0.0f) {
                value = soft_clip(value, config->loop_drive);
            }
            loop[channel] = value;
        }

        // The wow oscillator on the wet gain as well as on the delay. Tape
        // and bucket-brigade level wobble is a loss, never a boost, so this
        // dips to (1 - depth) and returns to unity -- and it is on the output
        // only, never in the loop, so it cannot change what the feedback
        // path does.
        float wet_gain = wet;
        if (am_depth > 0.0f) {
            wet_gain = wet *
                (1.0f - am_depth * 0.5f * (1.0f - state->wow_sine));
        }

        for (uint32_t channel = 0; channel < channels; ++channel) {
            const float other = channels == 2u ? loop[1u - channel] : 0.0f;
            const float sent = loop[channel] * direct + other * crossed;
            const float source = (float)in[frame * channels + channel];
            const float other_source = channels == 2u
                ? (float)in[frame * channels + (1u - channel)] : 0.0f;
            const float fed = source * config->feed_own[channel] +
                other_source * config->feed_other[channel];
            state->line[(size_t)channel * length + state->write_frame] =
                to_s16(fed + config->feedback * sent);
            // The dry path is the channel's own signal, never the panned
            // one: `input_pan` steers what goes round the loop, not what the
            // listener hears straight through.
            out[frame * channels + channel] =
                to_s16(dry * source + wet_gain * loop[channel]);
        }
        state->write_frame = (state->write_frame + 1u) % length;
    }
}
