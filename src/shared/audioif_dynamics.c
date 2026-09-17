// Runtime-neutral dynamics DSP. See audioif_dynamics.h for provenance.
// SPDX-License-Identifier: MIT

// No fused multiply-add here. audioif#66 proved contraction was the whole
// of the P4-vs-S3 split on this file's transient attack path and turned it
// off inline; audioif#79 generalised it to the rest of the kernel, so the
// mechanism, the two spellings that do not work and the measurements now
// live in one place. Same pragma, same codegen -- checked, not assumed.
#include "shared/audioif_fp_contract.h"

#include "shared/audioif_dynamics.h"

#include <math.h>
#include <string.h>

float audioif_dynamics_ms_to_coef(float ms, float sample_rate) {
    if (ms <= 0.0f) {
        return 1.0f;
    }
    return 1.0f - expf(-1000.0f / (ms * sample_rate));
}

float audioif_dynamics_db_to_gain(float db) {
    return expf(db * 0.115129254649702f);
}

float audioif_dynamics_gain_to_db(float gain) {
    return logf(gain < 1e-6f ? 1e-6f : gain) * 8.68588963806504f;
}

void audioif_dynamics_config_init(audioif_dynamics_config_t *config, int mode,
    uint32_t sample_rate) {
    config->sample_rate = sample_rate;
    config->channel_count = 2;
    config->mode = mode;
    config->threshold_db = -24.0f;
    config->ratio = 4.0f;
    config->knee_db = 6.0f;
    config->makeup_gain = 1.0f;
    config->attack_coef = 0.0f;
    config->release_coef = 0.0f;
    config->attack_gain_db = 0.0f;
    config->sustain_gain_db = 0.0f;
    config->sidechain_coef = 0.0f;
    config->lookahead_frames = 0;
    config->true_peak = AUDIOIF_DYNAMICS_TP_OFF;
    // The transient shaper's four time constants were literals evaluated at
    // process entry. They are fields now, and these are those literals, so a
    // node that sets none of them computes what it always computed.
    config->transient_fast_attack_ms = 1.0f;
    config->transient_fast_release_ms = 50.0f;
    config->transient_slow_attack_ms = 25.0f;
    config->transient_slow_release_ms = 300.0f;
    config->transient_dual = false;
    // The second pair is deliberately slower than the first: the sustain
    // section is meant to read the note's body, not its edge.
    config->sustain_fast_attack_ms = 1.0f;
    config->sustain_fast_release_ms = 200.0f;
    config->sustain_slow_attack_ms = 25.0f;
    config->sustain_slow_release_ms = 1200.0f;
    config->slow_hold_frames = 0;
    config->detector = AUDIOIF_DYNAMICS_DETECT_PEAK;
    config->rms_coef = 0.0f;
    config->gain_smooth_coef = 1.0f;   // off
    config->feedback_detector = false;
    config->feedback_gain_corrected = false;
    config->relative_threshold = false;
    config->program_attack = false;
    config->sidechain_lp_coef = 0.0f;
    config->sidechain_poles = 1u;
    config->key_listen = false;
    config->depth_db = 1.0f;
    config->hold_frames = 0;
    config->hysteresis_db = 0.0f;
}

void audioif_dynamics_config_finish(audioif_dynamics_config_t *config) {
    if (config->attack_coef == 0.0f) {
        config->attack_coef = audioif_dynamics_ms_to_coef(10.0f,
            (float)config->sample_rate);
    }
    if (config->release_coef == 0.0f) {
        config->release_coef = audioif_dynamics_ms_to_coef(120.0f,
            (float)config->sample_rate);
    }
    // Same shape for the RMS window: unset means 10 ms, which is short enough
    // to follow a syllable and long enough to average a low note's cycle.
    if (config->rms_coef == 0.0f) {
        config->rms_coef = audioif_dynamics_ms_to_coef(10.0f,
            (float)config->sample_rate);
    }
}

