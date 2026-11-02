// SPDX-License-Identifier: MIT

#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_modal.h"

#include "shared/audiodsp_trig.h"

#include <string.h>

// ln(1000) -- a mode's `decay` is its 60 dB time, so the per-sample pole
// radius is exp(-LN1000 / (decay * sample_rate)).
#define AUDIODSP_MODAL_LN1000 6.907755278982137

// Frames the process loop takes per pass over the modes. Two float arrays of
// this many live on the audio thread's stack, 512 bytes at 64; a longer
// stretch saves only a reload of each mode's state per pass.
#define AUDIODSP_MODAL_STRETCH 64u

// Nothing here calls libm. `audiodsp_sincos()` supplies the angle, and the
// exponential below is a series with range reduction, for the reason
// audiodsp_trig.h gives: three platforms' `exp()` agree to within an ulp and
// differ in the last place, and this port's rule is that one hash of one
// probe matches on all three.

static float clampf(float value, float low, float high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

// Round half away from zero, then clamp -- the same conversion
// audiodsp_filter_f32.c and audiodsp_feedback_delay.c use, so a chain built out
// of several of them rounds one way.
static int16_t to_s16(float value) {
    if (value > 32767.0f) {
        return 32767;
    }
    if (value < -32768.0f) {
        return -32768;
    }
    return (int16_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

// |x|, inline on every target (no libm), and a hint that a branch is rare.
#if defined(__GNUC__) || defined(__clang__)
#define MODAL_FABS(x) __builtin_fabsf(x)
#define MODAL_COPYSIGN(x, y) __builtin_copysignf(x, y)
#define MODAL_FMIN(x, y) __builtin_fminf(x, y)
#define MODAL_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define MODAL_LIKELY(x) __builtin_expect(!!(x), 1)
#else
#include <math.h>
#define MODAL_FABS(x) fabsf(x)
#define MODAL_COPYSIGN(x, y) copysignf(x, y)
#define MODAL_FMIN(x, y) fminf(x, y)
#define MODAL_UNLIKELY(x) (x)
#define MODAL_LIKELY(x) (x)
#endif

// to_s16() without its branches, for a value already known to be in range:
// value + copysign(0.5, value) is the same addition to_s16() makes (adding
// -0.5 is subtracting 0.5), and -0.0f comes out 0 either way.
static inline int in_s16(float value) {
    return (value <= 32767.0f) & (value >= -32768.0f);
}

static inline int16_t round_s16(float value) {
    return (int16_t)(value + MODAL_COPYSIGN(0.5f, value));
}

// What makes a decaying float recursion actually arrive at zero, and here also
// what makes "this mode has finished" an exact test.
//
// BOTH WORDS OR NEITHER, and that is not tidiness. audiodsp_filter_f32.c
// flushes each word on its own, which is safe there; here it produced a stable
// limit cycle *above* the threshold, measured at 220 Hz / 0.125 s decay /
// 8 kHz: the state parked at ~2.1e-19 and was still there after two thousand
// blocks, thirty-eight seconds of audio, with the output long since silent.
//
// The mechanism, in the one place it can be seen. With no input the recursion
// is s1' = -a1*y0 + s2 and s2' = -a2*y0, y0 = s1, and -a1 is close to 2 for a
// high-Q pole (1.9567 for that mode). Flushing independently zeroes s2 first,
// because a2 < -a1 makes it the smaller word; the very next sample then
// computes s1' = 1.9567 * y0 + 0, which is nearly twice what it was. That
// pushes the pair back above the threshold, s2 refills from it, and the cycle
// repeats for ever. The flush was not ending the tail, it was feeding it.
//
// Zeroing the pair together cannot do that: the only state it can produce is
// the one where the whole mode is off.
//
// A word is nothing when |s| < FLUSH, so -FLUSH and FLUSH themselves are
// not, and neither is a NaN. The process loop tests s2 alone on every sample,
// one compare on one branch that a ringing mode never takes, and only when
// that passes does flush_pair() look at both.
static inline int maybe_tiny(float s) {
    return MODAL_FABS(s) < AUDIODSP_MODAL_FLUSH;
}

// tiny() is the same comparison as maybe_tiny(), written apart so that the
// compiler does not keep the fast path's |s| alive for it.
static inline bool tiny(float value) {
    return value > -AUDIODSP_MODAL_FLUSH && value < AUDIODSP_MODAL_FLUSH;
}

static inline void flush_pair(float *s1, float *s2) {
    if (tiny(*s1) && tiny(*s2)) {
        *s1 = 0.0f;
        *s2 = 0.0f;
    }
}

// Four silent modes across one stretch: the sustained note's case, so it is
// the one with the most of its own. Four recursions to interleave hide the
// FPU's latency, and acc[j] is loaded and stored once for all four, taking
// their y0 in ascending order.
//
// Two samples a pass, with no branch between them: the second is worked out
// from the first as if neither flushed, and one test of the smallest |s2| of
// the eight says whether either could have (fminf() passes over a NaN, which
// can never flush, so a NaN only ever sends the pass down the slow path). On
// the slow path the second sample is worked out again from the first one's
// flushed state, exactly as one sample at a time would.
static void ring4(const audiodsp_modal_config_t *config,
    audiodsp_modal_state_t *state, uint32_t channels, uint32_t channel,
    const uint8_t *modes, float *acc, uint32_t n) {
    const audiodsp_modal_coeff_t *c = config->coeffs;
    const uint32_t wa = modes[0] * channels + channel;
    const uint32_t wb = modes[1] * channels + channel;
    const uint32_t wc = modes[2] * channels + channel;
    const uint32_t wd = modes[3] * channels + channel;
    const float na1a = -c[modes[0]].a1;
    const float na2a = -c[modes[0]].a2;
    const float na1b = -c[modes[1]].a1;
    const float na2b = -c[modes[1]].a2;
    const float na1c = -c[modes[2]].a1;
    const float na2c = -c[modes[2]].a2;
    const float na1d = -c[modes[3]].a1;
    const float na2d = -c[modes[3]].a2;
    float s1a = state->s1[wa];
    float s2a = state->s2[wa];
    float s1b = state->s1[wb];
    float s2b = state->s2[wb];
    float s1c = state->s1[wc];
    float s2c = state->s2[wc];
    float s1d = state->s1[wd];
    float s2d = state->s2[wd];
    uint32_t j = 0;
    for (; j + 2u <= n; j += 2u) {
        // First sample: y0 is s1.
        float sum0 = acc[j];
        sum0 += s1a;
        sum0 += s1b;
        sum0 += s1c;
        sum0 += s1d;
        float t1a = na1a * s1a + s2a;
        float t2a = na2a * s1a;
        float t1b = na1b * s1b + s2b;
        float t2b = na2b * s1b;
        float t1c = na1c * s1c + s2c;
        float t2c = na2c * s1c;
        float t1d = na1d * s1d + s2d;
        float t2d = na2d * s1d;
        // Second sample, from the first's state as if nothing flushed.
        s1a = na1a * t1a + t2a;
        s2a = na2a * t1a;
        s1b = na1b * t1b + t2b;
        s2b = na2b * t1b;
        s1c = na1c * t1c + t2c;
        s2c = na2c * t1c;
        s1d = na1d * t1d + t2d;
        s2d = na2d * t1d;
        const float low = MODAL_FMIN(
            MODAL_FMIN(MODAL_FMIN(MODAL_FABS(t2a), MODAL_FABS(t2b)),
                MODAL_FMIN(MODAL_FABS(t2c), MODAL_FABS(t2d))),
            MODAL_FMIN(MODAL_FMIN(MODAL_FABS(s2a), MODAL_FABS(s2b)),
                MODAL_FMIN(MODAL_FABS(s2c), MODAL_FABS(s2d))));
        if (MODAL_UNLIKELY(low < AUDIODSP_MODAL_FLUSH)) {
            flush_pair(&t1a, &t2a);
            flush_pair(&t1b, &t2b);
            flush_pair(&t1c, &t2c);
            flush_pair(&t1d, &t2d);
            s1a = na1a * t1a + t2a;
            s2a = na2a * t1a;
            s1b = na1b * t1b + t2b;
            s2b = na2b * t1b;
            s1c = na1c * t1c + t2c;
            s2c = na2c * t1c;
            s1d = na1d * t1d + t2d;
            s2d = na2d * t1d;
            flush_pair(&s1a, &s2a);
            flush_pair(&s1b, &s2b);
            flush_pair(&s1c, &s2c);
            flush_pair(&s1d, &s2d);
        }
        float sum1 = acc[j + 1u];
        sum1 += t1a;
        sum1 += t1b;
        sum1 += t1c;
        sum1 += t1d;
        acc[j] = sum0;
        acc[j + 1u] = sum1;
    }
    if (j < n) {
        float sum = acc[j];
        sum += s1a;
        sum += s1b;
        sum += s1c;
        sum += s1d;
        acc[j] = sum;
        const float ya = s1a;
        const float yb = s1b;
        const float yc = s1c;
        const float yd = s1d;
        s1a = na1a * ya + s2a;
        s2a = na2a * ya;
        s1b = na1b * yb + s2b;
        s2b = na2b * yb;
        s1c = na1c * yc + s2c;
        s2c = na2c * yc;
        s1d = na1d * yd + s2d;
        s2d = na2d * yd;
        flush_pair(&s1a, &s2a);
        flush_pair(&s1b, &s2b);
        flush_pair(&s1c, &s2c);
        flush_pair(&s1d, &s2d);
    }
    state->s1[wa] = s1a;
    state->s2[wa] = s2a;
    state->s1[wb] = s1b;
    state->s2[wb] = s2b;
    state->s1[wc] = s1c;
    state->s2[wc] = s2c;
    state->s1[wd] = s1d;
    state->s2[wd] = s2d;
}

// exp(-x) for x >= 0, in a fixed order on every platform.
//
// Halve the argument until it is under an eighth, evaluate seven Taylor terms
// there, then square back. Over [0, 1/8] the first dropped term is
// x^8/8! < 1.5e-12, which is four orders below float32's epsilon, and the
// squaring is exact in the sense that matters: it is the same sequence of
// multiplications everywhere. Range reduction rather than a longer series
// because the argument can reach ~0.9 (a one-millisecond decay on an 8 kHz
// graph) and a single polynomial wide enough for that is both longer and
// worse at the small end, which is where every real mode sits.
static double audiodsp_modal_exp_neg(double x) {
    int halvings = 0;
    if (x < 0.0) {
        x = 0.0;
    }
    while (x > 0.125 && halvings < 24) {
        x *= 0.5;
        halvings++;
    }
    // exp(-x) = 1 - x + x^2/2! - ... , Horner from the last term inward so
    // the additions happen in one fixed order.
    double t = -x;
    double y = 1.0 + t * (1.0 + t * (1.0 / 2.0 + t * (1.0 / 6.0
        + t * (1.0 / 24.0 + t * (1.0 / 120.0 + t * (1.0 / 720.0
        + t * (1.0 / 5040.0)))))));
    while (halvings-- > 0) {
        y *= y;
    }
    return y;
}

void audiodsp_modal_config_init(audiodsp_modal_config_t *config,
    uint32_t sample_rate, uint32_t channel_count, uint32_t mode_count,
    audiodsp_modal_mode_t *modes, audiodsp_modal_coeff_t *coeffs) {
    memset(config, 0, sizeof(*config));
    config->sample_rate = sample_rate < 1u ? 1u : sample_rate;
    config->channel_count = channel_count == 1u ? 1u : 2u;
    if (mode_count < 1u) {
        mode_count = 1u;
    }
    if (mode_count > AUDIODSP_MODAL_MAX_MODES) {
        mode_count = AUDIODSP_MODAL_MAX_MODES;
    }
    config->mode_count = mode_count;
    config->mix = 1.0f;
    config->gain = 1.0f;
    config->modes = modes;
    config->coeffs = coeffs;
    for (uint32_t i = 0; i < mode_count; ++i) {
        // Silent by default: a bank nobody has configured makes no sound
        // rather than a chord of whatever happened to be in memory.
        modes[i].frequency = 100.0f;
        modes[i].decay = AUDIODSP_MODAL_MIN_DECAY;
        modes[i].gain = 0.0f;
    }
    config->derived = false;
}

void audiodsp_modal_configure(audiodsp_modal_config_t *config,
    audiodsp_modal_option_t option, float value) {
    switch (option) {
        case AUDIODSP_MODAL_OPT_MIX:
            config->mix = clampf(value, 0.0f, 1.0f);
            break;
        case AUDIODSP_MODAL_OPT_GAIN:
            config->gain = clampf(value, 0.0f, 16.0f);
            break;
        default:
            break;
    }
}

void audiodsp_modal_set_mode(audiodsp_modal_config_t *config, uint32_t index,
    float frequency, float decay, float gain) {
    if (index >= config->mode_count || config->modes == NULL) {
        return;
    }
    const float nyquist = (float)config->sample_rate * 0.5f;
    audiodsp_modal_mode_t *mode = &config->modes[index];
    // A mode at or above Nyquist is not a mode, it is an alias. Clamped just
    // under rather than refused, so a table written for 48 kHz still loads on
    // a 22.05 kHz graph with its top partials folded onto the ceiling instead
    // of wrapping down into the middle of the drum.
    mode->frequency = clampf(frequency, 0.0f, nyquist * 0.999f);
    mode->decay = clampf(decay, AUDIODSP_MODAL_MIN_DECAY,
        AUDIODSP_MODAL_MAX_DECAY);
    mode->gain = clampf(gain, -16.0f, 16.0f);
    config->derived = false;
}

void audiodsp_modal_config_finish(audiodsp_modal_config_t *config) {
    if (config->derived || config->modes == NULL || config->coeffs == NULL) {
        return;
    }
    const double rate = (double)config->sample_rate;
    for (uint32_t i = 0; i < config->mode_count; ++i) {
        const audiodsp_modal_mode_t *mode = &config->modes[i];
        audiodsp_modal_coeff_t *coeff = &config->coeffs[i];
        if (mode->frequency <= 0.0f) {
            // No pole to speak of. This is the only case that clears the
            // recursion as well as the input.
            coeff->b0 = 0.0f;
            coeff->a1 = 0.0f;
            coeff->a2 = 0.0f;
            continue;
        }
        // A gain of zero clears b0 and NOTHING ELSE, which is the difference
        // between muting a mode and stopping it. `b0` is how new signal gets
        // in; `a1` and `a2` are the pole, and a mode already in motion has to
        // keep its pole or it does not decay, it simply ceases.
        //
        // That distinction is what lets one bank hold a whole drum kit. A kit
        // arms the drum being struck and zeroes the gain of every other, so
        // one excitation plays one drum - and a crash struck four bars ago
        // goes on ringing underneath, because muting its input never touched
        // its recursion. Written the other way first, a kick silenced a
        // ringing crash outright: 7584 peak to 61.
        const double w0 = 2.0 * AUDIODSP_PI * (double)mode->frequency / rate;
        audiodsp_sincos_t sc;
        audiodsp_sincos(w0, &sc);
        const double r = audiodsp_modal_exp_neg(
            AUDIODSP_MODAL_LN1000 / ((double)mode->decay * rate));
        // y[n] = b0*x[n] - a1*y[n-1] - a2*y[n-2] has impulse response
        // r^n * sin(w0*(n+1)) / sin(w0); scaling b0 by sin(w0) makes the peak
        // of that response the caller's `gain`, which is the number a modal
        // table out of the literature actually publishes.
        coeff->b0 = (float)((double)mode->gain * sc.s);
        coeff->a1 = (float)(-2.0 * r * sc.c);
        coeff->a2 = (float)(r * r);
    }
    config->derived = true;
}

uint32_t audiodsp_modal_state_floats(const audiodsp_modal_config_t *config) {
    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    return config->mode_count * channels;
}

void audiodsp_modal_state_init(audiodsp_modal_state_t *state,
    const audiodsp_modal_config_t *config, float *s1, float *s2,
    uint32_t words) {
    state->s1 = s1;
    state->s2 = s2;
    state->words = words;
    (void)config;
    audiodsp_modal_reset(state);
}

void audiodsp_modal_reset(audiodsp_modal_state_t *state) {
    if (state->s1 == NULL || state->s2 == NULL) {
        return;
    }
    for (uint32_t i = 0; i < state->words; ++i) {
        state->s1[i] = 0.0f;
        state->s2[i] = 0.0f;
    }
}

bool audiodsp_modal_silent(const audiodsp_modal_state_t *state) {
    if (state->s1 == NULL || state->s2 == NULL) {
        return true;
    }
    for (uint32_t i = 0; i < state->words; ++i) {
        if (state->s1[i] != 0.0f || state->s2[i] != 0.0f) {
            return false;
        }
    }
    return true;
}

void audiodsp_modal_process_s16(const audiodsp_modal_config_t *config,
    audiodsp_modal_state_t *state, int16_t *out, const int16_t *in,
    uint32_t frames) {
    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    const uint32_t modes = config->mode_count;
    if (state->s1 == NULL || state->s2 == NULL ||
        state->words < modes * channels || config->coeffs == NULL) {
        if (in == NULL) {
            memset(out, 0, (size_t)frames * channels * sizeof(int16_t));
        } else if (out != in && frames > 0u) {
            memcpy(out, in, (size_t)frames * channels * sizeof(int16_t));
        }
        return;
    }
    const float mix = config->mix;
    const float dry = 1.0f - mix;
    const float gain = config->gain * mix;

    // --- narrow the loop to the modes that can do anything ----------------
    //
    // audiodsp#82. The skip inside the inner loop is right but late: a mode
    // at rest still costs an index, a coefficient pointer and a compare,
    // every sample. At 64 modes that residual is most of what a RESIDENT
    // bank pays -- acoustickit holds 73 modes across two banks and spent
    // 2.55 ms a block on a P4 with nothing struck at all.
    //
    // The audio is bit-identical: a mode outside the bounds below satisfies
    // the existing skip on EVERY sample of this block, so dropping it
    // changes nothing. Measured that way, digest for digest, on six shapes.
    //
    // Two things make a mode do something -- it is already ringing, or it is
    // driven (b0 != 0) AND the input is not silent -- so the range is the
    // union. The tests are ordered by what each costs and how often it
    // settles the question, because this runs per block on the audio thread
    // and a narrowing that does not narrow must be nearly free:
    //
    //   1. the ringing range, O(modes x channels). If it already spans the
    //      bank there is nothing to win and we stop here -- which is the
    //      all-ringing case, where every later test would be pure overhead.
    //   2. the driven range, O(modes) and no memory beyond the coefficients.
    //      If nothing is driven, the input does not matter.
    //   3. only then, whether the input is silent, O(frames x channels).
    //      This is the biggest of the three and the one that is skipped
    //      whenever either of the others has already answered.
    uint32_t lo = modes;
    uint32_t hi = 0;
    for (uint32_t m = 0; m < modes; ++m) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const uint32_t w = m * channels + channel;
            if (state->s1[w] != 0.0f || state->s2[w] != 0.0f) {
                if (m < lo) {
                    lo = m;
                }
                hi = m + 1u;
                break;
            }
        }
    }
    if (lo != 0u || hi != modes) {
        uint32_t driven_lo = modes;
        uint32_t driven_hi = 0;
        for (uint32_t m = 0; m < modes; ++m) {
            if (config->coeffs[m].b0 != 0.0f) {
                if (m < driven_lo) {
                    driven_lo = m;
                }
                driven_hi = m + 1u;
            }
        }
        if (driven_lo < driven_hi
            && (driven_lo < lo || driven_hi > hi)) {
            bool excited = false;
            for (uint32_t i = 0, n = in == NULL ? 0u : frames * channels;
                 i < n; ++i) {
                if (in[i] != 0) {
                    excited = true;
                    break;
                }
            }
            if (excited) {
                if (driven_lo < lo) {
                    lo = driven_lo;
                }
                if (driven_hi > hi) {
                    hi = driven_hi;
                }
            }
        }
    }
    if (lo >= hi) {
        // Nothing can sound, so the dry path is the whole of the work.
        if (in == NULL) {
            memset(out, 0, (size_t)frames * channels * sizeof(int16_t));
            return;
        }
        for (uint32_t i = 0, n = frames * channels; i < n; ++i) {
            out[i] = to_s16(dry * (float)in[i]);
        }
        return;
    }

    // --- one mode at a time across a stretch of frames --------------------
    //
    // Mode outermost, frames inside it, so a mode's state and coefficients
    // sit in registers for the whole stretch, two modes per pass so an
    // in-order core has the second one's arithmetic to issue while the first
    // one's recursion waits on its own result.
    //
    // The audio is bit-identical to running each mode a sample at a time,
    // skipping a sample whenever the mode is taking nothing in (b0 or the
    // input zero) AND holding nothing (both state words zero): a muted mode
    // that is still ringing keeps advancing, or muting would be a cut. That
    // is a property of the arithmetic, not a tolerance:
    //
    // - `acc[j]` starts at +0.0f and takes each mode's y0 in ascending mode
    //   order, the same float additions the per-sample sum made. It can never
    //   be -0.0f (only -0 + -0 makes -0), so adding a zero leaves it as it was.
    // - The skip is only tested per stretch here: a mode holding nothing at
    //   the start of a stretch, with no way in (b0 zero or the stretch
    //   silent), holds nothing all through it, so it is left out. Every other
    //   mode runs every sample. On a sample the per-sample test would have
    //   skipped, the state is zero and so is the input, so y0 and the new
    //   state come out zero, the flush makes the state +0.0f exactly, and y0
    //   adds nothing. Only the sign of a zero state word can differ, and a
    //   signed zero never reaches a non-zero result: it is added to a
    //   non-zero number, which comes out exact, or multiplied into a zero.
    // - With nothing coming in, b0 * x0 is a zero, so y0 = b0 * x0 + s1 is s1
    //   exactly whenever s1 is not zero, and a zero when it is. The silent
    //   stretch, every sustained note between strikes, skips that multiply
    //   and add, which are half the recursion's latency.
    // - The flush is the per-sample loop's, tested s2 first (flush_pair()).
    float xs[AUDIODSP_MODAL_STRETCH];
    float acc[AUDIODSP_MODAL_STRETCH];
    uint8_t act[AUDIODSP_MODAL_MAX_MODES];
    for (uint32_t channel = 0; channel < channels; ++channel) {
        for (uint32_t start = 0; start < frames;
             start += AUDIODSP_MODAL_STRETCH) {
            const uint32_t n = frames - start < AUDIODSP_MODAL_STRETCH
                ? frames - start : AUDIODSP_MODAL_STRETCH;
            bool silent = true;
            if (in != NULL) {
                const int16_t *src = in + (size_t)start * channels + channel;
                int32_t any = 0;
                for (uint32_t j = 0; j < n; ++j) {
                    const int16_t v = src[(size_t)j * channels];
                    any |= v;
                    xs[j] = (float)v;
                }
                silent = any == 0;
            }
            for (uint32_t j = 0; j < n; ++j) {
                acc[j] = 0.0f;
            }
            uint32_t count = 0;
            for (uint32_t m = lo; m < hi; ++m) {
                const uint32_t w = m * channels + channel;
                if ((silent || config->coeffs[m].b0 == 0.0f)
                    && state->s1[w] == 0.0f && state->s2[w] == 0.0f) {
                    continue;
                }
                act[count++] = (uint8_t)m;
            }
            uint32_t k = 0;
            if (silent) {
                for (; k + 3u < count; k += 4u) {
                    ring4(config, state, channels, channel, &act[k], acc, n);
                }
            }
            for (; k + 1u < count; k += 2u) {
                const audiodsp_modal_coeff_t ca = config->coeffs[act[k]];
                const audiodsp_modal_coeff_t cb = config->coeffs[act[k + 1u]];
                const uint32_t wa = act[k] * channels + channel;
                const uint32_t wb = act[k + 1u] * channels + channel;
                const float na1a = -ca.a1;
                const float na2a = -ca.a2;
                const float na1b = -cb.a1;
                const float na2b = -cb.a2;
                const float b0a = ca.b0;
                const float b0b = cb.b0;
                float s1a = state->s1[wa];
                float s2a = state->s2[wa];
                float s1b = state->s1[wb];
                float s2b = state->s2[wb];
                if (silent) {
                    for (uint32_t j = 0; j < n; ++j) {
                        const float ya = s1a;
                        const float yb = s1b;
                        s1a = na1a * ya + s2a;
                        s2a = na2a * ya;
                        s1b = na1b * yb + s2b;
                        s2b = na2b * yb;
                        float sum = acc[j];
                        sum += ya;
                        sum += yb;
                        acc[j] = sum;
                        if (MODAL_UNLIKELY(maybe_tiny(s2a) | maybe_tiny(s2b))) {
                            flush_pair(&s1a, &s2a);
                            flush_pair(&s1b, &s2b);
                        }
                    }
                } else {
                    for (uint32_t j = 0; j < n; ++j) {
                        const float ya = b0a * xs[j] + s1a;
                        const float yb = b0b * xs[j] + s1b;
                        s1a = na1a * ya + s2a;
                        s2a = na2a * ya;
                        s1b = na1b * yb + s2b;
                        s2b = na2b * yb;
                        float sum = acc[j];
                        sum += ya;
                        sum += yb;
                        acc[j] = sum;
                        if (MODAL_UNLIKELY(maybe_tiny(s2a) | maybe_tiny(s2b))) {
                            flush_pair(&s1a, &s2a);
                            flush_pair(&s1b, &s2b);
                        }
                    }
                }
                state->s1[wa] = s1a;
                state->s2[wa] = s2a;
                state->s1[wb] = s1b;
                state->s2[wb] = s2b;
            }
            if (k < count) {
                const audiodsp_modal_coeff_t c = config->coeffs[act[k]];
                const uint32_t w = act[k] * channels + channel;
                const float b0 = c.b0;
                const float na1 = -c.a1;
                const float na2 = -c.a2;
                float s1 = state->s1[w];
                float s2 = state->s2[w];
                for (uint32_t j = 0; j < n; ++j) {
                    const float y0 = silent ? s1 : b0 * xs[j] + s1;
                    s1 = na1 * y0 + s2;
                    s2 = na2 * y0;
                    acc[j] += y0;
                    if (MODAL_UNLIKELY(maybe_tiny(s2))) {
                        flush_pair(&s1, &s2);
                    }
                }
                state->s1[w] = s1;
                state->s2[w] = s2;
            }
            // With mix at 1 (STRUCK's) dry is +0.0f, and with a silent
            // stretch xs is zero: either way dry * xs is a zero, and adding
            // it to gain * acc leaves that as it was or, for a zero, changes
            // only its sign, which to_s16() rounds to 0 regardless.
            int16_t *dst = out + (size_t)start * channels + channel;
            if (silent || dry == 0.0f) {
                // Four at a time, so the core has four conversions to
                // overlap: when all four are in range (a NaN is not), each
                // rounds as to_s16() does, and otherwise to_s16() does it.
                uint32_t j = 0;
                for (; j + 4u <= n; j += 4u) {
                    const float v0 = gain * acc[j];
                    const float v1 = gain * acc[j + 1u];
                    const float v2 = gain * acc[j + 2u];
                    const float v3 = gain * acc[j + 3u];
                    int16_t *d = dst + (size_t)j * channels;
                    if (MODAL_LIKELY(in_s16(v0) & in_s16(v1)
                                     & in_s16(v2) & in_s16(v3))) {
                        d[0] = round_s16(v0);
                        d[channels] = round_s16(v1);
                        d[2u * channels] = round_s16(v2);
                        d[3u * channels] = round_s16(v3);
                    } else {
                        d[0] = to_s16(v0);
                        d[channels] = to_s16(v1);
                        d[2u * channels] = to_s16(v2);
                        d[3u * channels] = to_s16(v3);
                    }
                }
                for (; j < n; ++j) {
                    dst[(size_t)j * channels] = to_s16(gain * acc[j]);
                }
            } else {
                for (uint32_t j = 0; j < n; ++j) {
                    dst[(size_t)j * channels] = to_s16(dry * xs[j] + gain * acc[j]);
                }
            }
        }
    }
}
