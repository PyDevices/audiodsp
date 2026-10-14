// Runtime-neutral feedback delay. See audiodsp_feedback_delay.h for what is in
// the loop and why none of it could be done outside one.
//
// `float` working precision, deliberately, to match audiodsp_dynamics.c: this
// runs per sample on parts without an FPU at all, and doubles would cost more
// than the last few bits are worth in a feedback path that is already storing
// its line as int16.
//
// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_feedback_delay.h"
#include "shared/audiodsp_trig.h"

#include <math.h>
#include <string.h>

#define AUDIODSP_FEEDBACK_DELAY_PI 3.14159265358979323846f

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
        1.0f - expf(-2.0f * AUDIODSP_FEEDBACK_DELAY_PI * hz / (float)sample_rate);
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
static void shift_window_finish(audiodsp_feedback_delay_config_t *config) {
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
static int32_t shift_step_for(const audiodsp_feedback_delay_config_t *config) {
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

// The band limit on an upward shift. Read at `ratio`, a frame of line at f
// comes out at f * ratio, so anything written above fs / (2 * ratio) folds
// back past Nyquist on the next pass, as a tone outside the series, and then
// climbs from there. What is written is low-passed first, at this fraction of
// that edge: a fourth-order Butterworth there is down 13 dB at the edge and
// further every pass above it, while a series tone more than a third of the
// way below the edge loses under 1 dB a pass. The fade at the top is the
// climb running out of band, which is what a listener expects to hear.
#define AUDIODSP_FEEDBACK_DELAY_BAND_EDGE 0.7

static void band_limit_finish(audiodsp_feedback_delay_config_t *config,
    float ratio) {
    config->band_limit = false;
    if (!(config->loop_semitones > 0.0f) || !(ratio > 1.0f) ||
        config->sample_rate == 0) {
        return;
    }
    // Worked in double through audiodsp_trig, so the coefficients are the
    // same bits on every desktop interpreter (the ratio comes through powf,
    // which a board's libm may round differently, audiodsp#183). The corner
    // is always under a quarter of the rate (ratio > 1), so w0 stays inside
    // the polynomial's quarter.
    const double w0 = 2.0 * AUDIODSP_PI * AUDIODSP_FEEDBACK_DELAY_BAND_EDGE *
        0.5 / (double)ratio;
    audiodsp_sincos_t sc;
    audiodsp_sincos_quarter(w0, &sc);
    // 1 / (2 cos(pi / 8)) and 1 / (2 cos(3 pi / 8)).
    static const double q[2] = { 0.54119610014619698, 1.3065629648763766 };
    for (int section = 0; section < 2; ++section) {
        const double alpha = sc.s / (2.0 * q[section]);
        const double a0 = 1.0 + alpha;
        const double b1 = (1.0 - sc.c) / a0;
        config->band_coef[section][0] = (float)(b1 * 0.5);
        config->band_coef[section][1] = (float)b1;
        config->band_coef[section][2] = (float)(b1 * 0.5);
        config->band_coef[section][3] = (float)(-2.0 * sc.c / a0);
        config->band_coef[section][4] = (float)((1.0 - alpha) / a0);
    }
    config->band_limit = true;
}

void audiodsp_feedback_delay_config_init(
    audiodsp_feedback_delay_config_t *config, uint32_t sample_rate,
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
    config->wow_depth_ramp_ms = AUDIODSP_FEEDBACK_DELAY_WOW_DEPTH_RAMP_MS;
    config->wow_am_depth = 0.0f;
    config->loop_semitones = 0.0f;
    config->loop_window_ms = 25.0f;
    config->shift_step = 0;
    config->band_limit = false;
    shift_window_finish(config);
    audiodsp_feedback_delay_configure(config,
        AUDIODSP_FEEDBACK_DELAY_OPT_INPUT_PAN, 0.0f);
}

void audiodsp_feedback_delay_set_channel_count(
    audiodsp_feedback_delay_config_t *config, uint32_t channel_count) {
    config->channel_count = channel_count == 1u ? 1u : 2u;
    if (config->channel_count == 1u) {
        config->feed_own[0] = 1.0f;
        config->feed_other[0] = 0.0f;
        config->feed_own[1] = 0.0f;
        config->feed_other[1] = 0.0f;
    }
}

void audiodsp_feedback_delay_configure(audiodsp_feedback_delay_config_t *config,
    audiodsp_feedback_delay_option_t option, float value) {
    const float rate = (float)config->sample_rate;
    switch (option) {
        case AUDIODSP_FEEDBACK_DELAY_OPT_DELAY_MS: {
            // One frame of headroom below the line's length: the read walks
            // backward from the write cursor and interpolates between two
            // neighbours, so it must not be able to land on the cursor itself.
            float frames = value * rate / 1000.0f;
            config->delay_frames =
                clampf(frames, 1.0f, (float)config->line_frames - 2.0f);
            break;
        }
        case AUDIODSP_FEEDBACK_DELAY_OPT_DELAY_FRAMES:
            // The same headroom as `delay_ms`, and no conversion at all.
            config->delay_frames =
                clampf(value, 1.0f, (float)config->line_frames - 2.0f);
            break;
        case AUDIODSP_FEEDBACK_DELAY_OPT_FEEDBACK:
            // Stops just short of 1: at exactly unity a loop with no filter
            // in it never decays, and with the soft-clip engaged it settles
            // into a self-oscillation that is not what anyone asked for.
            config->feedback = clampf(value, 0.0f, 0.99f);
            break;
        case AUDIODSP_FEEDBACK_DELAY_OPT_MIX:
            // 0..2, and the dry stays at unity until 1 -- `audiodelays.Echo`'s
            // convention, adopted deliberately rather than crossfaded. Six
            // delay classes sit on top of these two modules and `mix` has to
            // mean one thing across all of them; three of them are Echo's and
            // that convention is CircuitPython's to set. So 0.3 is "dry, plus
            // 30 percent wet", 1 is "dry plus all of it", and 2 is wet alone.
            config->mix = clampf(value, 0.0f, 2.0f);
            break;
        case AUDIODSP_FEEDBACK_DELAY_OPT_DAMPING_HZ:
            config->damping_hz = value;
            config->damping_coef = one_pole_coefficient(value,
                config->sample_rate);
            break;
        case AUDIODSP_FEEDBACK_DELAY_OPT_CUT_HZ:
            config->cut_hz = value;
            config->cut_coef = one_pole_coefficient(value,
                config->sample_rate);
            break;
        case AUDIODSP_FEEDBACK_DELAY_OPT_WOW_HZ:
            config->wow_hz = value;
            config->wow_step = value <= 0.0f ? 0.0f :
                2.0f * sinf(AUDIODSP_FEEDBACK_DELAY_PI * value / rate);
            // The shape table's accumulator runs at the same rate as the
            // magic circle, so the two stay in step and `wow_am_depth` reads
            // the same cycle the delay is moving on.
            config->wow_phase_step = phase_step_for(value, rate);
            break;
        case AUDIODSP_FEEDBACK_DELAY_OPT_WOW_DEPTH_MS:
            config->wow_depth_frames =
                clampf(value, 0.0f, 1000.0f) * rate / 1000.0f;
            break;
        case AUDIODSP_FEEDBACK_DELAY_OPT_CROSS_FEED:
            config->cross_feed = clampf(value, 0.0f, 1.0f);
            break;
        case AUDIODSP_FEEDBACK_DELAY_OPT_LOOP_DRIVE:
            config->loop_drive = clampf(value, 0.0f, 1.0f);
            break;
        case AUDIODSP_FEEDBACK_DELAY_OPT_DELAY_SLEW:
            // Delay-seconds per second, so it is the same number at any
            // sample rate: 1 is the read head standing still while the write
            // head runs, which is an octave for as long as the move lasts.
            // Clamped at 64 -- four octaves either way is past anything a
            // tape machine does, and it keeps a single frame from crossing
            // the whole line.
            config->delay_slew_frames = clampf(value, 0.0f, 64.0f);
            break;
        case AUDIODSP_FEEDBACK_DELAY_OPT_WOW_DEPTH_RAMP_MS:
            // Read when a depth change arrives, so a new time takes the next
            // move and leaves one already under way alone.
            config->wow_depth_ramp_ms = clampf(value, 0.0f, 60000.0f);
            break;
        case AUDIODSP_FEEDBACK_DELAY_OPT_WOW_AM_DEPTH:
            config->wow_am_depth = clampf(value, 0.0f, 1.0f);
            break;
        case AUDIODSP_FEEDBACK_DELAY_OPT_LOOP_SEMITONES:
            // Two octaves either way. Past that the crossfade is doing more
            // than the signal is.
            config->loop_semitones = clampf(value, -24.0f, 24.0f);
            config->shift_step = shift_step_for(config);
            band_limit_finish(config, powf(2.0f,
                config->loop_semitones * (1.0f / 12.0f)));
            break;
        case AUDIODSP_FEEDBACK_DELAY_OPT_LOOP_WINDOW_MS:
            config->loop_window_ms = clampf(value, 1.0f, 250.0f);
            shift_window_finish(config);
            config->shift_step = shift_step_for(config);
            break;
        case AUDIODSP_FEEDBACK_DELAY_OPT_INPUT_PAN: {
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

void audiodsp_feedback_delay_config_finish(
    audiodsp_feedback_delay_config_t *config) {
    audiodsp_feedback_delay_configure(config,
        AUDIODSP_FEEDBACK_DELAY_OPT_DAMPING_HZ, config->damping_hz);
    audiodsp_feedback_delay_configure(config,
        AUDIODSP_FEEDBACK_DELAY_OPT_CUT_HZ, config->cut_hz);
    audiodsp_feedback_delay_configure(config,
        AUDIODSP_FEEDBACK_DELAY_OPT_WOW_HZ, config->wow_hz);
    shift_window_finish(config);
    audiodsp_feedback_delay_configure(config,
        AUDIODSP_FEEDBACK_DELAY_OPT_LOOP_SEMITONES, config->loop_semitones);
}

bool audiodsp_feedback_delay_set_wow_shape(
    audiodsp_feedback_delay_config_t *config, const int16_t *table,
    uint32_t length) {
    if (table == NULL) {
        config->wow_shape = NULL;
        config->wow_shape_shift = 0;
        return true;
    }
    if (length < AUDIODSP_FEEDBACK_DELAY_WOW_SHAPE_MIN ||
        length > AUDIODSP_FEEDBACK_DELAY_WOW_SHAPE_MAX ||
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

void audiodsp_feedback_delay_state_init(audiodsp_feedback_delay_state_t *state,
    int16_t *line) {
    state->line = line;
    state->write_frame = 0;
    state->damping_state[0] = state->damping_state[1] = 0.0f;
    state->cut_state[0] = state->cut_state[1] = 0.0f;
    state->wow_sine = 0.0f;
    state->wow_cosine = 1.0f;
    state->wow_phase = 0;
    state->shift_phase = 0;
    state->shift_align[0] = state->shift_align[1] = 0.0f;
    // Not primed. The first block snaps the read head onto whatever delay is
    // configured; only a change *after* that glides.
    state->delay_current = -1.0f;
    state->wow_depth_current = -1.0f;
    state->wow_depth_target = 0.0f;
    state->wow_depth_step = 0.0f;
    state->wow_depth_left = 0;
    state->wow_value = 0.0f;
    state->wow_parked = false;
    state->shift_engaged = false;
    state->shift_parked = false;
    state->wow_coast_step = 0.0f;
    state->wow_coast_phase_step = 0;
    state->shift_coast_step = 0;
    memset(state->band_state, 0, sizeof(state->band_state));
    state->band_running = false;
    memset(state->band_now, 0, sizeof(state->band_now));
    state->resting = false;
    state->quiet_frames = 0;
}

void audiodsp_feedback_delay_reset(audiodsp_feedback_delay_state_t *state,
    const audiodsp_feedback_delay_config_t *config) {
    if (state->line != NULL) {
        memset(state->line, 0,
            (size_t)config->line_frames * 2u * sizeof(int16_t));
    }
    int16_t *line = state->line;
    audiodsp_feedback_delay_state_init(state, line);
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

// What goes back round the loop: `feedback * sent`, made unable to hold a
// value forever (audiodsp#153).
//
// The write rounds to nearest, and rounded to nearest a repeat x comes back as
// round(feedback * x): every |x| <= 0.5 / (1 - feedback) is its own image, so
// 1 LSB from feedback 0.5, 5 at 0.9 and 50 at the 0.99 clamp went round the
// line forever. Exactly there -- where what comes back is within half an LSB
// of what went in, so rounding could hand it back unchanged -- the term is
// truncated toward zero instead, and |trunc(feedback * x)| < |x|. Everywhere
// else rounding already makes it smaller (|round(p)| <= |p| + 0.5 < |x|), and
// the term is left exactly as it was. Either way, with nothing coming in, no
// write is as large as what the loop sent it, so the largest value on the
// line falls by at least one LSB a lap and reaches exact zero. The
// interpolation, the damping low-pass, the soft-clip, the cross-feed and the
// shifter's crossfade are each a convex mix or a shrink and cannot undo that;
// the cut high-pass can overshoot, and is measured rather than argued
// (test_cpython_audioecho, E10). The damping low-pass is convex only once its
// state lands: in float32 it stopped a few ulps above a steady line, which
// was enough to hand that line back (audiodsp#157), so near the floor a state
// that has stopped moving is set onto its input (E11).
//
// The cross-feed is convex only in exact arithmetic. In float32
// `own * direct + other * crossed` can land an ulp above both lanes: with
// both lanes of a stereo line at k, one or two float32 steps under
// f = 1 - 0.5 / k, the test saw a gap a hair over 0.5, rounded, and wrote k
// back on every lap (audiodsp#170, E14). So a gap within EDGE of 0.5 is not
// trusted: there the write is worked out, and rounding stands only if what
// comes back is smaller than the larger lane. That is the property the whole
// argument needs, checked directly, so it holds however the sum was rounded
// -- each product on its own or fused into a multiply-add, in either order --
// and it never touches a write that was already smaller than both lanes,
// which is every write in mono, at cross-feed 0 or 1, and in stereo wherever
// the lanes differ. How far the sum can overshoot is bounded: a few float32
// steps of a value no larger than 51 (the test only bites where |sent| <=
// 0.5 / (1 - 0.99)), under 1.1e-5, so EDGE is 2^-14 = 6.1e-5 and a gap past
// it goes straight to rounding, one compare, as before. (A margin on the
// test alone, the same constant with no check behind it, also ends every
// tail, but it moves the edge for every sample that crosses it, and loud
// passages cross it at every zero crossing: it moved 90 of 286 audible
// renders by a 1 LSB flip carried round the loop.)
//
// Why not truncate always, as audiodsp_tank.c's tank_quantize() does: that
// takes half an LSB on average from every pass at every level, and a short
// loop makes hundreds of passes a second. On a 3 ms flanger at the clamp it
// ended the ring half a second early, 5 dB down at 1.5 s. Stepping only where
// rounding would stall keeps every repeat above the stall region
// byte-identical and changes only the part that used to circulate for ever.
#define AUDIODSP_FEEDBACK_DELAY_STALL_EDGE (1.0f / 16384.0f)

static float recirculated(float sent, float own, float other,
    float feedback) {
    const float back = feedback * sent;
    const float in_size = sent < 0.0f ? -sent : sent;
    const float back_size = back < 0.0f ? -back : back;
    const float gap = in_size - back_size;
    if (gap > 0.5f + AUDIODSP_FEEDBACK_DELAY_STALL_EDGE) {
        return back;
    }
    if (gap > 0.5f) {
        // Rounded, `back` would write this (to_s16 of it alone); keep it only
        // if that is smaller than the larger lane (audiodsp#170).
        const float own_size = own < 0.0f ? -own : own;
        const float other_size = other < 0.0f ? -other : other;
        const float larger = own_size > other_size ? own_size : other_size;
        if ((float)(int32_t)(back_size + 0.5f) < larger) {
            return back;
        }
    }
    // Here |back| <= |sent| <= (0.5 + EDGE) / (1 - feedback), about 50
    // under the 0.99 clamp, so the cast is defined.
    return (float)(int32_t)back;
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
    const audiodsp_feedback_delay_state_t *state, uint32_t length,
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

static void feedback_delay_run(
    const audiodsp_feedback_delay_config_t *config,
    audiodsp_feedback_delay_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames);

// The splice. A tap starting again from zero gain is placed where the line
// it reads is most like the line the other tap is reading, within half a
// window of where its phase puts it, so the two fade across in phase.
// Fixed half a window apart, they were in antiphase on every note whose
// period goes into half the window an odd number of halves -- G3 against a
// 25 ms window -- and the crossfade cancelled the note, leaving its
// neighbours a grain rate either side.
//
// The measure is the sum of absolute differences between what each tap
// will read on the first lane over the next 16 ms (no more than the window
// or the delay allow), ALIGN_LENGTH frames a 3000th of a second apart, in
// integers, so every interpreter lands on the
// same frame. A difference, not a correlation: over a stretch shorter than
// a low note's period a correlation peaks away from the matching phase, and
// a difference is zero exactly there. 16 ms, not the 0.7 ms of 32 adjacent
// frames, so a chord is matched as a chord: matched over a stretch shorter
// than its root's period, a C3 triad kept one note in phase and lost its
// root by 23 dB. Searched every ALIGN_STRIDE frames across a 40th of the
// rate (25 ms) either way, never more than half the window, then frame by
// frame around the best; a tie, silence included, keeps the smaller move,
// so a quiet line splices where it always did.
#define AUDIODSP_FEEDBACK_DELAY_ALIGN_LENGTH 48
#define AUDIODSP_FEEDBACK_DELAY_ALIGN_STEP_DIVISOR 3000u
#define AUDIODSP_FEEDBACK_DELAY_ALIGN_STRIDE 6
#define AUDIODSP_FEEDBACK_DELAY_ALIGN_DIVISOR 40u

static int64_t align_score(const int16_t *line, uint32_t length,
    uint32_t channels, uint32_t write_frame, int32_t other, int32_t candidate,
    uint32_t pace) {
    int64_t sum = 0;
    for (uint32_t channel = 0; channel < channels; ++channel) {
        const int16_t *lane = line + (size_t)channel * length;
        uint32_t a = (write_frame + length - (uint32_t)other) % length;
        uint32_t b = (write_frame + length - (uint32_t)candidate) % length;
        const uint32_t reach = (AUDIODSP_FEEDBACK_DELAY_ALIGN_LENGTH - 1) *
            pace;
        if (a + reach < length && b + reach < length) {
            // Neither stretch wraps: no remainder in the inner loop.
            const int16_t *x = lane + a;
            const int16_t *y = lane + b;
            for (int k = 0; k < AUDIODSP_FEEDBACK_DELAY_ALIGN_LENGTH; ++k) {
                const int32_t gap = (int32_t)*x - (int32_t)*y;
                sum += gap < 0 ? -gap : gap;
                x += pace;
                y += pace;
            }
            continue;
        }
        for (int k = 0; k < AUDIODSP_FEEDBACK_DELAY_ALIGN_LENGTH; ++k) {
            const int32_t gap = (int32_t)lane[a] - (int32_t)lane[b];
            sum += gap < 0 ? -gap : gap;
            a = (a + pace) % length;
            b = (b + pace) % length;
        }
    }
    return sum;
}

// The move, in whole frames, for a tap whose phase puts it `nominal` frames
// back, against the other tap `other` frames back. Both are read forward
// from there, so neither may come within ALIGN_LENGTH of the write cursor or
// past the far end of the line; a move that would is not considered.
static float splice_align(const audiodsp_feedback_delay_config_t *config,
    const audiodsp_feedback_delay_state_t *state, float nominal, float other) {
    const uint32_t length = config->line_frames;
    // Never more than half the window: a tap may not wander past the grain
    // it reads, and a short window, which splices often, searches little.
    int32_t span = (int32_t)(config->sample_rate /
        AUDIODSP_FEEDBACK_DELAY_ALIGN_DIVISOR);
    const int32_t window = (int32_t)config->shift_window_frames;
    // Half the window, but a short window gets up to a 100th of the rate
    // (or four windows, if that is less): it needs room past its grain to
    // find a held note's phase, and it splices so often that a full 10 ms at
    // a 1 ms window would cost more than a board's block.
    int32_t room = window / 2;
    int32_t floor_room = (int32_t)(config->sample_rate / 100u);
    if (floor_room > window * 4) {
        floor_room = window * 4;
    }
    if (room < floor_room) {
        room = floor_room;
    }
    if (span > room) {
        span = room;
    }

    const int32_t base = (int32_t)nominal;
    const int32_t against = (int32_t)other;
    // The compared stretch is 16 ms, but never longer than the window (a
    // grain is all a splice joins), nor than the shortest delay it must
    // read back from without reaching the write cursor.
    const int32_t nearest = against < base - span ? against : base - span;
    uint32_t pace = config->sample_rate /
        AUDIODSP_FEEDBACK_DELAY_ALIGN_STEP_DIVISOR;
    const int32_t steps = AUDIODSP_FEEDBACK_DELAY_ALIGN_LENGTH;
    if ((int32_t)pace * steps > window) {
        // A window shorter than the stretch compares adjacent frames, as a
        // grain that short holds no chord to match (a spaced stretch there
        // matched the line past the grain and lost held notes).
        pace = 1u;
    }
    if (nearest > 1 && (int32_t)pace * steps > nearest - 1) {
        pace = (uint32_t)((nearest - 1) / steps);
    }
    if (pace < 1u) {
        pace = 1u;
    }
    const int32_t low = (int32_t)(steps * pace) + 1;
    const int32_t high = (int32_t)length - 2;
    if (span <= 0 || against < low || against > high) {
        return 0.0f;
    }
    // The first lane alone decides, so a stereo node splices its left lane
    // exactly as a mono node fed the left lane does.
    const uint32_t channels = 1u;
    int32_t best = 0;
    int64_t best_score = 0;
    bool found = false;
    // 0 first, then outward, so a tie keeps the smaller move.
    for (int32_t step = 0; step <= span;
         step += AUDIODSP_FEEDBACK_DELAY_ALIGN_STRIDE) {
        for (int side = 0; side < (step == 0 ? 1 : 2); ++side) {
            const int32_t move = side ? -step : step;
            const int32_t at = base + move;
            if (at < low || at > high) {
                continue;
            }
            const int64_t score = align_score(state->line, length, channels,
                state->write_frame, against, at, pace);
            if (!found || score < best_score) {
                found = true;
                best = move;
                best_score = score;
            }
        }
    }
    if (!found) {
        return 0.0f;
    }
    const int32_t coarse = best;
    for (int32_t fine = -(AUDIODSP_FEEDBACK_DELAY_ALIGN_STRIDE - 1);
         fine <= AUDIODSP_FEEDBACK_DELAY_ALIGN_STRIDE - 1; ++fine) {
        const int32_t move = coarse + fine;
        const int32_t at = base + move;
        if (fine == 0 || move > span || move < -span || at < low ||
            at > high) {
            continue;
        }
        const int64_t score = align_score(state->line, length, channels,
            state->write_frame, against, at, pace);
        const int32_t size = move < 0 ? -move : move;
        const int32_t best_size = best < 0 ? -best : best;
        if (score < best_score || (score == best_score && size < best_size)) {
            best = move;
            best_score = score;
        }
    }
    return (float)best;
}

void audiodsp_feedback_delay_process_s16(
    const audiodsp_feedback_delay_config_t *config,
    audiodsp_feedback_delay_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames) {
    if (frames > 0u) {
        state->resting = false;
        state->quiet_frames = 0;
    }
    feedback_delay_run(config, state, out, in, frames);
}

// Below this a filter state is a fraction of an LSB that no write can round
// up to one: the line holds it at zero, so it is set to zero when the node
// rests rather than left to decay through float32's denormals for ever.
#define AUDIODSP_FEEDBACK_DELAY_REST_FLOOR (1.0f / 1024.0f)

static bool feedback_delay_below_floor(float value) {
    return (value < 0.0f ? -value : value) < AUDIODSP_FEEDBACK_DELAY_REST_FLOOR;
}

// Everything audible is zero: the whole line, both lanes, and the two loop
// filters to within a fraction of an LSB.
static bool feedback_delay_settled(
    const audiodsp_feedback_delay_config_t *config,
    const audiodsp_feedback_delay_state_t *state) {
    for (uint32_t ch = 0; ch < 2u; ++ch) {
        if (!feedback_delay_below_floor(state->damping_state[ch]) ||
            !feedback_delay_below_floor(state->cut_state[ch])) {
            return false;
        }
        for (uint32_t section = 0; section < 2u; ++section) {
            if (!feedback_delay_below_floor(state->band_state[ch][section][0]) ||
                !feedback_delay_below_floor(state->band_state[ch][section][1])) {
                return false;
            }
        }
    }
    const size_t samples = (size_t)config->line_frames * 2u;
    for (size_t i = 0; i < samples; ++i) {
        if (state->line[i] != 0) {
            return false;
        }
    }
    return true;
}

void audiodsp_feedback_delay_process_silence(
    const audiodsp_feedback_delay_config_t *config,
    audiodsp_feedback_delay_state_t *state, int16_t *out, uint32_t frames) {
    const size_t samples = (size_t)frames * config->channel_count;
    memset(out, 0, samples * sizeof(int16_t));
    if (state->resting || frames == 0u || state->line == NULL) {
        return;
    }
    // `out` may alias `in`, so the zeros just written are the input.
    feedback_delay_run(config, state, out, out, frames);
    for (size_t i = 0; i < samples; ++i) {
        if (out[i] != 0) {
            state->quiet_frames = 0;
            return;
        }
    }
    state->quiet_frames += frames;
    if (state->quiet_frames < config->line_frames) {
        return;
    }
    state->quiet_frames = 0;
    if (feedback_delay_settled(config, state)) {
        state->damping_state[0] = state->damping_state[1] = 0.0f;
        state->cut_state[0] = state->cut_state[1] = 0.0f;
        memset(state->band_state, 0, sizeof(state->band_state));
        state->resting = true;
    }
}

static void feedback_delay_run(
    const audiodsp_feedback_delay_config_t *config,
    audiodsp_feedback_delay_state_t *state, int16_t *out, const int16_t *in,
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
    // `wow_hz` at 0 does not freeze the oscillator mid-swing, which left the
    // read head displaced and the echo off pitch for as long as it stayed
    // there (audiodsp#177). It runs on at its last rate to the next zero
    // crossing and parks at centre; a new rate starts it again from there.
    const bool wow_stopped =
        config->wow_step == 0.0f && config->wow_phase_step == 0u;
    if (!wow_stopped) {
        state->wow_parked = false;
        state->wow_coast_step = config->wow_step;
        state->wow_coast_phase_step = config->wow_phase_step;
    }
    const bool wow_coasting = wow_stopped && !state->wow_parked &&
        (state->wow_coast_step != 0.0f || state->wow_coast_phase_step != 0u);
    const float wow_step = wow_coasting ? state->wow_coast_step :
        config->wow_step;
    const uint32_t wow_phase_step = wow_coasting ?
        state->wow_coast_phase_step : config->wow_phase_step;
    // The pitch shifter the same way. Once it has run, `loop_semitones` at 0
    // turns its crossfade on to the next point where one tap carries all the
    // gain (a phase of 0 or one half) and holds it there, so the read head
    // never steps by the half window between the taps; back on, it continues
    // from that phase rather than from wherever it stopped.
    if (shifting) {
        state->shift_engaged = true;
        state->shift_parked = false;
        if (config->shift_step != 0) {
            state->shift_coast_step = config->shift_step;
        }
    }
    const bool shift_path = shifting || state->shift_engaged;
    const int32_t shift_step = shifting ? config->shift_step :
        (state->shift_parked ? 0 : state->shift_coast_step);
    // With no slew the read head is wherever it was told to be, so `offset`
    // below is the same float it always was and every existing golden holds.
    // A state that has never run is primed the same way, so turning the slew
    // on at construction does not make the first block glide from nowhere.
    if (slew <= 0.0f || state->delay_current < 0.0f) {
        state->delay_current = config->delay_frames;
    }
    // A new wow depth ramps in over 20 ms instead of landing on the next
    // frame (audiodsp#160). Landing at once moved the read head by the whole
    // change between two samples, a click and a step in pitch; SlapbackDelay
    // measured 7 684 LSB against a tone whose own largest step was 1 565. A
    // straight line in a fixed time, so the extra pitch while it travels is
    // the change over 20 ms times the wow (a 0.46 ms move is 2.3 % at the
    // wow's crest), and the last frame lands on the target exactly, so a depth
    // that is not moving is the same float it always was and renders the
    // same bytes. A fresh state starts on its depth, as the delay does.
    const float depth_target = config->wow_depth_frames;
    if (state->wow_depth_current < 0.0f) {
        state->wow_depth_current = depth_target;
        state->wow_depth_target = depth_target;
        state->wow_depth_left = 0;
    } else if (depth_target != state->wow_depth_target) {
        // At the default 20 ms this is exactly sample_rate / 50: 20 x rate
        // is a whole number float holds exactly, and its thousandth is
        // either whole or at least 0.02 from one, so the truncation lands
        // where the integer division did.
        uint32_t ramp = (uint32_t)(config->wow_depth_ramp_ms
            * (float)config->sample_rate / 1000.0f);
        if (ramp < 1u) {
            ramp = 1u;
        }
        state->wow_depth_target = depth_target;
        state->wow_depth_step =
            (depth_target - state->wow_depth_current) / (float)ramp;
        state->wow_depth_left = ramp;
    }
    float depth = state->wow_depth_current;
    uint32_t depth_left = state->wow_depth_left;
    const float depth_step = state->wow_depth_step;
    // While the cut high-pass is out its state is held at zero, the state
    // at which its output is its input (audiodsp#159). Left frozen, putting
    // the filter back in subtracted whatever it last held from silence,
    // 19 208 LSB measured. At zero, switching it in is continuous, the same
    // as a node built with it, and the low end then falls away at the
    // corner's own rate. Following the signal instead, as the low-pass does,
    // would subtract the signal's last value on return, which is the same
    // click. Once per block: `config` cannot change inside one.
    if (config->cut_coef <= 0.0f) {
        state->cut_state[0] = state->cut_state[1] = 0.0f;
    }
    // The band limit switched in starts from the line's last write on each
    // lane, as if that value had been standing, so it neither dips to zero
    // nor plays a state left from before. Switched out, it is not run, and
    // its state is zero, so a node that has stopped shifting up can rest.
    const bool band = config->band_limit;
    if (!band) {
        memset(state->band_state, 0, sizeof(state->band_state));
    } else if (!state->band_running) {
        memcpy(state->band_now, config->band_coef, sizeof(state->band_now));
        const uint32_t last = (state->write_frame + length - 1u) % length;
        for (uint32_t channel = 0; channel < 2u; ++channel) {
            const float held = (float)state->line[(size_t)channel * length +
                last];
            for (uint32_t section = 0; section < 2u; ++section) {
                const float *k = config->band_coef[section];
                state->band_state[channel][section][0] = held - k[0] * held;
                state->band_state[channel][section][1] =
                    k[2] * held - k[4] * held;
            }
        }
    }
    state->band_running = band;
    // A corner that moved is walked to across this block, a frame at a
    // time, and lands exactly on the new coefficients at its last frame.
    bool band_moving = false;
    float band_step[2][5];
    if (band && frames > 0u &&
        memcmp(state->band_now, config->band_coef, sizeof(band_step)) != 0) {
        band_moving = true;
        for (uint32_t section = 0; section < 2u; ++section) {
            for (uint32_t c = 0; c < 5u; ++c) {
                band_step[section][c] = (config->band_coef[section][c] -
                    state->band_now[section][c]) / (float)frames;
            }
        }
    }
    feedback_delay_tap_t near_tap = { 0u, 0u, 0.0f };
    feedback_delay_tap_t far_tap = { 0u, 0u, 0.0f };
    float near_gain = 1.0f;
    float far_gain = 0.0f;
    // The last frame's tap per channel, for the damping landing below.
    float tapped[2] = { 0.0f, 0.0f };
    for (uint32_t frame = 0; frame < frames; ++frame) {
        if (band_moving) {
            if (frame + 1u == frames) {
                memcpy(state->band_now, config->band_coef,
                    sizeof(state->band_now));
            } else {
                for (uint32_t section = 0; section < 2u; ++section) {
                    for (uint32_t c = 0; c < 5u; ++c) {
                        state->band_now[section][c] += band_step[section][c];
                    }
                }
            }
        }
        // Rotate the wow oscillator one step. Updating the sine first and
        // feeding the new value back into the cosine is what keeps this
        // stable indefinitely; the naive pair drifts in amplitude. It is
        // stepped whether or not a shape table is in use: `wow_am_depth`
        // reads it, and skipping it would move the default path.
        if (!state->wow_parked) {
            state->wow_sine += wow_step * state->wow_cosine;
            state->wow_cosine -= wow_step * state->wow_sine;
        }

        // A bucket brigade's delay is the line over its clock, so the Small
        // Clone's triangle on the clock arrives as a reciprocal on the delay
        // and no sine can be it. With a table the shape is Python's to bake
        // and this only looks it up.
        float wow = state->wow_sine;
        if (config->wow_shape != NULL) {
            state->wow_phase += state->wow_parked ? 0u : wow_phase_step;
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
        if (wow_coasting && !state->wow_parked) {
            const float last = state->wow_value;
            if (last == 0.0f || (last > 0.0f) != (wow > 0.0f) || wow == 0.0f) {
                // The crossing: park at centre. The circle restarts from 0
                // in the direction it was going, so a new rate carries on
                // from here.
                state->wow_parked = true;
                state->wow_sine = 0.0f;
                state->wow_cosine = state->wow_cosine < 0.0f ? -1.0f : 1.0f;
            }
        }
        if (state->wow_parked) {
            wow = 0.0f;
        }
        state->wow_value = wow;

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

        if (depth_left > 0u) {
            --depth_left;
            depth = depth_left == 0u ? depth_target : depth + depth_step;
        }
        const float offset = state->delay_current + depth * wow;
        if (shift_path) {
            // Two taps half a window apart, each walking one window per
            // crossfade turn, mixed with a triangular fade whose two halves
            // sum to exactly one. That is a resampled read: the head advances
            // at the pitch ratio while the line is written at unity, and the
            // shift is inside the loop, so every pass rises again.
            const uint32_t before = state->shift_phase;
            if (!state->shift_parked) {
                state->shift_phase += (uint32_t)shift_step;
            }
            if (!shifting && !state->shift_parked) {
                // Coasting to a phase of 0 or one half, whichever comes
                // first in the direction the crossfade turns.
                const uint32_t after = state->shift_phase;
                const uint32_t half = 0x80000000u;
                if (before == 0u || before == half) {
                    state->shift_phase = before;
                    state->shift_parked = true;
                } else if (shift_step > 0) {
                    if (after < before) {
                        state->shift_phase = 0u;
                        state->shift_parked = true;
                    } else if (before < half && after >= half) {
                        state->shift_phase = half;
                        state->shift_parked = true;
                    }
                } else if (shift_step < 0) {
                    if (after > before) {
                        state->shift_phase = 0u;
                        state->shift_parked = true;
                    } else if (before > half && after <= half) {
                        state->shift_phase = half;
                        state->shift_parked = true;
                    }
                }
            }
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
            // A tap whose gain has just passed through zero -- the near one
            // at a phase of 0, the far one at one half -- starts its next
            // grain: place it in phase with the other (the splice above).
            const uint32_t now = state->shift_phase;
            if (now != before) {
                const uint32_t half = 0x80000000u;
                const bool up = (int32_t)(now - before) > 0;
                const bool near_starts = before == 0u ||
                    (up ? now < before : now > before);
                const bool far_starts = before == half ||
                    (up ? (before < half && now >= half) :
                     (before > half && now <= half));
                if (near_starts) {
                    state->shift_align[0] = splice_align(config, state,
                        offset + turn * window,
                        offset + opposite * window + state->shift_align[1]);
                }
                if (far_starts) {
                    state->shift_align[1] = splice_align(config, state,
                        offset + opposite * window,
                        offset + turn * window + state->shift_align[0]);
                }
            }
            near_tap = tap_for(state, length,
                offset + turn * window + state->shift_align[0]);
            far_tap = tap_for(state, length,
                offset + opposite * window + state->shift_align[1]);
        } else {
            near_tap = tap_for(state, length, offset);
        }

        float loop[2];
        const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const int16_t *lane = state->line + (size_t)channel * length;
            float delayed = read_tap(lane, &near_tap);
            if (shift_path) {
                delayed = delayed * near_gain +
                    read_tap(lane, &far_tap) * far_gain;
            }

            float value = delayed;
            tapped[channel] = value;
            if (config->damping_coef > 0.0f) {
                state->damping_state[channel] += config->damping_coef *
                    (value - state->damping_state[channel]);
                value = state->damping_state[channel];
            } else {
                // Out of the loop, the state follows the tap (audiodsp#158),
                // so switching the low-pass back in starts from the signal
                // rather than from whatever it held when it went out -- which
                // played out of silence, 25 220 LSB measured. The output here
                // never reads it, so nothing that plays with the filter out
                // changes.
                state->damping_state[channel] = value;
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
            float written = fed + recirculated(sent, loop[channel], other,
                config->feedback);
            if (band) {
                for (uint32_t section = 0; section < 2u; ++section) {
                    const float *k = state->band_now[section];
                    float *z = state->band_state[channel][section];
                    const float y = k[0] * written + z[0];
                    z[0] = k[1] * written - k[3] * y + z[1];
                    z[1] = k[2] * written - k[4] * y;
                    written = y;
                }
            }
            state->line[(size_t)channel * length + state->write_frame] =
                to_s16(written);
            // The dry path is the channel's own signal, never the panned
            // one: `input_pan` steers what goes round the loop, not what the
            // listener hears straight through.
            out[frame * channels + channel] =
                to_s16(dry * source + wet_gain * loop[channel]);
        }
        state->write_frame = (state->write_frame + 1u) % length;
    }
    state->wow_depth_current = depth;
    state->wow_depth_left = depth_left;

    // Near the floor, a damping state that has stopped moving lands on its
    // input (audiodsp#157). In float32 the one-pole approaches a steady tap
    // from above and stops a few ulps short, where its step rounds away; left
    // there, `recirculated` saw a value a hair over the line's and handed the
    // line back unchanged, so 1 to 5 LSB went round for ever at the feedbacks
    // where 0.5 / (1 - feedback) is whole. A stuck state is a steady state,
    // so it is looked for once a block, on the last frame's tap, rather than
    // on every sample, where the test sat on the one-pole's own dependency
    // chain and cost 6 to 7 % of the node at -O2. It can only matter where
    // `recirculated` rounds or truncates by the stall test, |sent| <=
    // 0.5 / (1 - 0.99) = 50 LSB, so it is kept to |state| <= 64: above that,
    // and wherever the state still moves, nothing changes.
    if (frames > 0u && config->damping_coef > 0.0f) {
        const uint32_t lanes = config->channel_count == 1u ? 1u : 2u;
        for (uint32_t channel = 0; channel < lanes; ++channel) {
            const float held = state->damping_state[channel];
            if (held <= 64.0f && held >= -64.0f &&
                held + config->damping_coef * (tapped[channel] - held) ==
                held) {
                state->damping_state[channel] = tapped[channel];
            }
        }
    }
}