void audioif_dynamics_configure(audioif_dynamics_config_t *config,
    audioif_dynamics_option_t option, float value) {
    switch (option) {
        case AUDIOIF_DYNAMICS_OPT_THRESHOLD_DB:
            config->threshold_db = value;
            break;
        case AUDIOIF_DYNAMICS_OPT_RATIO:
            config->ratio = value < 1.0f ? 1.0f : value;
            break;
        case AUDIOIF_DYNAMICS_OPT_KNEE_DB:
            config->knee_db = value < 0.0f ? 0.0f : value;
            break;
        case AUDIOIF_DYNAMICS_OPT_MAKEUP_DB:
            config->makeup_gain = audioif_dynamics_db_to_gain(value);
            break;
        case AUDIOIF_DYNAMICS_OPT_ATTACK_MS:
            config->attack_coef = audioif_dynamics_ms_to_coef(value,
                (float)config->sample_rate);
            break;
        case AUDIOIF_DYNAMICS_OPT_RELEASE_MS:
            config->release_coef = audioif_dynamics_ms_to_coef(value,
                (float)config->sample_rate);
            break;
        case AUDIOIF_DYNAMICS_OPT_ATTACK_GAIN_DB:
            config->attack_gain_db = value;
            break;
        case AUDIOIF_DYNAMICS_OPT_SUSTAIN_GAIN_DB:
            config->sustain_gain_db = value;
            break;
        case AUDIOIF_DYNAMICS_OPT_SIDECHAIN_HZ:
            config->sidechain_coef = value <= 0.0f ? 0.0f
                : 1.0f - expf(-6.283185307f * value /
                              (float)config->sample_rate);
            break;
        case AUDIOIF_DYNAMICS_OPT_LOOKAHEAD_MS: {
            float ms = value;
            if (ms < 0.0f) {
                ms = 0.0f;
            } else if (ms > AUDIOIF_DYNAMICS_MAX_LOOKAHEAD_MS) {
                ms = AUDIOIF_DYNAMICS_MAX_LOOKAHEAD_MS;
            }
            config->lookahead_frames =
                (uint32_t)(ms * (float)config->sample_rate / 1000.0f);
            break;
        }
        case AUDIOIF_DYNAMICS_OPT_TRUE_PEAK:
            // Was a flag; it is a level now. Anything at or above 2 selects
            // the 4x reconstruction, so `true_peak=True` still means exactly
            // the half-band estimate it always did.
            config->true_peak = value <= 0.0f ? AUDIOIF_DYNAMICS_TP_OFF
                : (value < 2.0f ? AUDIOIF_DYNAMICS_TP_HALF_BAND
                                : AUDIOIF_DYNAMICS_TP_OVERSAMPLED);
            break;
        case AUDIOIF_DYNAMICS_OPT_TRANSIENT_FAST_ATTACK_MS:
            config->transient_fast_attack_ms = value;
            break;
        case AUDIOIF_DYNAMICS_OPT_TRANSIENT_FAST_RELEASE_MS:
            config->transient_fast_release_ms = value;
            break;
        case AUDIOIF_DYNAMICS_OPT_TRANSIENT_SLOW_ATTACK_MS:
            config->transient_slow_attack_ms = value;
            break;
        case AUDIOIF_DYNAMICS_OPT_TRANSIENT_SLOW_RELEASE_MS:
            config->transient_slow_release_ms = value;
            break;
        case AUDIOIF_DYNAMICS_OPT_DETECTOR:
            config->detector = value >= 1.0f ? AUDIOIF_DYNAMICS_DETECT_RMS
                                             : AUDIOIF_DYNAMICS_DETECT_PEAK;
            break;
        case AUDIOIF_DYNAMICS_OPT_RMS_MS:
            config->rms_coef = audioif_dynamics_ms_to_coef(value,
                (float)config->sample_rate);
            break;
        case AUDIOIF_DYNAMICS_OPT_GAIN_SMOOTH_MS:
            // The house ms-to-coefficient helper, which already returns
            // 1.0 for a non-positive time -- and 1.0 is off here, because the
            // one-pole then passes the computed gain through unchanged.
            config->gain_smooth_coef = audioif_dynamics_ms_to_coef(value,
                (float)config->sample_rate);
            break;
        case AUDIOIF_DYNAMICS_OPT_FEEDBACK_DETECTOR:
            config->feedback_detector = value != 0.0f;
            break;
        case AUDIOIF_DYNAMICS_OPT_FEEDBACK_GAIN_CORRECTED:
            config->feedback_gain_corrected = value != 0.0f;
            break;
        case AUDIOIF_DYNAMICS_OPT_SIDECHAIN_LP_HZ:
            config->sidechain_lp_coef = value <= 0.0f ? 0.0f
                : 1.0f - expf(-6.283185307f * value /
                              (float)config->sample_rate);
            break;
        case AUDIOIF_DYNAMICS_OPT_SIDECHAIN_POLES:
            config->sidechain_poles = value >= 2.0f ? 2u : 1u;
            break;
        case AUDIOIF_DYNAMICS_OPT_KEY_LISTEN:
            config->key_listen = value != 0.0f;
            break;
        case AUDIOIF_DYNAMICS_OPT_DEPTH_DB:
            config->depth_db = value;
            break;
        case AUDIOIF_DYNAMICS_OPT_HOLD_MS:
            config->hold_frames = value <= 0.0f ? 0u
                : (uint32_t)(value * (float)config->sample_rate / 1000.0f);
            break;
        case AUDIOIF_DYNAMICS_OPT_HYSTERESIS_DB:
            // A hysteresis is a width, so its sign carries no information.
            config->hysteresis_db = value < 0.0f ? -value : value;
            break;
        case AUDIOIF_DYNAMICS_OPT_RELATIVE_THRESHOLD:
            config->relative_threshold = value != 0.0f;
            break;
        case AUDIOIF_DYNAMICS_OPT_PROGRAM_ATTACK:
            config->program_attack = value != 0.0f;
            break;
        case AUDIOIF_DYNAMICS_OPT_TRANSIENT_DUAL:
            config->transient_dual = value != 0.0f;
            break;
        case AUDIOIF_DYNAMICS_OPT_SUSTAIN_FAST_ATTACK_MS:
            config->sustain_fast_attack_ms = value;
            break;
        case AUDIOIF_DYNAMICS_OPT_SUSTAIN_FAST_RELEASE_MS:
            config->sustain_fast_release_ms = value;
            break;
        case AUDIOIF_DYNAMICS_OPT_SUSTAIN_SLOW_ATTACK_MS:
            config->sustain_slow_attack_ms = value;
            break;
        case AUDIOIF_DYNAMICS_OPT_SUSTAIN_SLOW_RELEASE_MS:
            config->sustain_slow_release_ms = value;
            break;
        case AUDIOIF_DYNAMICS_OPT_SLOW_HOLD_MS:
            config->slow_hold_frames = value <= 0.0f ? 0u
                : (uint32_t)(value * (float)config->sample_rate / 1000.0f);
            break;
        case AUDIOIF_DYNAMICS_OPT_COUNT:
        default:
            break;
    }
}

uint32_t audioif_dynamics_lookahead_frames(
    const audioif_dynamics_config_t *config) {
    return config->lookahead_frames;
}

void audioif_dynamics_set_lookahead(audioif_dynamics_state_t *state,
    int16_t *buffer, uint32_t frames) {
    state->lookahead = buffer;
    state->lookahead_capacity = frames;
    state->lookahead_write = 0;
    if (buffer != NULL && frames != 0) {
        memset(buffer, 0, (size_t)frames * state->channel_count *
            sizeof(int16_t));
    }
}

// Everything the detector remembers, and nothing that is configuration.
// `state_init` and `reset` both go through here so the two cannot drift: a
// reset that left behind something a fresh build clears is a reset that does
// not mean what it says, and that is exactly what audioif#56 was -- measured,
// a fresh node under a quiet tone gated it to 0 LSB and the same node after a
// loud pass and a reset passed it at 32.
void audioif_dynamics_clear_detector(audioif_dynamics_state_t *state) {
    // The side-chain filter memory. It used to survive a reset, on the
    // grounds that the original engine keeps its filter memory across one --
    // but the original's reset was never held to returning the node to its
    // built state, and a gate that opens on the difference between silence
    // and a stale high-pass is not carrying fidelity, it is carrying the
    // previous take.
    state->sidechain_lp[0] = 0.0f;
    state->sidechain_lp[1] = 0.0f;
    state->sidechain_hp2[0] = 0.0f;
    state->sidechain_hp2[1] = 0.0f;
    state->sidechain_low[0] = 0.0f;
    state->sidechain_low[1] = 0.0f;
    state->sidechain_low2[0] = 0.0f;
    state->sidechain_low2[1] = 0.0f;
    state->envelope = 0.0f;
    state->fast_env = 0.0f;
    state->slow_env = 0.0f;
    // A fresh node reports no gain reduction; so does a reset one now.
    state->gain_reduction_db = 0.0f;
    // The gain smoother's memory, and the flag that says it has one. Added
    // after this helper was, and it belongs here for the helper's whole
    // reason: whatever a fresh build clears, a reset clears.
    state->smoothed_gain = 0.0f;
    state->gain_smooth_primed = false;
    memset(state->peak_history, 0, sizeof(state->peak_history));
    audioif_dynamics_clear_extras(state);
}

void audioif_dynamics_state_init(audioif_dynamics_state_t *state) {
    state->channel_count = 2;
    state->lookahead = NULL;
    state->lookahead_capacity = 0;
    state->lookahead_write = 0;
    audioif_dynamics_clear_detector(state);
}

// Everything the additions remember. The side-chain filters are not in here
// because they are not an addition -- `sidechain_lp` is the original's own,
// and all four are cleared by audioif_dynamics_clear_detector above, which is
// the single door both state_init and reset go through.
void audioif_dynamics_clear_extras(audioif_dynamics_state_t *state) {
    state->rms_env[0] = 0.0f;
    state->rms_env[1] = 0.0f;
    state->prev_output[0] = 0.0f;
    state->prev_output[1] = 0.0f;
    state->full_env = 0.0f;
    state->fast_env2 = 0.0f;
    state->slow_env2 = 0.0f;
    state->slow_peak = 0.0f;
    state->slow_hold_left = 0;
    state->gate_stage = AUDIOIF_DYNAMICS_GATE_CLOSED;
    state->gate_gain = 0.0f;
    state->hold_left = 0;
    memset(state->tp_history, 0, sizeof(state->tp_history));
}

void audioif_dynamics_set_channel_count(
    audioif_dynamics_config_t *config, audioif_dynamics_state_t *state,
    uint32_t channel_count) {
    if (channel_count != 1u) {
        channel_count = 2u;
    }
    config->channel_count = channel_count;
    state->channel_count = channel_count;
    state->lookahead_write = 0;
}

void audioif_dynamics_reset(audioif_dynamics_state_t *state) {
    // A reset means "as if this node had just been built", so it clears
    // exactly what a fresh build clears and keeps only the configuration --
    // the channel count and the lookahead storage the bindings own. The
    // lookahead *contents* go: that is audio in flight, and a chain
    // restarted with the previous take still queued would play it.
    audioif_dynamics_clear_detector(state);
    if (state->lookahead != NULL && state->lookahead_capacity != 0) {
        memset(state->lookahead, 0,
            (size_t)state->lookahead_capacity * state->channel_count *
            sizeof(int16_t));
        state->lookahead_write = 0;
    }
}

// The peak between two samples, which is where a limiter's overshoot lives:
// a signal can pass through 0 dBFS between one sample and the next without
// any sample being over. Four-point half-band interpolation of the midpoint,
// which is the cheapest estimate worth having -- proper true-peak metering
// oversamples by four and this does not pretend to be that.
static float half_sample_peak(const float history[3], float current) {
    const float mid = -0.0625f * history[0] + 0.5625f * history[1] +
        0.5625f * history[2] - 0.0625f * current;
    return mid < 0.0f ? -mid : mid;
}

// Four phases of twelve taps: an order-48 polyphase FIR that reconstructs the
// detector signal at 4x, which is the shape ITU-R BS.1770 Annex 2 specifies
// for true-peak metering. The taps are NOT the Recommendation's table -- they
// are a Blackman-windowed sinc, generated once on CPython and written out
// here as literals so every target reads the same numbers rather than
// recomputing them through a platform libm. Each phase is normalised to unit
// DC gain. Measured against worst-phase full-scale tones, this reads between
// -0.26 dB and 0.00 dB of the true peak across the band, where the half-band
// estimate above under-reads by 1.24 dB at f_s/4.
static const float dynamics_tp_taps[AUDIOIF_DYNAMICS_TP_PHASES]
                                   [AUDIOIF_DYNAMICS_TP_TAPS] = {
    {
        2.87730009e-19f, 0.000705750655f, -0.00429586426f,
        0.0151204466f, -0.0425751007f, 0.127196428f,
        0.972673602f, -0.0932201934f, 0.0331783852f,
        -0.0113643234f, 0.00294428057f, -0.000363411954f,
    },
    {
        -8.46131255e-05f, 0.00293353543f, -0.0146775584f,
        0.0478926834f, -0.131991576f, 0.449310957f,
        0.771207998f, -0.170813997f, 0.0621681154f,
        -0.020270794f, 0.00468613765f, -0.000360888677f,
    },
    {
        -0.000360888677f, 0.00468613765f, -0.020270794f,
        0.0621681154f, -0.170813997f, 0.771207998f,
        0.449310957f, -0.131991576f, 0.0478926834f,
        -0.0146775584f, 0.00293353543f, -8.46131255e-05f,
    },
    {
        -0.000363411954f, 0.00294428057f, -0.0113643234f,
        0.0331783852f, -0.0932201934f, 0.972673602f,
        0.127196428f, -0.0425751007f, 0.0151204466f,
        -0.00429586426f, 0.000705750655f, 2.87730009e-19f,
    },
};

// The largest magnitude the four reconstructed points between the last two
// detector samples reach. `history[0]` is the oldest of the twelve.
static float oversampled_peak(const float *history) {
    float peak = 0.0f;
    for (uint32_t phase = 0; phase < AUDIOIF_DYNAMICS_TP_PHASES; ++phase) {
        float sum = 0.0f;
        for (uint32_t tap = 0; tap < AUDIOIF_DYNAMICS_TP_TAPS; ++tap) {
            sum += dynamics_tp_taps[phase][tap] * history[tap];
        }
        const float magnitude = sum < 0.0f ? -sum : sum;
        if (magnitude > peak) {
            peak = magnitude;
        }
    }
    return peak;
}

// The gain computer: how many dB to apply for a detector reading of `env_db`.
static float dynamics_gain_db(const audioif_dynamics_config_t *config,
    float env_db) {
    const float over = env_db - config->threshold_db;
    switch (config->mode) {
        case AUDIOIF_DYNAMICS_LIMIT:
            return over > 0.0f ? -over : 0.0f;
        case AUDIOIF_DYNAMICS_EXPAND: {
            if (over >= 0.0f) {
                return 0.0f;
            }
            float cut = over * (config->ratio - 1.0f);
            // `depth_db` positive means unset, and unset is the original's
            // fixed literal.
            const float floor_db = config->depth_db > 0.0f ? -60.0f
                                                           : config->depth_db;
            return cut < floor_db ? floor_db : cut;
        }
        case AUDIOIF_DYNAMICS_GATE: {
            if (over >= 0.0f) {
                return 0.0f;
            }
            float cut = over * 8.0f;
            const float floor_db = config->depth_db > 0.0f ? -80.0f
                                                           : config->depth_db;
            return cut < floor_db ? floor_db : cut;
        }
        case AUDIOIF_DYNAMICS_COMPRESS:
        default: {
            const float half_knee = config->knee_db * 0.5f;
            // Which slope the detector's reading needs depends on what the
            // detector is reading. Feed-forward reads the input, so a ratio R
            // wants (1 - 1/R). A gain-corrected feedback detector reads the
            // OUTPUT, and the slope that makes the loop settle on that same
            // static curve is (R - 1):
            //
            //     g = -(R-1)(y - T)  and  y = x + g
            //  => g(1 + R - 1) = -(R-1)(x - T)
            //  => g = -(1 - 1/R)(x - T)
            //
            // which is the feed-forward law, arrived at with the detector
            // still following the output -- so the loop keeps its own
            // dynamics and stops settling at 2:1 whatever the knob says
            // (audioif#62).
            //
            // This is the inversion, and it is deliberately not the other
            // thing that reaches the same steady state: dividing the fed-back
            // sample by the gain that produced it recovers the input exactly
            // and so cancels the loop, which measured feed-forward to within
            // 0.026 dB over a whole step response. That would have been an
            // option nobody has a reason to switch on.
            const float slope = (config->feedback_detector &&
                                 config->feedback_gain_corrected)
                ? config->ratio - 1.0f
                : 1.0f - 1.0f / config->ratio;
            if (over <= -half_knee) {
                return 0.0f;
            }
            if (over < half_knee && config->knee_db > 0.0f) {
                const float x = over + half_knee;
                return -slope * x * x / (2.0f * config->knee_db);
            }
            return -slope * over;
        }
    }
}

void audioif_dynamics_process_s16(const audioif_dynamics_config_t *config,
    audioif_dynamics_state_t *state, int16_t *output, const int16_t *input,
    uint32_t frames) {
    audioif_dynamics_process_s16_key(config, state, output, input, NULL,
        frames);
}

void audioif_dynamics_process_s16_key(const audioif_dynamics_config_t *config,
    audioif_dynamics_state_t *state, int16_t *output, const int16_t *input,
    const int16_t *key, uint32_t frames) {
    // The transient detectors' time constants. They were compiled-in literals
    // evaluated here on every call; they are config fields now, defaulted to
    // those same literals, so a node that leaves them alone is unchanged.
    const float fast_att = audioif_dynamics_ms_to_coef(
        config->transient_fast_attack_ms, (float)config->sample_rate);
    const float fast_rel = audioif_dynamics_ms_to_coef(
        config->transient_fast_release_ms, (float)config->sample_rate);
    const float slow_att = audioif_dynamics_ms_to_coef(
        config->transient_slow_attack_ms, (float)config->sample_rate);
    const float slow_rel = audioif_dynamics_ms_to_coef(
        config->transient_slow_release_ms, (float)config->sample_rate);
    // The second pair, which only `transient_dual` reads.
    const float sus_fast_att = audioif_dynamics_ms_to_coef(
        config->sustain_fast_attack_ms, (float)config->sample_rate);
    const float sus_fast_rel = audioif_dynamics_ms_to_coef(
        config->sustain_fast_release_ms, (float)config->sample_rate);
    const float sus_slow_att = audioif_dynamics_ms_to_coef(
        config->sustain_slow_attack_ms, (float)config->sample_rate);
    const float sus_slow_rel = audioif_dynamics_ms_to_coef(
        config->sustain_slow_release_ms, (float)config->sample_rate);

    // Lookahead only reaches as far as the buffer the binding handed over,
    // so a binding that allocated nothing gets no lookahead rather than
    // reading off the end of one.
    uint32_t held = config->lookahead_frames;
    if (held > state->lookahead_capacity) {
        held = state->lookahead_capacity;
    }
    if (held != 0 && state->lookahead_write >= held) {
        state->lookahead_write = 0;
    }

    // The gate's four-stage envelope, and the linear levels it compares
    // against. Precomputing them in dB->gain here is what lets the per-sample
    // path skip the logf the memoryless computer needs.
    const bool gate_machine = config->hold_frames != 0u &&
        config->mode == AUDIOIF_DYNAMICS_GATE;
    const float open_level = gate_machine
        ? audioif_dynamics_db_to_gain(config->threshold_db) : 0.0f;
    const float close_level = gate_machine
        ? audioif_dynamics_db_to_gain(config->threshold_db -
                                      config->hysteresis_db) : 0.0f;
    const float floor_gain = gate_machine
        ? audioif_dynamics_db_to_gain(config->depth_db > 0.0f
                                      ? -80.0f : config->depth_db)
        : 0.0f;
    // Program-dependent attack measures the overshoot against the threshold,
    // which is the "how far over" a compressor's attack is meant to depend
    // on. Against the envelope instead it would be enormous out of silence
    // and every attack would collapse to one sample.
    const float threshold_level = config->program_attack
        ? audioif_dynamics_db_to_gain(config->threshold_db) : 0.0f;

    const uint32_t channels = config->channel_count == 1u ? 1u : 2u;
    while (frames-- != 0) {
        float source[2] = {0.0f, 0.0f};
        for (uint32_t channel = 0; channel < channels; ++channel) {
            source[channel] = (float)input[channel] / 32768.0f;
        }
        // The detector reads the signal as it arrives; the audio it applies
        // its gain to is what came in `held` frames ago. That is the whole
        // trick, and the whole cost: the chain is now that much later.
        float delayed[2] = {source[0], source[channels == 1u ? 0u : 1u]};
        if (held != 0) {
            for (uint32_t channel = 0; channel < channels; ++channel) {
                delayed[channel] = (float)state->lookahead[
                    state->lookahead_write * channels + channel] / 32768.0f;
                state->lookahead[state->lookahead_write * channels + channel] =
                    input[channel];
            }
            state->lookahead_write = (state->lookahead_write + 1u) % held;
        }
        // What the detector listens to. Three choices, and the first is the
        // original: this frame's input, an external key, or the frame this
        // node last put out -- the feedback topology, where the side chain is
        // tapped after the gain cell rather than before it.
        float sense[2] = {source[0], source[channels == 1u ? 0u : 1u]};
        if (key != NULL) {
            for (uint32_t channel = 0; channel < channels; ++channel) {
                sense[channel] = (float)key[channel] / 32768.0f;
            }
            if (channels == 1u) {
                sense[1] = sense[0];
            }
            key += channels;
        }
        if (config->feedback_detector) {
            sense[0] = state->prev_output[0];
            sense[1] = state->prev_output[channels == 1u ? 0u : 1u];
        }
        float detector[2] = {sense[0], sense[channels == 1u ? 0u : 1u]};
        if (config->sidechain_coef > 0.0f) {
            for (uint32_t channel = 0; channel < channels; ++channel) {
                state->sidechain_lp[channel] += config->sidechain_coef *
                    (sense[channel] - state->sidechain_lp[channel]);
                detector[channel] = sense[channel] -
                    state->sidechain_lp[channel];
            }
            if (config->sidechain_poles > 1u) {
                for (uint32_t channel = 0; channel < channels; ++channel) {
                    state->sidechain_hp2[channel] += config->sidechain_coef *
                        (detector[channel] - state->sidechain_hp2[channel]);
                    detector[channel] -= state->sidechain_hp2[channel];
                }
            }
        }
        // The low-cut above and this low-pass make the key path a band, which
        // is what an expander's or a gate's key filter is.
        if (config->sidechain_lp_coef > 0.0f) {
            for (uint32_t channel = 0; channel < channels; ++channel) {
                state->sidechain_low[channel] += config->sidechain_lp_coef *
                    (detector[channel] - state->sidechain_low[channel]);
                detector[channel] = state->sidechain_low[channel];
            }
            if (config->sidechain_poles > 1u) {
                for (uint32_t channel = 0; channel < channels; ++channel) {
                    state->sidechain_low2[channel] +=
                        config->sidechain_lp_coef *
                        (detector[channel] - state->sidechain_low2[channel]);
                    detector[channel] = state->sidechain_low2[channel];
                }
            }
        }
        float level = 0.0f;
        if (config->detector == AUDIOIF_DYNAMICS_DETECT_RMS) {
            // A one-pole mean square, rooted at the end: a sine and a square
            // of equal RMS land on the same number, which a rectified peak
            // does not.
            float mean_square = 0.0f;
            for (uint32_t channel = 0; channel < channels; ++channel) {
                const float squared = detector[channel] * detector[channel];
                state->rms_env[channel] += config->rms_coef *
                    (squared - state->rms_env[channel]);
                if (state->rms_env[channel] > mean_square) {
                    mean_square = state->rms_env[channel];
                }
            }
            level = sqrtf(mean_square);
        } else {
            for (uint32_t channel = 0; channel < channels; ++channel) {
                const float channel_level = fabsf(detector[channel]);
                if (channel_level > level) {
                    level = channel_level;
                }
            }
        }
        if (config->true_peak == AUDIOIF_DYNAMICS_TP_HALF_BAND) {
            for (uint32_t channel = 0; channel < channels; ++channel) {
                const float between = half_sample_peak(
                    state->peak_history[channel], detector[channel]);
                if (between > level) {
                    level = between;
                }
                state->peak_history[channel][0] =
                    state->peak_history[channel][1];
                state->peak_history[channel][1] =
                    state->peak_history[channel][2];
                state->peak_history[channel][2] = detector[channel];
            }
        } else if (config->true_peak == AUDIOIF_DYNAMICS_TP_OVERSAMPLED) {
            for (uint32_t channel = 0; channel < channels; ++channel) {
                for (uint32_t tap = 1; tap < AUDIOIF_DYNAMICS_TP_TAPS; ++tap) {
                    state->tp_history[channel][tap - 1] =
                        state->tp_history[channel][tap];
                }
                state->tp_history[channel][AUDIOIF_DYNAMICS_TP_TAPS - 1] =
                    detector[channel];
                const float between =
                    oversampled_peak(state->tp_history[channel]);
                if (between > level) {
                    level = between;
                }
            }
        }
        // The full-band level the relative-threshold gain computer subtracts,
        // read off the same signal the side chain filtered.
        float full_level = 0.0f;
        if (config->relative_threshold) {
            for (uint32_t channel = 0; channel < channels; ++channel) {
                const float channel_level = fabsf(sense[channel]);
                if (channel_level > full_level) {
                    full_level = channel_level;
                }
            }
        }
        float gain;
        if (config->mode == AUDIOIF_DYNAMICS_TRANSIENT) {
            state->fast_env += (level > state->fast_env ? fast_att : fast_rel)
                * (level - state->fast_env);
            // The peak-hold goes on whichever slow envelope drives the
            // sustain section: the only one there is, or the second pair's
            // when `transient_dual` gives the sustain its own. Holding the
            // first pair's while a second exists would cost the attack gain
            // for nothing -- a held slow envelope is never below the fast one.
            if (config->slow_hold_frames != 0u && !config->transient_dual) {
                // A peak-hold rather than a follower: the sustain difference
                // then grows monotonically through a note's decay instead of
                // peaking early and falling away with it.
                if (level >= state->slow_peak) {
                    state->slow_peak = level;
                    state->slow_hold_left = config->slow_hold_frames;
                } else if (state->slow_hold_left != 0u) {
                    state->slow_hold_left--;
                } else {
                    state->slow_peak += slow_rel * (level - state->slow_peak);
                }
                state->slow_env = state->slow_peak;
            } else {
                state->slow_env +=
                    (level > state->slow_env ? slow_att : slow_rel)
                    * (level - state->slow_env);
            }
            // `+1e-5f` on each envelope, and it is a *measured* optimum
            // rather than an arbitrary guard -- audioif#57 asks for it to be
            // smaller or replaced by a floor on the ratio, and both are worse.
            // The pedestal is added before the log, so the level only cancels
            // while both envelopes are well above it; but it also damps the
            // int16 quantisation noise that a bare ratio amplifies. Swept, as
            // the worst departure of one shape's gain trace from the same
            // shape 54 dB louder (the property a thresholdless design
            // promises), at -30 / -40 / -50 / -60 dBFS:
            //
            //   1e-3   0.3690  1.1117  4.4914  7.4214
            //   1e-4   0.0361  0.1185  0.3671  1.0456
            //   1e-5   0.0022  0.0066  0.0208  0.0448   <- this
            //   1e-6   0.0043  0.0133  0.0387  0.1000
            //   0      0.0045  0.0141  0.0412  0.1103   (exact ratio)
            //
            // Non-monotonic, with the minimum here. Larger pedestals distort,
            // which is what #57 saw; smaller ones and the exact ratio expose
            // quantisation. Do not "improve" this number without re-running
            // that sweep.
            const float diff =
                audioif_dynamics_gain_to_db(state->fast_env + 1e-5f) -
                audioif_dynamics_gain_to_db(state->slow_env + 1e-5f);
            float norm = diff / 6.0f;
            if (norm > 1.0f) {
                norm = 1.0f;
            } else if (norm < -1.0f) {
                norm = -1.0f;
            }
            float gain_db;
            if (config->transient_dual) {
                // A second, slower pair drives the sustain section, and both
                // differences are applied at once rather than one of them
                // being selected by the sign of the first.
                state->fast_env2 +=
                    (level > state->fast_env2 ? sus_fast_att : sus_fast_rel)
                    * (level - state->fast_env2);
                if (config->slow_hold_frames != 0u) {
                    if (level >= state->slow_peak) {
                        state->slow_peak = level;
                        state->slow_hold_left = config->slow_hold_frames;
                    } else if (state->slow_hold_left != 0u) {
                        state->slow_hold_left--;
                    } else {
                        state->slow_peak +=
                            sus_slow_rel * (level - state->slow_peak);
                    }
                    state->slow_env2 = state->slow_peak;
                } else {
                    state->slow_env2 +=
                        (level > state->slow_env2 ? sus_slow_att
                                                  : sus_slow_rel)
                        * (level - state->slow_env2);
                }
                const float diff2 =
                    audioif_dynamics_gain_to_db(state->fast_env2 + 1e-5f) -
                    audioif_dynamics_gain_to_db(state->slow_env2 + 1e-5f);
                float norm2 = diff2 / 6.0f;
                if (norm2 > 1.0f) {
                    norm2 = 1.0f;
                } else if (norm2 < -1.0f) {
                    norm2 = -1.0f;
                }
                gain_db = config->attack_gain_db * (norm > 0.0f ? norm : 0.0f)
                    + config->sustain_gain_db * (norm2 < 0.0f ? -norm2 : 0.0f);
            } else {
                gain_db = norm > 0.0f ? config->attack_gain_db * norm
                                      : config->sustain_gain_db * -norm;
            }
            state->gain_reduction_db = gain_db;
            gain = audioif_dynamics_db_to_gain(gain_db) * config->makeup_gain;
        } else if (gate_machine) {
            // Closed -> attack -> hold -> decay, on the same peak level. Once
            // ATTACK has been entered it runs to unity whatever the key does
            // next, which is the trigger state a memoryless gain computer
            // cannot have: a 1 ms burst under a 200 ms attack still opens.
            if (level >= open_level &&
                state->gate_stage != AUDIOIF_DYNAMICS_GATE_HOLD) {
                state->gate_stage = AUDIOIF_DYNAMICS_GATE_ATTACK;
            }
            switch (state->gate_stage) {
                case AUDIOIF_DYNAMICS_GATE_ATTACK:
                    state->gate_gain += config->attack_coef *
                        (1.0f - state->gate_gain);
                    if (state->gate_gain >= 0.999f) {
                        state->gate_gain = 1.0f;
                        state->gate_stage = AUDIOIF_DYNAMICS_GATE_HOLD;
                        state->hold_left = config->hold_frames;
                    }
                    break;
                case AUDIOIF_DYNAMICS_GATE_HOLD:
                    state->gate_gain = 1.0f;
                    // The hold reloads while the key stays above the
                    // hysteresis point, which is below the open threshold --
                    // so a level hovering on the threshold does not chatter.
                    if (level >= close_level) {
                        state->hold_left = config->hold_frames;
                    } else if (state->hold_left != 0u) {
                        state->hold_left--;
                    } else {
                        state->gate_stage = AUDIOIF_DYNAMICS_GATE_DECAY;
                    }
                    break;
                case AUDIOIF_DYNAMICS_GATE_DECAY:
                    state->gate_gain += config->release_coef *
                        (floor_gain - state->gate_gain);
                    if (state->gate_gain <= floor_gain + 1e-6f) {
                        state->gate_gain = floor_gain;
                        state->gate_stage = AUDIOIF_DYNAMICS_GATE_CLOSED;
                    }
                    break;
                case AUDIOIF_DYNAMICS_GATE_CLOSED:
                default:
                    state->gate_gain = floor_gain;
                    break;
            }
            state->gain_reduction_db =
                audioif_dynamics_gain_to_db(state->gate_gain);
            gain = state->gate_gain * config->makeup_gain;
        } else {
            const bool rising = level > state->envelope;
            float coef = rising ? config->attack_coef : config->release_coef;
            if (config->program_attack && rising &&
                threshold_level > 0.0f) {
                // The coefficient is scaled by the square root of how far
                // the level is over the threshold, which puts 20 dB over
                // about 3.4x faster than 10 dB over -- the ratio a
                // program-dependent attack is specified by. Scaling by the
                // overshoot itself gives 6x, which is past every dossier's
                // band. A root and a ratio, so no logarithm is needed.
                const float overshoot = level / threshold_level;
                if (overshoot > 1.0f) {
                    coef *= sqrtf(overshoot);
                    if (coef > 1.0f) {
                        coef = 1.0f;
                    }
                }
            }
            state->envelope += coef * (level - state->envelope);
            float env_db =
                audioif_dynamics_gain_to_db(state->envelope + 1e-6f);
            if (config->relative_threshold) {
                // The gain computer sees the side-chained level relative to
                // the full-band one, so a fixed spectral balance gets the
                // same reduction wherever it sits in the level range.
                state->full_env += (full_level > state->full_env
                    ? config->attack_coef : config->release_coef)
                    * (full_level - state->full_env);
                env_db -= audioif_dynamics_gain_to_db(state->full_env + 1e-6f);
            }
            const float gain_db = dynamics_gain_db(config, env_db);
            state->gain_reduction_db = gain_db;
            gain = audioif_dynamics_db_to_gain(gain_db) * config->makeup_gain;
        }
        // The gain smoother sits here, after every one of the gain
        // computers above and before the multiply, so one place covers all
        // five modes. Off by default, where `gain_smooth_coef` is 1.0 and
        // this is an identity.
        if (config->gain_smooth_coef < 1.0f) {
            if (!state->gain_smooth_primed) {
                state->smoothed_gain = gain;
                state->gain_smooth_primed = true;
            } else {
                state->smoothed_gain += config->gain_smooth_coef *
                    (gain - state->smoothed_gain);
            }
            gain = state->smoothed_gain;
        }
        // Key Listen puts the detector's own signal on the output, which is
        // how a gate's key band is auditioned; otherwise this is the audio.
        const float *out_source = config->key_listen ? detector : delayed;
        const float out_gain = config->key_listen ? 1.0f : gain;
        for (uint32_t channel = 0; channel < channels; ++channel) {
            float value = out_source[channel] * out_gain * 32768.0f;
            if (value > 32767.0f) {
                value = 32767.0f;
            } else if (value < -32768.0f) {
                value = -32768.0f;
            }
            output[channel] = (int16_t)value;
            if (config->feedback_detector) {
                state->prev_output[channel] = value * (1.0f / 32768.0f);
            }
        }
        input += channels;
        output += channels;
    }
}
