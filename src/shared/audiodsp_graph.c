// The graph runner. See audiodsp_graph.h.
// SPDX-License-Identifier: MIT

// First, so no multiply-add here is fused on one target and not another: a
// graph renders the same samples on every board (audiodsp_fp_contract.h).
#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_graph.h"

#include "shared/audiodsp_freeverb.h"
#include "shared/audiodsp_graph_overdrive.h"

#include <float.h>
#include <math.h>
#include <string.h>

// The queue's and the published words' ordering: the acquire/release pair
// audiopump.c uses, and a volatile fallback for compilers without
// __atomic_* (MSVC, the Windows wheel), where x86's ordering does the rest.
#if defined(__GNUC__) || defined(__clang__)
#define GRAPH_LOAD_ACQ(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define GRAPH_STORE_REL(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#else
#define GRAPH_LOAD_ACQ(p)     (*(volatile const uint32_t *)(p))
#define GRAPH_STORE_REL(p, v) (*(volatile uint32_t *)(p) = (v))
#endif

// Round to the nearest integer, ties to even: what lrintf gives in the default
// rounding mode, without calling it. CircuitPython's RP2350 port links no
// lrintf, so a call to it leaves the firmware unable to link (the same trap
// audiodsp_trig.c steps round for fmod). Callers pass a finite value well
// inside int32_t range: a sample rate in Hz, or a sample already clamped to
// the int16 range. A NaN gives 0, as lrintf's result does once cut to int16.
static inline int32_t round_even(float v) {
    if (!(v == v)) {
        return 0;
    }
    float a = v < 0.0f ? -v : v;
    int32_t i = (int32_t)a;
    float frac = a - (float)i;  // exact: a and i are within a binade
    if (frac > 0.5f || (frac == 0.5f && (i & 1))) {
        ++i;
    }
    return v < 0.0f ? -i : i;
}

// ---------------------------------------------------------------- nodes --

typedef struct {
    audiodsp_oscillator_config_t config;
    audiodsp_oscillator_state_t state;
} graph_osc_t;

typedef struct {
    audiodsp_ladder_config_t config;
    audiodsp_ladder_state_t state;
} graph_ladder_t;

typedef struct {
    audiodsp_biquad_f32_config_t config;
    audiodsp_biquad_f32_state_t state;
} graph_biquad_t;

typedef struct {
    audiodsp_feedback_delay_config_t config;
    audiodsp_feedback_delay_state_t state;
    uint32_t line_frames;
    // The line follows, 2 * line_frames samples.
    int16_t line[];
} graph_fbdelay_t;

enum { ENV_IDLE = 0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

// vrack's EnvelopeCore: each stage a one-pole approach to a target, the
// attack aimed past 1.0 so it arrives with a slope.
#define ENV_ATTACK_TARGET 1.3f
#define ENV_DECAY_TOLERANCE 0.001f
#define ENV_RELEASE_TOLERANCE 0.0005f
#define ENV_GATE_THRESHOLD (0.1f * AUDIODSP_GRAPH_UNIT)

typedef struct {
    uint8_t stage;
    bool high;
    float val;
    float a, d, r;
} graph_env_t;

typedef struct {
    float phase;
    // The random shape: the level held for this cycle (-1..1), the square's
    // state at the last sample, the shape last run, and the generator.
    float level;
    bool high;
    uint8_t shape;
    uint32_t rng;
} graph_lfo_t;

typedef struct {
    audiodsp_allpass_f32_config_t config;
    audiodsp_allpass_f32_state_t state;
    // The stages' state follows, `config.stages` floats.
    float lanes[];
} graph_phaser_t;

typedef struct {
    // The linear shape (GlideCore in dsp/glide.py): the value has been seen,
    // the note being glided to, the step per sample and the samples left.
    bool primed;
    float value;
    float target;
    float slope;
    uint32_t left;
    // The one-pole shape (GlideCore in dsp/utility.py), which starts at 0.
    float pole;
} graph_lag_t;

typedef struct {
    // The level followed so far, in counts. Written once a block.
    float env;
} graph_follow_t;

typedef struct {
    // vrack's SampleAndHoldCore: the clock's state at the last sample, the
    // value held and the output slewing toward it.
    bool high;
    float held;
    float out;
    // The free-running hold when the clock is open: audioshaper's kernel.
    audiodsp_samplehold_config_t config;
    audiodsp_samplehold_state_t state;
    uint32_t rate;
} graph_sh_t;

// audiobiquad.AllPass's default, for a PHASER built with `arg` 0.
#define PHASER_DEFAULT_STAGES 4u

static uint32_t phaser_stages(float arg) {
    if (!(arg >= 1.0f)) {
        return PHASER_DEFAULT_STAGES;
    }
    if (arg > (float)AUDIODSP_FILTER_F32_MAX_STAGES) {
        return AUDIODSP_FILTER_F32_MAX_STAGES;
    }
    return (uint32_t)arg;
}

typedef struct {
    // The two integrator states of dsp/twopole.py, carried across blocks.
    float s1;
    float s2;
} graph_twopole_t;

typedef struct {
    // The vactrol's state, in double as LowPassGateCore keeps it.
    double vactrol;
    // The cutoff the coefficients were made for; 0 before the first.
    double cutoff;
    // synthio.Biquad's low-pass coefficients, their fractional bits, and its
    // filter state (`y` with LPG_STATE_SHIFT bits below the sample).
    int32_t a1, a2, b0, b1, b2;
    int32_t shift;
    int32_t x0, x1, y0, y1;
} graph_lpg_t;

typedef struct {
    // audiomodal.Bank's kernel, one channel, with its tables inline.
    audiodsp_modal_config_t config;
    audiodsp_modal_state_t state;
    audiodsp_modal_mode_t modes[AUDIODSP_GRAPH_MODAL_MODES];
    audiodsp_modal_coeff_t coeffs[AUDIODSP_GRAPH_MODAL_MODES];
    float s1[AUDIODSP_GRAPH_MODAL_MODES];
    float s2[AUDIODSP_GRAPH_MODAL_MODES];
    // The parameters the modes were made from: frequency, decay,
    // brightness and the ratios, once `ready` says the modes were made.
    float tuned[3 + AUDIODSP_GRAPH_MODAL_MODES];
    bool ready;
    // The strike's burst: its last value and the samples it has left.
    double burst;
    int32_t pending;
    // The strike was above the threshold at the end of the last block.
    bool high;
} graph_modal_t;

static void modal_init(graph_modal_t *d, uint32_t sample_rate);
static void modal_bind(graph_modal_t *d);

typedef struct {
    audiodsp_modal_config_t config;
    audiodsp_modal_state_t state;
    audiodsp_modal_mode_t modes[AUDIODSP_GRAPH_STRUCK_MODES];
    audiodsp_modal_coeff_t coeffs[AUDIODSP_GRAPH_STRUCK_MODES];
    float s1[AUDIODSP_GRAPH_STRUCK_MODES];
    float s2[AUDIODSP_GRAPH_STRUCK_MODES];
    float table[AUDIODSP_GRAPH_STRUCK_MODES * 3];
    uint32_t table_modes;
    bool has_table;
    bool ready;
    float tuned_f0;
    bool high;
    bool pulse_active;
    uint32_t pulse_len;
    uint32_t pulse_pos;
    float pulse_amp;
    uint32_t pulse_shape;
    // The bank table() prepared for this table, off the audio thread.
    audiodsp_graph_struck_prep_t pre;
} graph_struck_t;

static void struck_init(graph_struck_t *d, uint32_t sample_rate);
static void struck_bind(graph_struck_t *d);
static void struck_tune_config(audiodsp_modal_config_t *config,
    const float *table, uint32_t table_modes, float f0, uint32_t sample_rate);

typedef struct {
    uint32_t rng;
} graph_noise_t;

typedef struct {
    bool primed;
    float gain_now[AUDIODSP_GRAPH_MAX_INPUTS];
} graph_mix_t;

typedef struct {
    bool primed;
    // The right side's buffer id (the node's output is the left side).
    int16_t right;
    float left_now[AUDIODSP_GRAPH_MAX_INPUTS];
    float right_now[AUDIODSP_GRAPH_MAX_INPUTS];
} graph_pan_t;

typedef struct {
    // The right side's buffer id (the node's output is the left side).
    int16_t right;
    // TremoloCore's phase in radians and its last block's depth, both in
    // double as the Python keeps them.
    double phase;
    double depth_now;
} graph_tremolo_t;

typedef struct {
    // The envelope's level before this block, the velocity the trigger set,
    // and the sample in the burst it has reached (the trigger's own is 0).
    float level;
    float velocity;
    uint32_t sample;
    // The subtracted rise envelope's level (DECAY_RISE); 0 when unused.
    float rise;
    // The trigger was above the threshold at the end of the last block, and
    // the envelope is still sounding.
    bool high;
    bool ringing;
    // The burst table: `strikes` pairs of (delay s, level), in delay order.
    float table[AUDIODSP_GRAPH_DECAY_STRIKES * 2];
    uint32_t strikes;
    bool has_table;
} graph_decay_t;

typedef struct {
    // Paul Kellet's seven pinking states.
    float b0, b1, b2, b3, b4, b5, b6;
} graph_pink_t;

typedef struct {
    // AcidFilterCore's four one-pole stages, its high-pass state and the
    // accent capacitor.
    float s0, s1, s2, s3;
    float hp;
    float accent_env;
    // exp(-1 / (sample_rate * 0.15)), the accent's per-sample decay.
    float accent_decay;
} graph_acid_t;

// One mono Freeverb bank: the eight combs and four all-passes of
// audiodsp_freeverb_process_s16 at channel_count 1.
typedef struct {
    int16_t comb[AUDIODSP_FREEVERB_COMB_SAMPLES];
    uint32_t comb_index[8];
    int16_t comb_filter[8];
    int16_t allpass[AUDIODSP_FREEVERB_ALLPASS_SAMPLES];
    uint32_t allpass_index[4];
} graph_spring_tank_t;

typedef struct {
    // The right side's buffer id (the node's output is the left side).
    int16_t right;
    graph_spring_tank_t left;
    graph_spring_tank_t right_tank;
} graph_spring_t;

size_t audiodsp_graph_node_data_size(audiodsp_graph_kind_t kind,
    uint32_t sample_rate, float arg) {
    switch (kind) {
        case AUDIODSP_GRAPH_OSC:
            return sizeof(graph_osc_t);
        case AUDIODSP_GRAPH_LADDER:
            return sizeof(graph_ladder_t);
        case AUDIODSP_GRAPH_BIQUAD:
            return sizeof(graph_biquad_t);
        case AUDIODSP_GRAPH_FBDELAY: {
            uint32_t line_frames = (uint32_t)((float)sample_rate * arg / 1000.0f);
            if (line_frames < 2u) {
                line_frames = 2u;
            }
            return sizeof(graph_fbdelay_t) + (size_t)line_frames * 2u * sizeof(int16_t);
        }
        case AUDIODSP_GRAPH_ENV:
        case AUDIODSP_GRAPH_ADSR2:
            return sizeof(graph_env_t);
        case AUDIODSP_GRAPH_LFO:
            return sizeof(graph_lfo_t);
        case AUDIODSP_GRAPH_NOISE:
            return sizeof(graph_noise_t);
        case AUDIODSP_GRAPH_MIX:
            return sizeof(graph_mix_t);
        case AUDIODSP_GRAPH_PAN:
            return sizeof(graph_pan_t);
        case AUDIODSP_GRAPH_PHASER:
            return sizeof(graph_phaser_t) + (size_t)phaser_stages(arg) * sizeof(float);
        case AUDIODSP_GRAPH_LAG:
        case AUDIODSP_GRAPH_GLISS:
            return sizeof(graph_lag_t);
        case AUDIODSP_GRAPH_FOLLOW:
            return sizeof(graph_follow_t);
        case AUDIODSP_GRAPH_SAMPLE_HOLD:
            return sizeof(graph_sh_t);
        case AUDIODSP_GRAPH_TWOPOLE:
            return sizeof(graph_twopole_t);
        case AUDIODSP_GRAPH_LPG:
            return sizeof(graph_lpg_t);
        case AUDIODSP_GRAPH_MODAL:
            return sizeof(graph_modal_t);
        case AUDIODSP_GRAPH_STRUCK:
            return sizeof(graph_struck_t);
        case AUDIODSP_GRAPH_TREMOLO:
            return sizeof(graph_tremolo_t);
        case AUDIODSP_GRAPH_DECAY:
            return sizeof(graph_decay_t);
        case AUDIODSP_GRAPH_PINK:
            return sizeof(graph_pink_t);
        case AUDIODSP_GRAPH_ACID:
            return sizeof(graph_acid_t);
        case AUDIODSP_GRAPH_SPRING:
            return sizeof(graph_spring_t);
#if AUDIODSP_GRAPH_HAS_OVERDRIVE
        case AUDIODSP_GRAPH_OVERDRIVE:
            return audiodsp_graph_overdrive_size();
#endif
        default:
            return 0;
    }
}

void audiodsp_graph_init(audiodsp_graph_t *graph, uint32_t sample_rate,
    uint32_t frames, uint32_t channel_count, int16_t **bufs, uint32_t nbufs,
    audiodsp_graph_node_t **nodes, uint32_t max_nodes) {
    graph->sample_rate = sample_rate;
    graph->frames = frames;
    graph->channel_count = channel_count;
    graph->bufs = bufs;
    graph->nbufs = nbufs;
    graph->nodes = nodes;
    graph->max_nodes = max_nodes;
    graph->nnodes = 0;
    graph->sounding = 0;
    graph->enable = 0xFFFFFFFFu;
    graph->pub_sounding = 0;
    graph->pub_frame = 0;
    graph->pub_applied = 0;
    graph->applied = 0;
    graph->records = NULL;
    graph->capacity = 0;
    graph->head = 0;
    graph->published = 0;
    graph->tail = 0;
    graph->held = false;
    graph->pushed = 0;
    graph->overflows = 0;
    graph->splits = 0;
    graph->high_water = 0;
    graph->shadow = NULL;
    for (uint32_t i = 0; i < nbufs; ++i) {
        memset(bufs[i], 0, (size_t)frames * sizeof(int16_t));
    }
}

static float env_coeff(float seconds, float scale, uint32_t sample_rate) {
    float steps = seconds * (float)sample_rate * scale;
    if (steps < 1.0f) {
        steps = 1.0f;
    }
    return expf(-1.0f / steps);
}

bool audiodsp_graph_kind_built(audiodsp_graph_kind_t kind) {
    #if !AUDIODSP_GRAPH_HAS_OVERDRIVE
    if (kind == AUDIODSP_GRAPH_OVERDRIVE) {
        return false;
    }
    #endif
    return kind < AUDIODSP_GRAPH_KINDS;
}

int audiodsp_graph_add(audiodsp_graph_t *graph, audiodsp_graph_kind_t kind,
    uint32_t group, const int16_t *inputs, uint32_t ninputs, int16_t output,
    void *data, float arg) {
    if (graph->nnodes >= graph->max_nodes || !audiodsp_graph_kind_built(kind) ||
        ninputs > AUDIODSP_GRAPH_MAX_INPUTS) {
        return -1;
    }
    if (output != AUDIODSP_GRAPH_NONE && (output < 0 || (uint32_t)output >= graph->nbufs)) {
        return -1;
    }
    if (output == AUDIODSP_GRAPH_NONE && kind != AUDIODSP_GRAPH_OUT) {
        return -1;
    }
    if ((kind == AUDIODSP_GRAPH_PAN || kind == AUDIODSP_GRAPH_TREMOLO ||
        kind == AUDIODSP_GRAPH_SPRING) &&
        (!(arg >= 0.0f && arg < (float)graph->nbufs) ||
        arg != (float)(int16_t)arg || (int16_t)arg == output)) {
        return -1;
    }
    for (uint32_t i = 0; i < ninputs; ++i) {
        if (inputs[i] != AUDIODSP_GRAPH_NONE &&
            (inputs[i] < 0 || (uint32_t)inputs[i] >= graph->nbufs)) {
            return -1;
        }
    }
    audiodsp_graph_node_t *node = audiodsp_graph_node(graph, graph->nnodes);
    memset(node, 0, sizeof(*node));
    node->kind = (uint8_t)kind;
    node->group = (uint8_t)group;
    node->ninputs = (uint8_t)ninputs;
    for (uint32_t i = 0; i < AUDIODSP_GRAPH_MAX_INPUTS; ++i) {
        node->inputs[i] = i < ninputs ? inputs[i] : AUDIODSP_GRAPH_NONE;
    }
    node->output = output;
    node->noutputs = output == AUDIODSP_GRAPH_NONE ? 0 : 1;
    node->data = data;
    node->dirty = true;
    node->arg = arg;
    const uint32_t rate = graph->sample_rate;
    switch (kind) {
        case AUDIODSP_GRAPH_OSC: {
            graph_osc_t *d = data;
            audiodsp_oscillator_config_init(&d->config, rate);
            audiodsp_oscillator_set_channel_count(&d->config, 1);
            audiodsp_oscillator_config_finish(&d->config);
            audiodsp_oscillator_state_init(&d->state);
            node->param[AUDIODSP_OSCILLATOR_OPT_FREQUENCY] = d->config.frequency;
            node->param[AUDIODSP_OSCILLATOR_OPT_WAVEFORM] = d->config.waveform[0];
            node->param[AUDIODSP_OSCILLATOR_OPT_PULSE_WIDTH] = d->config.pulse_width;
            node->param[AUDIODSP_OSCILLATOR_OPT_LEVEL] = d->config.level;
            node->param[AUDIODSP_OSCILLATOR_OPT_PITCH_OCTAVES] = d->config.pitch_octaves;
            node->param[AUDIODSP_OSCILLATOR_OPT_FM_DEPTH] = d->config.fm_depth;
            node->param[AUDIODSP_OSCILLATOR_OPT_PWM_DEPTH] = d->config.pwm_depth;
            break;
        }
        case AUDIODSP_GRAPH_LADDER: {
            graph_ladder_t *d = data;
            audiodsp_ladder_config_init(&d->config, rate);
            audiodsp_ladder_set_channel_count(&d->config, 1);
            audiodsp_ladder_config_finish(&d->config);
            audiodsp_ladder_state_init(&d->state);
            node->param[AUDIODSP_LADDER_OPT_CUTOFF_HZ] = d->config.cutoff_hz;
            node->param[AUDIODSP_LADDER_OPT_RESONANCE] = d->config.resonance;
            node->param[AUDIODSP_LADDER_OPT_DRIVE] = d->config.drive;
            node->param[AUDIODSP_LADDER_OPT_POLES] = d->config.poles;
            node->param[AUDIODSP_LADDER_OPT_PASSBAND_COMP] = d->config.passband_comp;
            node->param[AUDIODSP_LADDER_OPT_OVERSAMPLE] = d->config.oversample;
            node->param[AUDIODSP_LADDER_OPT_MIX] = d->config.mix;
            node->param[AUDIODSP_LADDER_OPT_CUTOFF_OCTAVES] = d->config.cutoff_octaves;
            break;
        }
        case AUDIODSP_GRAPH_BIQUAD: {
            graph_biquad_t *d = data;
            audiodsp_biquad_f32_config_init(&d->config, rate, 1);
            audiodsp_biquad_f32_state_init(&d->state);
            node->param[AUDIODSP_BIQUAD_F32_OPT_MODE] = (float)d->config.mode;
            node->param[AUDIODSP_BIQUAD_F32_OPT_FREQUENCY] = d->config.frequency;
            node->param[AUDIODSP_BIQUAD_F32_OPT_Q] = d->config.q;
            node->param[AUDIODSP_BIQUAD_F32_OPT_GAIN_DB] = d->config.gain_db;
            node->param[AUDIODSP_BIQUAD_F32_OPT_MIX] = d->config.mix;
            break;
        }
        case AUDIODSP_GRAPH_FBDELAY: {
            graph_fbdelay_t *d = data;
            uint32_t line_frames = (uint32_t)((float)rate * arg / 1000.0f);
            if (line_frames < 2u) {
                line_frames = 2u;
            }
            d->line_frames = line_frames;
            audiodsp_feedback_delay_config_init(&d->config, rate, line_frames);
            audiodsp_feedback_delay_set_channel_count(&d->config, 1);
            audiodsp_feedback_delay_configure(&d->config,
                AUDIODSP_FEEDBACK_DELAY_OPT_DELAY_MS, arg * 0.5f);
            audiodsp_feedback_delay_config_finish(&d->config);
            audiodsp_feedback_delay_state_init(&d->state, d->line);
            // What the config holds now, so get() answers before any set().
            node->param[AUDIODSP_FEEDBACK_DELAY_OPT_DELAY_MS] = arg * 0.5f;
            node->param[AUDIODSP_FEEDBACK_DELAY_OPT_FEEDBACK] = d->config.feedback;
            node->param[AUDIODSP_FEEDBACK_DELAY_OPT_MIX] = d->config.mix;
            node->param[AUDIODSP_FEEDBACK_DELAY_OPT_DAMPING_HZ] = d->config.damping_hz;
            node->param[AUDIODSP_FEEDBACK_DELAY_OPT_CUT_HZ] = d->config.cut_hz;
            node->param[AUDIODSP_FEEDBACK_DELAY_OPT_WOW_HZ] = d->config.wow_hz;
            node->param[AUDIODSP_FEEDBACK_DELAY_OPT_CROSS_FEED] = d->config.cross_feed;
            node->param[AUDIODSP_FEEDBACK_DELAY_OPT_LOOP_DRIVE] = d->config.loop_drive;
            break;
        }
        case AUDIODSP_GRAPH_ENV: {
            graph_env_t *d = data;
            d->stage = ENV_IDLE;
            d->val = 0.0f;
            node->param[AUDIODSP_GRAPH_ENV_ATTACK] = 0.01f;
            node->param[AUDIODSP_GRAPH_ENV_DECAY] = 0.2f;
            node->param[AUDIODSP_GRAPH_ENV_SUSTAIN] = 0.7f;
            node->param[AUDIODSP_GRAPH_ENV_RELEASE] = 0.3f;
            break;
        }
        case AUDIODSP_GRAPH_ADSR2: {
            graph_env_t *d = data;
            d->stage = ENV_IDLE;
            d->val = 0.0f;
            node->param[AUDIODSP_GRAPH_ADSR2_ATTACK] = 0.01f;
            node->param[AUDIODSP_GRAPH_ADSR2_DECAY] = 0.2f;
            node->param[AUDIODSP_GRAPH_ADSR2_SUSTAIN] = 0.7f;
            node->param[AUDIODSP_GRAPH_ADSR2_RELEASE] = 0.3f;
            node->param[AUDIODSP_GRAPH_ADSR2_INITIAL] = 0.0f;
            node->param[AUDIODSP_GRAPH_ADSR2_ATTACK_LEVEL] = 1.0f;
            break;
        }
        case AUDIODSP_GRAPH_LFO: {
            graph_lfo_t *d = data;
            d->rng = 0x5EED1234u ^ (graph->nnodes * 0x9E3779B9u);
            node->param[AUDIODSP_GRAPH_LFO_RATE] = 1.0f;
            node->param[AUDIODSP_GRAPH_LFO_AMPLITUDE] = 0.5f * AUDIODSP_GRAPH_UNIT;
            node->param[AUDIODSP_GRAPH_LFO_SHAPE] = (float)AUDIODSP_GRAPH_LFO_TRIANGLE;
            break;
        }
        case AUDIODSP_GRAPH_PHASER: {
            graph_phaser_t *d = data;
            const uint32_t stages = phaser_stages(arg);
            audiodsp_allpass_f32_config_init(&d->config, rate, 1, stages);
            audiodsp_allpass_f32_config_finish(&d->config);
            audiodsp_allpass_f32_state_init(&d->state, d->lanes, stages);
            node->param[AUDIODSP_ALLPASS_F32_OPT_FREQUENCY] = d->config.frequency;
            node->param[AUDIODSP_ALLPASS_F32_OPT_FEEDBACK] = d->config.feedback;
            node->param[AUDIODSP_ALLPASS_F32_OPT_MIX] = d->config.mix;
            break;
        }
        case AUDIODSP_GRAPH_LAG:
            node->param[AUDIODSP_GRAPH_LAG_MODE] = (float)AUDIODSP_GRAPH_LAG_ON;
            break;
        case AUDIODSP_GRAPH_GLISS:
            node->param[AUDIODSP_GRAPH_GLISS_MODE] = (float)AUDIODSP_GRAPH_LAG_ON;
            node->param[AUDIODSP_GRAPH_GLISS_STEP] = 1.0f;
            break;
        case AUDIODSP_GRAPH_FOLLOW: {
            graph_follow_t *d = data;
            d->env = 0.0f;
            // vocoder330's FOLLOW_ATTACK_S and FOLLOW_RELEASE_S.
            node->param[AUDIODSP_GRAPH_FOLLOW_ATTACK] = 0.005f;
            node->param[AUDIODSP_GRAPH_FOLLOW_RELEASE] = 0.030f;
            node->param[AUDIODSP_GRAPH_FOLLOW_MODE] = (float)AUDIODSP_GRAPH_FOLLOW_PEAK;
            break;
        }
        case AUDIODSP_GRAPH_SAMPLE_HOLD: {
            graph_sh_t *d = data;
            audiodsp_samplehold_config_init(&d->config, 1u, 1u);
            audiodsp_samplehold_state_init(&d->state, &d->config);
            break;
        }
        case AUDIODSP_GRAPH_TWOPOLE:
            node->param[AUDIODSP_GRAPH_TWOPOLE_TYPE] = (float)AUDIODSP_GRAPH_TWOPOLE_SK_LOWPASS;
            node->param[AUDIODSP_GRAPH_TWOPOLE_CUTOFF_HZ] = 1000.0f;
            node->param[AUDIODSP_GRAPH_TWOPOLE_DEPTH] = 1.0f;
            node->param[AUDIODSP_GRAPH_TWOPOLE_RESONANCE] = 0.0f;
            node->param[AUDIODSP_GRAPH_TWOPOLE_DRIVE] = 1.0f;
            break;
        case AUDIODSP_GRAPH_OVERDRIVE:
            // OverdriveCore.render's defaults.
            node->param[AUDIODSP_GRAPH_OVERDRIVE_DRIVE] = 0.5f;
            node->param[AUDIODSP_GRAPH_OVERDRIVE_TONE] = 0.5f;
            node->param[AUDIODSP_GRAPH_OVERDRIVE_LEVEL] = 1.0f;
            break;
        case AUDIODSP_GRAPH_TANH:
            node->param[AUDIODSP_GRAPH_TANH_DRIVE] = 1.0f;
            node->param[AUDIODSP_GRAPH_TANH_LEVEL] = 1.0f;
            break;
        case AUDIODSP_GRAPH_LPG:
            // LowPassGateCore.render's defaults, and unity after it.
            node->param[AUDIODSP_GRAPH_LPG_MODE] = (float)AUDIODSP_GRAPH_LPG_BOTH;
            node->param[AUDIODSP_GRAPH_LPG_DECAY] = 0.25f;
            node->param[AUDIODSP_GRAPH_LPG_MANUAL] = 0.0f;
            node->param[AUDIODSP_GRAPH_LPG_LEVEL] = 1.0f;
            break;
        case AUDIODSP_GRAPH_MODAL: {
            // ModalResonator's panel at rest: middle C, a bar's ratios at
            // structure 0.5, its damping, brightness and strike level.
            graph_modal_t *d = data;
            static const float bar[AUDIODSP_GRAPH_MODAL_MODES] = {
                1.0f, 2.0f, 3.0f, 4.2f, 7.9f, 9.8f, 11.9f, 14.2f,
            };
            node->param[AUDIODSP_GRAPH_MODAL_FREQUENCY] = 261.6256f;
            node->param[AUDIODSP_GRAPH_MODAL_DECAY] = 1.6068429f;
            node->param[AUDIODSP_GRAPH_MODAL_BRIGHTNESS] = 0.6f;
            node->param[AUDIODSP_GRAPH_MODAL_INPUT] = 0.05f;
            node->param[AUDIODSP_GRAPH_MODAL_MIX] = 1.0f;
            for (uint32_t m = 0; m < AUDIODSP_GRAPH_MODAL_MODES; ++m) {
                node->param[AUDIODSP_GRAPH_MODAL_RATIO + m] = bar[m];
            }
            modal_init(d, graph->sample_rate);
            break;
        }
        case AUDIODSP_GRAPH_STRUCK: {
            // HammerExciter + StruckBarCore at middle C with the tine table.
            graph_struck_t *d = data;
            node->param[AUDIODSP_GRAPH_STRUCK_FREQUENCY] = 261.6256f;
            node->param[AUDIODSP_GRAPH_STRUCK_HARDNESS] = 0.5f;
            node->param[AUDIODSP_GRAPH_STRUCK_LEVEL] = 0.8f;
            node->param[AUDIODSP_GRAPH_STRUCK_SHAPE] = (float)AUDIODSP_GRAPH_STRUCK_HAMMER;
            struck_init(d, graph->sample_rate);
            break;
        }
        case AUDIODSP_GRAPH_PICKUP:
            node->param[AUDIODSP_GRAPH_PICKUP_MODE] = (float)AUDIODSP_GRAPH_PICKUP_ELECTROMAGNETIC;
            node->param[AUDIODSP_GRAPH_PICKUP_DRIVE] = 1.0f;
            node->param[AUDIODSP_GRAPH_PICKUP_OFFSET] = 0.0f;
            node->param[AUDIODSP_GRAPH_PICKUP_LEVEL] = 1.0f;
            break;
        case AUDIODSP_GRAPH_TREMOLO:
            ((graph_tremolo_t *)data)->right = (int16_t)arg;
            ((graph_tremolo_t *)data)->phase = 0.0;
            ((graph_tremolo_t *)data)->depth_now = 0.0;
            node->param[AUDIODSP_GRAPH_TREMOLO_RATE] = 3.0f;
            node->param[AUDIODSP_GRAPH_TREMOLO_DEPTH] = 0.0f;
            node->param[AUDIODSP_GRAPH_TREMOLO_MODE] = (float)AUDIODSP_GRAPH_TREMOLO_PAN;
            break;
        case AUDIODSP_GRAPH_DECAY: {
            // OneShotEnvelopeCore's defaults: a 200 ms one-shot, no burst,
            // no attack ramp, no fast stage and no subtracted rise.
            graph_decay_t *d = data;
            d->level = 0.0f;
            d->velocity = 1.0f;
            d->sample = 0u;
            d->rise = 0.0f;
            d->high = false;
            d->ringing = false;
            d->strikes = 0u;
            d->has_table = false;
            node->param[AUDIODSP_GRAPH_DECAY_DECAY] = 0.2f;
            node->param[AUDIODSP_GRAPH_DECAY_RESTRIKE] = 0.01f;
            node->param[AUDIODSP_GRAPH_DECAY_ATTACK] = 0.0f;
            node->param[AUDIODSP_GRAPH_DECAY_FAST] = 0.0f;
            node->param[AUDIODSP_GRAPH_DECAY_FAST_SEC] = 0.0f;
            node->param[AUDIODSP_GRAPH_DECAY_RISE] = 0.0f;
            break;
        }
        case AUDIODSP_GRAPH_PINK: {
            graph_pink_t *d = data;
            d->b0 = d->b1 = d->b2 = d->b3 = d->b4 = d->b5 = d->b6 = 0.0f;
            node->param[AUDIODSP_GRAPH_PINK_LEVEL] = 1.0f;
            break;
        }
        case AUDIODSP_GRAPH_CRUSH:
            // BitDepthRateCore's default bit depth.
            node->param[AUDIODSP_GRAPH_CRUSH_BITS] = 6.0f;
            break;
        case AUDIODSP_GRAPH_ACID: {
            // AcidFilterCore at rest: middle C's cutoff, no resonance, the
            // env_mod the acid303 panel starts at.
            graph_acid_t *d = data;
            d->s0 = d->s1 = d->s2 = d->s3 = 0.0f;
            d->hp = 0.0f;
            d->accent_env = 0.0f;
            d->accent_decay = expf(-1.0f / ((float)rate * 0.15f));
            node->param[AUDIODSP_GRAPH_ACID_CUTOFF_HZ] = 440.0f;
            node->param[AUDIODSP_GRAPH_ACID_RESONANCE] = 0.5f;
            node->param[AUDIODSP_GRAPH_ACID_ENVMOD] = 0.6f;
            break;
        }
        case AUDIODSP_GRAPH_SPRING: {
            // SpringReverbCore at patch2600's panel: the mix the spring
            // starts at and its default decay.
            graph_spring_t *d = data;
            d->right = (int16_t)arg;
            memset(&d->left, 0, sizeof(d->left));
            memset(&d->right_tank, 0, sizeof(d->right_tank));
            node->param[AUDIODSP_GRAPH_SPRING_MIX] = 0.25f;
            node->param[AUDIODSP_GRAPH_SPRING_DECAY] = 0.75f;
            node->param[AUDIODSP_GRAPH_SPRING_DAMP] = 0.55f;
            break;
        }
        case AUDIODSP_GRAPH_NOISE: {
            graph_noise_t *d = data;
            d->rng = 0x5EED1234u ^ (graph->nnodes * 0x9E3779B9u);
            node->param[0] = 0.5f * AUDIODSP_GRAPH_UNIT;
            break;
        }
        case AUDIODSP_GRAPH_MIX:
            for (uint32_t i = 0; i < AUDIODSP_GRAPH_MAX_INPUTS; ++i) {
                node->param[i] = 1.0f;
            }
            break;
        case AUDIODSP_GRAPH_PAN:
            ((graph_pan_t *)data)->right = (int16_t)arg;
            for (uint32_t i = 0; i < AUDIODSP_GRAPH_MAX_INPUTS; ++i) {
                node->param[i] = 1.0f;
            }
            break;
        case AUDIODSP_GRAPH_VCA:
            node->param[AUDIODSP_GRAPH_VCA_GAIN] = 1.0f;
            break;
        case AUDIODSP_GRAPH_OUT:
            node->param[0] = 1.0f;
            break;
        default:
            break;
    }
    if (graph->shadow != NULL) {
        memcpy(audiodsp_graph_shadow(graph, graph->nnodes),
            node->param, sizeof(node->param));
    }
    return (int)graph->nnodes++;
}

bool audiodsp_graph_set(audiodsp_graph_t *graph, uint32_t node,
    uint32_t index, float value) {
    if (node >= graph->nnodes || index >= AUDIODSP_GRAPH_MAX_PARAMS) {
        return false;
    }
    audiodsp_graph_node_t *n = audiodsp_graph_node(graph, node);
    n->param[index] = value;
    n->dirty = true;
    return true;
}

bool audiodsp_graph_get(const audiodsp_graph_t *graph, uint32_t node,
    uint32_t index, float *value) {
    if (node >= graph->nnodes || index >= AUDIODSP_GRAPH_MAX_PARAMS) {
        return false;
    }
    *value = audiodsp_graph_node(graph, node)->param[index];
    return true;
}

bool audiodsp_graph_set_table(audiodsp_graph_t *graph, uint32_t node,
    const int16_t *table, uint32_t length) {
    if (node >= graph->nnodes || audiodsp_graph_node(graph, node)->kind != AUDIODSP_GRAPH_FBDELAY) {
        return false;
    }
    graph_fbdelay_t *d = audiodsp_graph_node(graph, node)->data;
    return audiodsp_feedback_delay_set_wow_shape(&d->config, table, length);
}

static bool struck_table_ok(const audiodsp_graph_t *graph, uint32_t node,
    const float *table, uint32_t modes) {
    return node < graph->nnodes &&
           audiodsp_graph_node(graph, node)->kind == AUDIODSP_GRAPH_STRUCK &&
           modes <= AUDIODSP_GRAPH_STRUCK_MODES &&
           (modes == 0u || table != NULL);
}

bool audiodsp_graph_struck_prepare(const audiodsp_graph_t *graph,
    uint32_t node, const float *table, uint32_t modes,
    audiodsp_graph_struck_prep_t *prep) {
    prep->valid = false;
    float raw_f0 = 0.0f;
    if (!struck_table_ok(graph, node, table, modes) ||
        !audiodsp_graph_get_shadow(graph, node, AUDIODSP_GRAPH_STRUCK_FREQUENCY,
            &raw_f0)) {
        return false;
    }
    // run_struck()'s own clamp, and the bank struck_init() builds.
    const float f0 = raw_f0 > 20.0f ? raw_f0 : 20.0f;
    audiodsp_modal_config_t config;
    audiodsp_modal_config_init(&config, graph->sample_rate, 1u,
        AUDIODSP_GRAPH_STRUCK_MODES, prep->modes, prep->coeffs);
    struck_tune_config(&config, table, modes, f0, graph->sample_rate);
    prep->f0 = f0;
    prep->valid = true;
    return true;
}

bool audiodsp_graph_set_struck_table_prepared(audiodsp_graph_t *graph,
    uint32_t node, const float *table, uint32_t modes,
    const audiodsp_graph_struck_prep_t *prep) {
    if (!struck_table_ok(graph, node, table, modes)) {
        return false;
    }
    graph_struck_t *d = audiodsp_graph_node(graph, node)->data;
    if (modes > 0u) {
        memcpy(d->table, table, (size_t)modes * 3u * sizeof(float));
    }
    d->table_modes = modes;
    d->has_table = true;
    d->ready = false;
    if (prep != NULL && prep->valid) {
        d->pre = *prep;
    } else {
        d->pre.valid = false;
    }
    return true;
}

bool audiodsp_graph_set_struck_table(audiodsp_graph_t *graph, uint32_t node,
    const float *table, uint32_t modes) {
    audiodsp_graph_struck_prep_t prep;
    audiodsp_graph_struck_prepare(graph, node, table, modes, &prep);
    return audiodsp_graph_set_struck_table_prepared(graph, node, table, modes,
        &prep);
}

bool audiodsp_graph_set_decay_table(audiodsp_graph_t *graph, uint32_t node,
    const float *table, uint32_t strikes) {
    if (node >= graph->nnodes ||
        audiodsp_graph_node(graph, node)->kind != AUDIODSP_GRAPH_DECAY ||
        strikes > AUDIODSP_GRAPH_DECAY_STRIKES ||
        (strikes > 0u && table == NULL)) {
        return false;
    }
    graph_decay_t *d = audiodsp_graph_node(graph, node)->data;
    if (strikes > 0u) {
        memcpy(d->table, table, (size_t)strikes * 2u * sizeof(float));
    }
    // The block loop walks the strikes in delay order, as the rack's
    // "cur in spikes" finds them in any order: sort by delay here.
    for (uint32_t i = 1u; i < strikes; ++i) {
        const float delay = d->table[2u * i];
        const float level = d->table[2u * i + 1u];
        uint32_t j = i;
        while (j > 0u && d->table[2u * (j - 1u)] > delay) {
            d->table[2u * j] = d->table[2u * (j - 1u)];
            d->table[2u * j + 1u] = d->table[2u * (j - 1u) + 1u];
            --j;
        }
        d->table[2u * j] = delay;
        d->table[2u * j + 1u] = level;
    }
    d->strikes = strikes;
    d->has_table = true;
    return true;
}

// The planted faults that tests/parity/graph_carry_probe.py must catch, for a
// build made with -DAUDIODSP_GRAPH_CARRY_FAULT=n: 1 leaves a FBDELAY's line
// pointing at the source's, 2 drops the source's pending parameter change.
#ifndef AUDIODSP_GRAPH_CARRY_FAULT
#define AUDIODSP_GRAPH_CARRY_FAULT 0
#endif

int audiodsp_graph_carry(audiodsp_graph_t *dst, uint32_t node,
    const audiodsp_graph_t *src, uint32_t src_node) {
    if (node >= dst->nnodes || src_node >= src->nnodes) {
        return AUDIODSP_GRAPH_CARRY_BAD_NODE;
    }
    audiodsp_graph_node_t *d = audiodsp_graph_node(dst, node);
    const audiodsp_graph_node_t *s = audiodsp_graph_node(src, src_node);
    if (d == s) {
        return AUDIODSP_GRAPH_CARRY_OK;
    }
    if (d->kind != s->kind) {
        return AUDIODSP_GRAPH_CARRY_KIND;
    }
    const audiodsp_graph_kind_t kind = (audiodsp_graph_kind_t)d->kind;
    if (kind != AUDIODSP_GRAPH_PAN && kind != AUDIODSP_GRAPH_TREMOLO &&
        kind != AUDIODSP_GRAPH_SPRING && !(d->arg == s->arg)) {
        return AUDIODSP_GRAPH_CARRY_ARG;
    }
    const size_t size = audiodsp_graph_node_data_size(kind, dst->sample_rate, d->arg);
    if (size != audiodsp_graph_node_data_size(kind, src->sample_rate, s->arg)) {
        return AUDIODSP_GRAPH_CARRY_SIZE;
    }
    if (dst->sample_rate != src->sample_rate) {
        return AUDIODSP_GRAPH_CARRY_RATE;
    }
    // What stays the destination's own: a PAN's right side, a delay's wow table,
    // or a STRUCK or DECAY node's table when the destination installed its own.
    int16_t right = 0;
    const int16_t *wow = NULL;
    uint32_t wow_shift = 0;
    float struck_table[AUDIODSP_GRAPH_STRUCK_MODES * 3];
    uint32_t struck_modes = 0;
    bool struck_has_table = false;
    audiodsp_graph_struck_prep_t struck_pre;
    struck_pre.valid = false;
    float decay_table[AUDIODSP_GRAPH_DECAY_STRIKES * 2];
    uint32_t decay_strikes = 0;
    bool decay_has_table = false;
    if (kind == AUDIODSP_GRAPH_PAN) {
        right = ((const graph_pan_t *)d->data)->right;
    } else if (kind == AUDIODSP_GRAPH_TREMOLO) {
        right = ((const graph_tremolo_t *)d->data)->right;
    } else if (kind == AUDIODSP_GRAPH_SPRING) {
        right = ((const graph_spring_t *)d->data)->right;
    } else if (kind == AUDIODSP_GRAPH_FBDELAY) {
        wow = ((const graph_fbdelay_t *)d->data)->config.wow_shape;
        wow_shift = ((const graph_fbdelay_t *)d->data)->config.wow_shape_shift;
    } else if (kind == AUDIODSP_GRAPH_STRUCK) {
        const graph_struck_t *st = d->data;
        struck_has_table = st->has_table;
        if (struck_has_table) {
            struck_modes = st->table_modes;
            memcpy(struck_table, st->table, sizeof(struck_table));
            struck_pre = st->pre;
        }
    } else if (kind == AUDIODSP_GRAPH_DECAY) {
        const graph_decay_t *dc = d->data;
        decay_has_table = dc->has_table;
        if (decay_has_table) {
            decay_strikes = dc->strikes;
            memcpy(decay_table, dc->table, sizeof(decay_table));
        }
    }
    if (size) {
        memcpy(d->data, s->data, size);
    }
    switch (kind) {
        case AUDIODSP_GRAPH_FBDELAY: {
            // The line came with the state; the state must read this copy of it.
            // A table that differs from the source's takes over as table()
            // would hand it over: the depth dips, the table swaps at zero.
            graph_fbdelay_t *f = d->data;
            #if AUDIODSP_GRAPH_CARRY_FAULT != 1
            f->state.line = f->line;
            #endif
            f->config.wow_shape = wow;
            f->config.wow_shape_shift = wow_shift;
            // An equal copy of the table playing is the same table: no dip.
            if (wow != NULL && f->state.wow_shape_playing != NULL &&
                f->state.wow_shape_playing != wow &&
                f->state.wow_shape_playing_shift == wow_shift &&
                memcmp(f->state.wow_shape_playing, wow,
                    sizeof(int16_t) << wow_shift) == 0) {
                f->state.wow_shape_playing = wow;
            }
            break;
        }
        case AUDIODSP_GRAPH_PHASER: {
            graph_phaser_t *ph = d->data;
            ph->state.stage_state = ph->lanes;
            break;
        }
        case AUDIODSP_GRAPH_MODAL:
            modal_bind(d->data);
            break;
        case AUDIODSP_GRAPH_STRUCK: {
            graph_struck_t *st = d->data;
            if (struck_has_table) {
                const bool same = st->table_modes == struck_modes &&
                    memcmp(st->table, struck_table,
                        (size_t)struck_modes * 3u * sizeof(float)) == 0;
                memcpy(st->table, struck_table, sizeof(struck_table));
                st->table_modes = struck_modes;
                st->has_table = true;
                // What was prepared for the table goes with the table.
                st->pre = struck_pre;
                if (!same) {
                    st->ready = false;
                }
            }
            struck_bind(st);
            break;
        }
        case AUDIODSP_GRAPH_DECAY: {
            graph_decay_t *dc = d->data;
            if (decay_has_table) {
                memcpy(dc->table, decay_table, sizeof(decay_table));
                dc->strikes = decay_strikes;
                dc->has_table = true;
            }
            break;
        }
        case AUDIODSP_GRAPH_PAN:
            ((graph_pan_t *)d->data)->right = right;
            break;
        case AUDIODSP_GRAPH_TREMOLO:
            ((graph_tremolo_t *)d->data)->right = right;
            break;
        case AUDIODSP_GRAPH_SPRING:
            ((graph_spring_t *)d->data)->right = right;
            break;
        default:
            break;
    }
    d->ran = s->ran;
    const bool moved = memcmp(d->param, s->param, sizeof(d->param)) != 0;
    #if AUDIODSP_GRAPH_CARRY_FAULT == 2
    d->dirty = moved;
    #else
    d->dirty = s->dirty || moved;
    #endif
    return AUDIODSP_GRAPH_CARRY_OK;
}

bool audiodsp_graph_reset(audiodsp_graph_t *graph, uint32_t node) {
    if (node >= graph->nnodes) {
        return false;
    }
    audiodsp_graph_node_t *n = audiodsp_graph_node(graph, node);
    switch (n->kind) {
        case AUDIODSP_GRAPH_OSC:
            audiodsp_oscillator_reset(&((graph_osc_t *)n->data)->state);
            break;
        case AUDIODSP_GRAPH_LADDER:
            audiodsp_ladder_reset(&((graph_ladder_t *)n->data)->state);
            break;
        case AUDIODSP_GRAPH_BIQUAD:
            audiodsp_biquad_f32_reset(&((graph_biquad_t *)n->data)->state);
            break;
        case AUDIODSP_GRAPH_FBDELAY: {
            graph_fbdelay_t *d = n->data;
            audiodsp_feedback_delay_reset(&d->state, &d->config);
            break;
        }
        case AUDIODSP_GRAPH_ENV:
        case AUDIODSP_GRAPH_ADSR2: {
            graph_env_t *d = n->data;
            d->stage = ENV_IDLE;
            d->val = 0.0f;
            d->high = false;
            break;
        }
        case AUDIODSP_GRAPH_LFO: {
            graph_lfo_t *d = n->data;
            d->phase = 0.0f;
            d->level = 0.0f;
            d->high = false;
            break;
        }
        case AUDIODSP_GRAPH_MIX:
            ((graph_mix_t *)n->data)->primed = false;
            break;
        case AUDIODSP_GRAPH_PAN:
            ((graph_pan_t *)n->data)->primed = false;
            break;
        case AUDIODSP_GRAPH_PHASER:
            audiodsp_allpass_f32_reset(&((graph_phaser_t *)n->data)->state);
            break;
        case AUDIODSP_GRAPH_LAG:
        case AUDIODSP_GRAPH_GLISS: {
            graph_lag_t *d = n->data;
            d->primed = false;
            d->left = 0;
            d->slope = 0.0f;
            d->pole = 0.0f;
            break;
        }
        case AUDIODSP_GRAPH_FOLLOW:
            ((graph_follow_t *)n->data)->env = 0.0f;
            break;
        case AUDIODSP_GRAPH_SAMPLE_HOLD: {
            graph_sh_t *d = n->data;
            d->high = false;
            d->held = 0.0f;
            d->out = 0.0f;
            audiodsp_samplehold_reset(&d->state, &d->config);
            break;
        }
        case AUDIODSP_GRAPH_TWOPOLE: {
            graph_twopole_t *d = n->data;
            d->s1 = 0.0f;
            d->s2 = 0.0f;
            break;
        }
        case AUDIODSP_GRAPH_LPG: {
            graph_lpg_t *d = n->data;
            d->vactrol = 0.0;
            d->x0 = d->x1 = d->y0 = d->y1 = 0;
            break;
        }
        case AUDIODSP_GRAPH_MODAL: {
            graph_modal_t *d = n->data;
            modal_bind(d);
            audiodsp_modal_reset(&d->state);
            d->burst = 0.0;
            d->pending = 0;
            d->high = false;
            break;
        }
        case AUDIODSP_GRAPH_STRUCK: {
            graph_struck_t *d = n->data;
            struck_bind(d);
            audiodsp_modal_reset(&d->state);
            d->high = false;
            d->pulse_active = false;
            d->pulse_len = 0u;
            d->pulse_pos = 0u;
            d->pulse_amp = 0.0f;
            break;
        }
        case AUDIODSP_GRAPH_TREMOLO: {
            // TremoloCore.reset: the phase to 0 and no depth glide, so the
            // next block starts at the depth last set (the CV not counted).
            graph_tremolo_t *d = n->data;
            const float depth = n->param[AUDIODSP_GRAPH_TREMOLO_DEPTH];
            d->phase = 0.0;
            d->depth_now = depth > 1.0f ? 1.0 : (depth > 0.0f ? (double)depth : 0.0);
            break;
        }
        case AUDIODSP_GRAPH_DECAY: {
            // OneShotEnvelopeCore.reset and the burst cores' reset.
            graph_decay_t *d = n->data;
            d->level = 0.0f;
            d->velocity = 1.0f;
            d->sample = 0u;
            d->rise = 0.0f;
            d->high = false;
            d->ringing = false;
            break;
        }
        case AUDIODSP_GRAPH_PINK: {
            graph_pink_t *d = n->data;
            d->b0 = d->b1 = d->b2 = d->b3 = d->b4 = d->b5 = d->b6 = 0.0f;
            break;
        }
        case AUDIODSP_GRAPH_ACID: {
            graph_acid_t *d = n->data;
            d->s0 = d->s1 = d->s2 = d->s3 = 0.0f;
            d->hp = 0.0f;
            d->accent_env = 0.0f;
            break;
        }
        case AUDIODSP_GRAPH_SPRING: {
            graph_spring_t *d = n->data;
            memset(&d->left, 0, sizeof(d->left));
            memset(&d->right_tank, 0, sizeof(d->right_tank));
            break;
        }
#if AUDIODSP_GRAPH_HAS_OVERDRIVE
        case AUDIODSP_GRAPH_OVERDRIVE:
            audiodsp_graph_overdrive_reset(n->data);
            break;
#endif
        default:
            break;
    }
    return true;
}

// ------------------------------------------------------------- the run --

static void derive(audiodsp_graph_t *graph, audiodsp_graph_node_t *n) {
    n->dirty = false;
    switch (n->kind) {
        case AUDIODSP_GRAPH_OSC: {
            graph_osc_t *d = n->data;
            for (uint32_t i = 0; i <= AUDIODSP_OSCILLATOR_OPT_PWM_DEPTH; ++i) {
                if (i == AUDIODSP_OSCILLATOR_OPT_WAVEFORM_RIGHT) {
                    continue;
                }
                audiodsp_oscillator_configure(&d->config,
                    (audiodsp_oscillator_option_t)i, n->param[i]);
            }
            audiodsp_oscillator_config_finish(&d->config);
            break;
        }
        case AUDIODSP_GRAPH_LADDER: {
            graph_ladder_t *d = n->data;
            for (uint32_t i = 0; i <= AUDIODSP_LADDER_OPT_CUTOFF_OCTAVES; ++i) {
                audiodsp_ladder_configure(&d->config,
                    (audiodsp_ladder_option_t)i, n->param[i]);
            }
            break;
        }
        case AUDIODSP_GRAPH_BIQUAD: {
            graph_biquad_t *d = n->data;
            for (uint32_t i = 0; i <= AUDIODSP_BIQUAD_F32_OPT_MIX; ++i) {
                audiodsp_biquad_f32_configure(&d->config,
                    (audiodsp_biquad_f32_option_t)i, n->param[i]);
            }
            audiodsp_biquad_f32_config_finish(&d->config);
            break;
        }
        case AUDIODSP_GRAPH_FBDELAY: {
            graph_fbdelay_t *d = n->data;
            // Only the options a chorus line moves; DELAY_FRAMES (14) would
            // override DELAY_MS, so the first ten and WOW_DEPTH_RAMP_MS.
            for (uint32_t i = 0; i <= AUDIODSP_FEEDBACK_DELAY_OPT_INPUT_PAN; ++i) {
                audiodsp_feedback_delay_configure(&d->config,
                    (audiodsp_feedback_delay_option_t)i, n->param[i]);
            }
            audiodsp_feedback_delay_config_finish(&d->config);
            break;
        }
        case AUDIODSP_GRAPH_PHASER: {
            graph_phaser_t *d = n->data;
            for (uint32_t i = 0; i <= AUDIODSP_ALLPASS_F32_OPT_MIX; ++i) {
                audiodsp_allpass_f32_configure(&d->config,
                    (audiodsp_allpass_f32_option_t)i, n->param[i]);
            }
            audiodsp_allpass_f32_config_finish(&d->config);
            break;
        }
        case AUDIODSP_GRAPH_SAMPLE_HOLD: {
            graph_sh_t *d = n->data;
            // Whole hertz, at most the sample rate: the kernel's ratio is a
            // pair of integers, so a rate it holds is exact for ever.
            float hz = n->param[AUDIODSP_GRAPH_SH_RATE];
            uint32_t rate = 0;
            if (hz >= 0.5f) {
                rate = hz >= (float)graph->sample_rate
                    ? graph->sample_rate : (uint32_t)round_even(hz);
            }
            if (rate != d->rate) {
                d->rate = rate;
                if (rate != 0 &&
                    audiodsp_samplehold_config_set(&d->config, graph->sample_rate, rate)) {
                    audiodsp_samplehold_reset(&d->state, &d->config);
                }
            }
            break;
        }
#if AUDIODSP_GRAPH_HAS_OVERDRIVE
        case AUDIODSP_GRAPH_OVERDRIVE:
            audiodsp_graph_overdrive_configure(n->data, n->param, graph->sample_rate);
            break;
#endif
        case AUDIODSP_GRAPH_ENV: {
            graph_env_t *d = n->data;
            d->a = env_coeff(n->param[AUDIODSP_GRAPH_ENV_ATTACK], 0.35f, graph->sample_rate);
            d->d = env_coeff(n->param[AUDIODSP_GRAPH_ENV_DECAY], 0.25f, graph->sample_rate);
            d->r = env_coeff(n->param[AUDIODSP_GRAPH_ENV_RELEASE], 0.25f, graph->sample_rate);
            break;
        }
        case AUDIODSP_GRAPH_ADSR2: {
            graph_env_t *d = n->data;
            // A NaN time is 0 (one sample): a NaN coefficient would hold the
            // stage forever.
            float t[3] = {
                n->param[AUDIODSP_GRAPH_ADSR2_ATTACK],
                n->param[AUDIODSP_GRAPH_ADSR2_DECAY],
                n->param[AUDIODSP_GRAPH_ADSR2_RELEASE],
            };
            for (uint32_t k = 0; k < 3u; ++k) {
                t[k] = t[k] == t[k] ? t[k] : 0.0f;
            }
            d->a = env_coeff(t[0], 0.35f, graph->sample_rate);
            d->d = env_coeff(t[1], 0.25f, graph->sample_rate);
            d->r = env_coeff(t[2], 0.25f, graph->sample_rate);
            break;
        }
        default:
            break;
    }
}

// Every sample every node writes goes through sat16, so its rounding is the
// runner's hottest arithmetic. Adding 1.5 * 2^23 puts a clamped sample in
// [2^23, 2^24), where a float holds whole numbers only: the add rounds it to
// an integer the way the FPU rounds, nearest with ties to even, and the
// subtract takes it back exactly. That is round_even's answer for every float
// (all 2^32 of them checked against it), in two adds instead of a dozen
// instructions and two branches; on the P4 it is a fifth of a mixer's time.
// It needs float arithmetic done in float (FLT_EVAL_METHOD 0); a target that
// carries floats wider, as x87 does, keeps round_even.
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD == 0
#define GRAPH_SAT16_MAGIC 1
#define GRAPH_ROUND_MAGIC 12582912.0f
#endif

static inline int16_t sat16(float v) {
    if (v > 32767.0f) {
        return 32767;
    }
    if (v < -32768.0f) {
        return -32768;
    }
#ifdef GRAPH_SAT16_MAGIC
    if (!(v == v)) {
        return 0;
    }
    return (int16_t)(int32_t)((v + GRAPH_ROUND_MAGIC) - GRAPH_ROUND_MAGIC);
#else
    return (int16_t)round_even(v);
#endif
}

static void run_env(audiodsp_graph_node_t *n, const int16_t *gate, int16_t *out,
    uint32_t frames) {
    graph_env_t *d = n->data;
    const float sus = n->param[AUDIODSP_GRAPH_ENV_SUSTAIN];
    uint8_t stage = d->stage;
    bool high = d->high;
    float val = d->val;
    for (uint32_t i = 0; i < frames; ++i) {
        if (gate != NULL) {
            bool g = (float)gate[i] > ENV_GATE_THRESHOLD;
            if (g != high) {
                high = g;
                if (g) {
                    stage = ENV_ATTACK;
                } else if (stage != ENV_IDLE) {
                    stage = ENV_RELEASE;
                }
            }
        }
        switch (stage) {
            case ENV_ATTACK:
                val = ENV_ATTACK_TARGET + (val - ENV_ATTACK_TARGET) * d->a;
                if (val >= 1.0f) {
                    val = 1.0f;
                    stage = ENV_DECAY;
                }
                break;
            case ENV_DECAY:
                val = sus + (val - sus) * d->d;
                if (fabsf(val - sus) <= ENV_DECAY_TOLERANCE) {
                    val = sus;
                    stage = ENV_SUSTAIN;
                }
                break;
            case ENV_SUSTAIN:
                val = sus;
                break;
            case ENV_RELEASE:
                val *= d->r;
                if (val <= ENV_RELEASE_TOLERANCE) {
                    val = 0.0f;
                    stage = ENV_IDLE;
                }
                break;
            default:
                val = 0.0f;
                break;
        }
        out[i] = sat16(val * AUDIODSP_GRAPH_UNIT);
    }
    d->stage = stage;
    d->high = high;
    d->val = val;
}

// ENV's contour remapped to start at initial and peak at attack_level:
// out = (initial + e * (attack_level - initial)) * UNIT. The state machine is
// ENV's, so the coefficients and the stage edges are the same; only the write
// differs. poly80's filter envelope is this shape with sustain set to
// clamp(-initial / (attack_level - initial), 0, 1), which puts the sustain
// step at 0.
static void run_adsr2(audiodsp_graph_node_t *n, const int16_t *gate, int16_t *out,
    uint32_t frames) {
    graph_env_t *d = n->data;
    // Held to 0..1, a NaN to 0: a NaN level never reaches the release's
    // floor, so the voice would never go idle.
    float sus = n->param[AUDIODSP_GRAPH_ADSR2_SUSTAIN];
    sus = !(sus >= 0.0f) ? 0.0f : (sus > 1.0f ? 1.0f : sus);
    const float il = n->param[AUDIODSP_GRAPH_ADSR2_INITIAL];
    const float span = n->param[AUDIODSP_GRAPH_ADSR2_ATTACK_LEVEL] - il;
    uint8_t stage = d->stage;
    bool high = d->high;
    float val = d->val;
    for (uint32_t i = 0; i < frames; ++i) {
        if (gate != NULL) {
            bool g = (float)gate[i] > ENV_GATE_THRESHOLD;
            if (g != high) {
                high = g;
                if (g) {
                    stage = ENV_ATTACK;
                } else if (stage != ENV_IDLE) {
                    stage = ENV_RELEASE;
                }
            }
        }
        switch (stage) {
            case ENV_ATTACK:
                val = ENV_ATTACK_TARGET + (val - ENV_ATTACK_TARGET) * d->a;
                if (val >= 1.0f) {
                    val = 1.0f;
                    stage = ENV_DECAY;
                }
                break;
            case ENV_DECAY:
                val = sus + (val - sus) * d->d;
                if (fabsf(val - sus) <= ENV_DECAY_TOLERANCE) {
                    val = sus;
                    stage = ENV_SUSTAIN;
                }
                break;
            case ENV_SUSTAIN:
                val = sus;
                break;
            case ENV_RELEASE:
                val *= d->r;
                if (val <= ENV_RELEASE_TOLERANCE) {
                    val = 0.0f;
                    stage = ENV_IDLE;
                }
                break;
            default:
                val = 0.0f;
                break;
        }
        out[i] = sat16((il + val * span) * AUDIODSP_GRAPH_UNIT);
    }
    d->stage = stage;
    d->high = high;
    d->val = val;
}

// sin(2 pi p) for a phase in [0, 1), in float and without libm: an odd
// polynomial (Taylor to x^11) over the quarter cycle each phase folds into.
// One polynomial in one order is one function on every target, where sinf()
// is a different one on each libm. Worst error about 6e-8, under a hundredth
// of a count at full scale.
static float lfo_sine(float phase) {
    float t = phase;
    if (t >= 0.75f) {
        t -= 1.0f;
    } else if (t >= 0.25f) {
        t = 0.5f - t;
    }
    const float x = t * 6.28318530717958648f;
    const float x2 = x * x;
    float r = -2.50521083854417188e-8f;     // -1/11!
    r = r * x2 + 2.75573192239858907e-6f;   //  1/9!
    r = r * x2 - 1.98412698412698413e-4f;   // -1/7!
    r = r * x2 + 8.33333333333333333e-3f;   //  1/5!
    r = r * x2 - 1.66666666666666667e-1f;   // -1/3!
    r = r * x2 + 1.0f;
    return r * x;
}

static uint32_t xorshift(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static void run_lfo(audiodsp_graph_node_t *n, int16_t *out, uint32_t frames,
    uint32_t sample_rate) {
    graph_lfo_t *d = n->data;
    const float dt = n->param[AUDIODSP_GRAPH_LFO_RATE] / (float)sample_rate;
    const float amp = n->param[AUDIODSP_GRAPH_LFO_AMPLITUDE];
    // The nearest shape; anything out of range is the triangle.
    const float pick = n->param[AUDIODSP_GRAPH_LFO_SHAPE] + 0.5f;
    const uint8_t shape = pick >= 1.0f && pick < (float)AUDIODSP_GRAPH_LFO_SHAPES
        ? (uint8_t)pick : (uint8_t)AUDIODSP_GRAPH_LFO_TRIANGLE;
    float phase = d->phase;
    switch (shape) {
        case AUDIODSP_GRAPH_LFO_SINE:
            for (uint32_t i = 0; i < frames; ++i) {
                phase += dt;
                phase -= floorf(phase);
                out[i] = sat16(lfo_sine(phase) * amp);
            }
            break;
        case AUDIODSP_GRAPH_LFO_SAW:
            for (uint32_t i = 0; i < frames; ++i) {
                phase += dt;
                phase -= floorf(phase);
                out[i] = sat16((1.0f - 2.0f * phase) * amp);
            }
            break;
        case AUDIODSP_GRAPH_LFO_SQUARE:
            for (uint32_t i = 0; i < frames; ++i) {
                phase += dt;
                phase -= floorf(phase);
                out[i] = sat16(phase < 0.5f ? amp : -amp);
            }
            break;
        case AUDIODSP_GRAPH_LFO_RANDOM: {
            // A shape just chosen starts with the square low, so the first
            // sample of a high half draws a level at once (Mono101LFO's
            // switch to its random wave).
            bool high = d->shape == AUDIODSP_GRAPH_LFO_RANDOM ? d->high : false;
            float level = d->level;
            uint32_t x = d->rng;
            for (uint32_t i = 0; i < frames; ++i) {
                phase += dt;
                phase -= floorf(phase);
                const bool up = phase < 0.5f;
                if (up && !high) {
                    x = xorshift(x);
                    level = (float)(int32_t)x * (1.0f / 2147483648.0f);
                }
                high = up;
                out[i] = sat16(level * amp);
            }
            d->high = high;
            d->level = level;
            d->rng = x;
            break;
        }
        default:
            for (uint32_t i = 0; i < frames; ++i) {
                phase += dt;
                phase -= floorf(phase);
                // 1 - |4p - 2|
                float tri = 4.0f * phase - 2.0f;
                tri = 1.0f - (tri < 0.0f ? -tri : tri);
                out[i] = sat16(tri * amp);
            }
            break;
    }
    d->phase = phase;
    d->shape = shape;
}

static void run_lag(const audiodsp_graph_t *graph, audiodsp_graph_node_t *n,
    int16_t *out, uint32_t frames) {
    graph_lag_t *d = n->data;
    const int16_t *note = n->inputs[0] == AUDIODSP_GRAPH_NONE
        ? NULL : graph->bufs[n->inputs[0]];
    const int16_t *pitch = n->ninputs > 1 && n->inputs[1] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[1]] : NULL;
    const int16_t *legato = n->ninputs > 2 && n->inputs[2] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[2]] : NULL;
    const float time = n->param[AUDIODSP_GRAPH_LAG_TIME];
    const float target = note != NULL ? (float)note[0] : 0.0f;
    float acc[AUDIODSP_GRAPH_MAX_FRAMES];
    if (n->param[AUDIODSP_GRAPH_LAG_SHAPE] >= 0.5f) {
        // dsp/utility.py's GlideCore: a one-pole toward the block's first
        // sample, or the input as it is for a time of a millisecond or less.
        if (!(time > 0.001f)) {
            for (uint32_t i = 0; i < frames; ++i) {
                acc[i] = note != NULL ? (float)note[i] : 0.0f;
            }
            d->pole = acc[frames - 1];
        } else {
            const float c = expf(-1.0f / (time * (float)graph->sample_rate * 0.35f));
            float v = d->pole;
            for (uint32_t i = 0; i < frames; ++i) {
                v = target + (v - target) * c;
                acc[i] = v;
            }
            d->pole = v;
        }
    } else {
        // dsp/glide.py's GlideCore: a straight line to each new note that
        // takes `time` seconds wherever it starts from.
        if (!d->primed) {
            d->value = d->target = target;
            d->primed = true;
        }
        if (target != d->target) {
            d->target = target;
            const float mode = n->param[AUDIODSP_GRAPH_LAG_MODE];
            const bool on = mode >= 0.5f && mode < 1.5f;
            const bool aut = mode >= 1.5f;
            const bool held = legato != NULL &&
                (float)legato[0] > 0.5f * AUDIODSP_GRAPH_UNIT;
            const double span = (double)time * (double)graph->sample_rate;
            const uint32_t steps = span >= 1.0 && span < 4294967295.0
                ? (uint32_t)span : (span >= 1.0 ? 4294967295u : 0u);
            if (steps >= 1u && (on || (aut && held))) {
                d->left = steps;
                // In double, rounded once: the slope GlideCore computes.
                d->slope = (float)(((double)target - (double)d->value) / (double)steps);
            } else {
                d->left = 0;
                d->value = target;
            }
        }
        if (d->left > 0u) {
            const uint32_t k = d->left > frames ? frames : d->left;
            const float start = d->value;
            const float slope = d->slope;
            float last = start;
            for (uint32_t i = 0; i < k; ++i) {
                last = (float)(i + 1u) * slope + start;
                acc[i] = last;
            }
            d->left -= k;
            if (d->left == 0u) {
                last = target;
                acc[k - 1u] = target;
            }
            d->value = last;
            for (uint32_t i = k; i < frames; ++i) {
                acc[i] = target;
            }
        } else {
            for (uint32_t i = 0; i < frames; ++i) {
                acc[i] = d->value;
            }
        }
    }
    if (pitch != NULL) {
        // The bend rides on top unslid: + pitch - note, in that order.
        for (uint32_t i = 0; i < frames; ++i) {
            float v = acc[i] + (float)pitch[i];
            out[i] = sat16(v - (note != NULL ? (float)note[i] : 0.0f));
        }
    } else {
        for (uint32_t i = 0; i < frames; ++i) {
            out[i] = sat16(acc[i]);
        }
    }
}

// LAG's linear glide, then once a block the last glided sample is rounded to
// the step grid and that one value is written across the block. poly80's
// glissando: the glide itself stays unquantised, so the next block starts from
// where the sweep really was and steps again. A semitone is UNIT/12, so the
// grid is step * UNIT / 12 counts. The bend still rides on top unslid.
static void run_gliss(const audiodsp_graph_t *graph, audiodsp_graph_node_t *n,
    int16_t *out, uint32_t frames) {
    graph_lag_t *d = n->data;
    const int16_t *note = n->inputs[0] == AUDIODSP_GRAPH_NONE
        ? NULL : graph->bufs[n->inputs[0]];
    const int16_t *pitch = n->ninputs > 1 && n->inputs[1] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[1]] : NULL;
    const int16_t *legato = n->ninputs > 2 && n->inputs[2] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[2]] : NULL;
    const float time = n->param[AUDIODSP_GRAPH_GLISS_TIME];
    const float target = note != NULL ? (float)note[0] : 0.0f;
    float acc[AUDIODSP_GRAPH_MAX_FRAMES];
    if (!d->primed) {
        d->value = d->target = target;
        d->primed = true;
    }
    if (target != d->target) {
        d->target = target;
        const float mode = n->param[AUDIODSP_GRAPH_GLISS_MODE];
        const bool on = mode >= 0.5f && mode < 1.5f;
        const bool aut = mode >= 1.5f;
        const bool held = legato != NULL &&
            (float)legato[0] > 0.5f * AUDIODSP_GRAPH_UNIT;
        const double span = (double)time * (double)graph->sample_rate;
        const uint32_t steps = span >= 1.0 && span < 4294967295.0
            ? (uint32_t)span : (span >= 1.0 ? 4294967295u : 0u);
        if (steps >= 1u && (on || (aut && held))) {
            d->left = steps;
            // In double, rounded once: the slope GlideCore computes.
            d->slope = (float)(((double)target - (double)d->value) / (double)steps);
        } else {
            d->left = 0;
            d->value = target;
        }
    }
    if (d->left > 0u) {
        const uint32_t k = d->left > frames ? frames : d->left;
        const float start = d->value;
        const float slope = d->slope;
        float last = start;
        for (uint32_t i = 0; i < k; ++i) {
            last = (float)(i + 1u) * slope + start;
            acc[i] = last;
        }
        d->left -= k;
        if (d->left == 0u) {
            last = target;
            acc[k - 1u] = target;
        }
        d->value = last;
        for (uint32_t i = k; i < frames; ++i) {
            acc[i] = target;
        }
    } else {
        for (uint32_t i = 0; i < frames; ++i) {
            acc[i] = d->value;
        }
    }
    // Once a block: round the last glided sample onto the step grid and write
    // that one value across the block. The glide state above is untouched.
    const float step = n->param[AUDIODSP_GRAPH_GLISS_STEP];
    float held = acc[frames - 1u];
    if (step > 0.0f && step <= 1.0e6f) {
        // An infinite step would make the grid inf and the value NaN, and a
        // tiny one a quotient past int32_t. From 2^23 a float quotient is
        // already whole, so it needs no rounding.
        const float grid = step * AUDIODSP_GRAPH_UNIT / 12.0f;
        const float q = held / grid;
        if (q > -8388608.0f && q < 8388608.0f) {
            held = (float)round_even(q) * grid;
        }
    }
    if (pitch != NULL) {
        // The bend rides on top unslid: + pitch - note, in that order.
        for (uint32_t i = 0; i < frames; ++i) {
            float v = held + (float)pitch[i];
            out[i] = sat16(v - (note != NULL ? (float)note[i] : 0.0f));
        }
    } else {
        for (uint32_t i = 0; i < frames; ++i) {
            out[i] = sat16(held);
        }
    }
}

// Once a block the block's level is followed by a one-pole, and that one
// value is written across the block. Peak is clavichord's auto-wah (the
// block's peak absolute sample, moved toward by 0.4 up and 0.06 down);
// RMS is vocoder330's band follower (about the block's own mean, moved
// toward by 1 - exp(-dt/t)). Both are the same one-pole: the coefficient
// here is exp(-dt/t), so clavichord's per-block 0.4 and 0.06 are times of
// -dt/ln(0.6) and -dt/ln(0.94) at its block size.
static double graph_exp2d(double y);

static void run_follow(const audiodsp_graph_t *graph, audiodsp_graph_node_t *n,
    int16_t *out, uint32_t frames) {
    graph_follow_t *d = n->data;
    const int16_t *in = n->inputs[0] == AUDIODSP_GRAPH_NONE
        ? NULL : graph->bufs[n->inputs[0]];
    float level = 0.0f;
    if (in != NULL && frames > 0u) {
        if (n->param[AUDIODSP_GRAPH_FOLLOW_MODE] >= 0.5f) {
            double sum = 0.0;
            double sq = 0.0;
            for (uint32_t i = 0; i < frames; ++i) {
                double v = (double)in[i];
                sum += v;
                sq += v * v;
            }
            const double mean = sum / (double)frames;
            const double var = sq / (double)frames - mean * mean;
            level = var > 0.0 ? (float)sqrt(var) : 0.0f;
        } else {
            float peak = 0.0f;
            for (uint32_t i = 0; i < frames; ++i) {
                const float a = fabsf((float)in[i]);
                if (a > peak) {
                    peak = a;
                }
            }
            level = peak;
        }
    }
    const double dt = (double)frames / (double)graph->sample_rate;
    const double t = level > d->env
        ? (double)n->param[AUDIODSP_GRAPH_FOLLOW_ATTACK]
        : (double)n->param[AUDIODSP_GRAPH_FOLLOW_RELEASE];
    double coef = 0.0;
    if (t > 0.0) {
        // In double, as the rack's math.exp is: the coefficient vocoder330
        // computes from its times. Through graph_exp2d, not libm's exp,
        // so every target computes the same coefficient.
        coef = graph_exp2d(-dt / t * 1.44269504088896340736);
    }
    d->env = (float)((double)level + ((double)d->env - (double)level) * coef);
    for (uint32_t i = 0; i < frames; ++i) {
        out[i] = sat16(d->env);
    }
}

static void run_sample_hold(const audiodsp_graph_t *graph,
    audiodsp_graph_node_t *n, int16_t *out, uint32_t frames) {
    graph_sh_t *d = n->data;
    const int16_t *signal = n->inputs[0] == AUDIODSP_GRAPH_NONE
        ? NULL : graph->bufs[n->inputs[0]];
    const int16_t *clock = n->ninputs > 1 && n->inputs[1] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[1]] : NULL;
    float lag = n->param[AUDIODSP_GRAPH_SH_LAG];
    if (!(lag > 0.0f)) {
        lag = 0.0f;
    } else if (lag > 0.999999f) {
        lag = 0.999999f;
    }
    int16_t staircase[AUDIODSP_GRAPH_MAX_FRAMES];
    float held = d->held;
    if (clock != NULL) {
        // Every rising edge latches the signal's sample at that edge: the
        // clock above 0.1 on the wire is high, as an envelope's gate is.
        bool high = d->high;
        for (uint32_t i = 0; i < frames; ++i) {
            const bool g = (float)clock[i] > ENV_GATE_THRESHOLD;
            if (g && !high) {
                held = signal != NULL ? (float)signal[i] : 0.0f;
            }
            high = g;
            staircase[i] = (int16_t)held;
        }
        d->high = high;
    } else if (d->rate != 0u && signal != NULL) {
        audiodsp_samplehold_process(&d->config, &d->state,
            (uint8_t *)staircase, (const uint8_t *)signal, frames,
            sizeof(int16_t));
        held = (float)staircase[frames - 1];
    } else {
        for (uint32_t i = 0; i < frames; ++i) {
            staircase[i] = (int16_t)held;
        }
    }
    d->held = held;
    if (lag == 0.0f) {
        memcpy(out, staircase, frames * sizeof(int16_t));
        d->out = held;
        return;
    }
    // out slews toward what is held with a one-pole of coefficient `lag`.
    float v = d->out;
    for (uint32_t i = 0; i < frames; ++i) {
        const float h = (float)staircase[i];
        v = h + (v - h) * lag;
        out[i] = sat16(v);
    }
    d->out = v;
}

static void run_noise(audiodsp_graph_node_t *n, int16_t *out, uint32_t frames) {
    graph_noise_t *d = n->data;
    const float amp = n->param[0];
    uint32_t x = d->rng;
    for (uint32_t i = 0; i < frames; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        // uniform in -1..1
        float u = (float)(int32_t)x * (1.0f / 2147483648.0f);
        out[i] = sat16(u * amp);
    }
    d->rng = x;
}

// One input into a mixer's sum at gain g1: ramped across the block from the
// gain it had (*now) when that moved, skipped at a gain of 0 that did not.
static void mix_input(float *acc, const int16_t *in, float *now, float g1,
    uint32_t frames) {
    const float g0 = *now;
    *now = g1;
    if (g0 == g1) {
        if (g1 == 0.0f) {
            return;
        }
        for (uint32_t i = 0; i < frames; ++i) {
            acc[i] += g1 * (float)in[i];
        }
    } else {
        const float step = (g1 - g0) * (1.0f / (float)frames);
        float g = g0;
        for (uint32_t i = 0; i < frames; ++i) {
            g += step;
            acc[i] += g * (float)in[i];
        }
    }
}

static void run_mix(const audiodsp_graph_t *graph, audiodsp_graph_node_t *n,
    int16_t *out, uint32_t frames, float *acc) {
    graph_mix_t *d = n->data;
    const float offset = n->param[AUDIODSP_GRAPH_MIX_OFFSET];
    if (!d->primed) {
        for (uint32_t k = 0; k < AUDIODSP_GRAPH_MAX_INPUTS; ++k) {
            d->gain_now[k] = n->param[k];
        }
        d->primed = true;
    }
    for (uint32_t i = 0; i < frames; ++i) {
        acc[i] = offset;
    }
    for (uint32_t k = 0; k < n->ninputs; ++k) {
        if (n->inputs[k] != AUDIODSP_GRAPH_NONE) {
            mix_input(acc, graph->bufs[n->inputs[k]], &d->gain_now[k], n->param[k], frames);
        }
    }
    for (uint32_t i = 0; i < frames; ++i) {
        out[i] = sat16(acc[i]);
    }
}

static void run_pan(const audiodsp_graph_t *graph, audiodsp_graph_node_t *n,
    int16_t *out, uint32_t frames, float *acc) {
    graph_pan_t *d = n->data;
    float left[AUDIODSP_GRAPH_MAX_INPUTS];
    float right[AUDIODSP_GRAPH_MAX_INPUTS];
    for (uint32_t k = 0; k < AUDIODSP_GRAPH_MAX_INPUTS; ++k) {
        const float g = n->param[k];
        float pan = n->param[AUDIODSP_GRAPH_PAN_PAN + k];
        pan = pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);
        left[k] = pan > 0.0f ? g * (1.0f - pan) : g;
        right[k] = pan < 0.0f ? g * (1.0f + pan) : g;
    }
    if (!d->primed) {
        for (uint32_t k = 0; k < AUDIODSP_GRAPH_MAX_INPUTS; ++k) {
            d->left_now[k] = left[k];
            d->right_now[k] = right[k];
        }
        d->primed = true;
    }
    for (int side = 0; side < 2; ++side) {
        float *now = side ? d->right_now : d->left_now;
        const float *gain = side ? right : left;
        int16_t *dst = side ? graph->bufs[d->right] : out;
        for (uint32_t i = 0; i < frames; ++i) {
            acc[i] = 0.0f;
        }
        for (uint32_t k = 0; k < n->ninputs; ++k) {
            if (n->inputs[k] != AUDIODSP_GRAPH_NONE) {
                mix_input(acc, graph->bufs[n->inputs[k]], &now[k], gain[k], frames);
            }
        }
        for (uint32_t i = 0; i < frames; ++i) {
            dst[i] = sat16(acc[i]);
        }
    }
}

static void run_vca(const audiodsp_graph_t *graph, audiodsp_graph_node_t *n,
    int16_t *out, uint32_t frames) {
    const int16_t *audio = n->inputs[0] == AUDIODSP_GRAPH_NONE
        ? NULL : graph->bufs[n->inputs[0]];
    const uint32_t which = n->param[AUDIODSP_GRAPH_VCA_SELECT] < 0.5f ? 1u : 2u;
    const int16_t *cv = n->inputs[which] == AUDIODSP_GRAPH_NONE
        ? NULL : graph->bufs[n->inputs[which]];
    const float k = n->param[AUDIODSP_GRAPH_VCA_GAIN] / AUDIODSP_GRAPH_UNIT;
    if (audio == NULL || cv == NULL) {
        memset(out, 0, frames * sizeof(int16_t));
        return;
    }
    if (n->param[AUDIODSP_GRAPH_VCA_CURVE] >= 0.5f) {
        // 10^(3 * (x - 1)) = e^(3 ln 10 * (x - 1))
        const float a = 6.907755279f / AUDIODSP_GRAPH_UNIT;
        const float g = n->param[AUDIODSP_GRAPH_VCA_GAIN];
        for (uint32_t i = 0; i < frames; ++i) {
            out[i] = sat16((float)audio[i] * expf(a * (float)cv[i] - 6.907755279f) * g);
        }
        return;
    }
    for (uint32_t i = 0; i < frames; ++i) {
        out[i] = sat16((float)audio[i] * (float)cv[i] * k);
    }
}

static void run_out(const audiodsp_graph_t *graph, audiodsp_graph_node_t *n,
    int16_t *out, uint32_t frames) {
    if (out == NULL) {
        return;
    }
    const int16_t *left = n->inputs[0] == AUDIODSP_GRAPH_NONE
        ? NULL : graph->bufs[n->inputs[0]];
    const int16_t *right = n->ninputs > 1 && n->inputs[1] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[1]] : left;
    const float scale = n->param[0];
    if (left == NULL) {
        memset(out, 0, (size_t)frames * graph->channel_count * sizeof(int16_t));
        return;
    }
    if (graph->channel_count == 2) {
        for (uint32_t i = 0; i < frames; ++i) {
            out[2 * i] = sat16((float)left[i] * scale);
            out[2 * i + 1] = sat16((float)right[i] * scale);
        }
    } else {
        for (uint32_t i = 0; i < frames; ++i) {
            out[i] = sat16(((float)left[i] + (float)right[i]) * 0.5f * scale);
        }
    }
}


// ----------------------------------------------------------- WAVEFOLD --

// sin(y) for any float y, without libm: y in cycles, folded into [0, 1), then
// the LFO's polynomial. The product y / 2 pi is rounded once, so the angle
// carries an error of a float ulp of the turns, about 1e-6 radians at the
// largest fold: hundredths of a count on the wire.
static float graph_sin(float y) {
    if (!(y == y)) {
        return 0.0f;
    }
    y = y > 1.0e6f ? 1.0e6f : (y < -1.0e6f ? -1.0e6f : y);
    float p = y * 0.159154943091895336f;
    float whole = (float)(int32_t)p;
    if (whole > p) {
        whole -= 1.0f;
    }
    p -= whole;
    return lfo_sine(p >= 1.0f ? 0.0f : p);
}

// WavefolderCore.render, operation for operation in float32 as numpy does it:
// the drive 1 + 7f, f1 = sin(x pi/2), f2 = sin((1.5 f1 + 0.5 x) pi/2),
// f3 = sin((1.3 f2 + 0.4 f1) pi/2), and the dry signal crossfaded with half
// of f3 by the fold f.
static void run_wavefold(const audiodsp_graph_t *graph, audiodsp_graph_node_t *n,
    int16_t *out, uint32_t frames) {
    const int16_t *in = n->inputs[0] == AUDIODSP_GRAPH_NONE
        ? NULL : graph->bufs[n->inputs[0]];
    const int16_t *cv = (n->ninputs > 1 && n->inputs[1] != AUDIODSP_GRAPH_NONE)
        ? graph->bufs[n->inputs[1]] : NULL;
    const float fold = n->param[AUDIODSP_GRAPH_WAVEFOLD_FOLD];
    if (fold <= 0.001f && cv == NULL) {
        if (in == NULL) {
            memset(out, 0, frames * sizeof(int16_t));
        } else if (in != out) {
            memcpy(out, in, frames * sizeof(int16_t));
        }
        return;
    }
    const float half_pi = 1.57079632679489662f;
    const float inv = 1.0f / AUDIODSP_GRAPH_UNIT;
    const float offset = n->param[AUDIODSP_GRAPH_WAVEFOLD_SYMMETRY] * 0.25f;
    for (uint32_t i = 0; i < frames; ++i) {
        const float src = in != NULL ? (float)in[i] * inv : 0.0f;
        float f = fold;
        if (cv != NULL) {
            f += (float)cv[i] * inv;
        }
        float x = src + offset;
        x *= f * 7.0f + 1.0f;
        const float f1 = graph_sin(x * half_pi);
        const float f2 = graph_sin((f1 * 1.5f + x * 0.5f) * half_pi);
        const float f3 = graph_sin((f2 * 1.3f + f1 * 0.4f) * half_pi) * 0.5f;
        out[i] = sat16((src * (-f + 1.0f) + f3 * f) * AUDIODSP_GRAPH_UNIT);
    }
}

// ------------------------------------------------------------ TWOPOLE --

// tanh, the same on every target: libm's differs in the last place between
// glibc, newlib and MicroPython's own. A rational function (an odd degree-13
// polynomial over an even degree-6 one in x*x, Horner, no fused multiply-add)
// good to a few float ulps, which is under a hundredth of a count at full
// scale. Beyond 7.9 it has reached 1 to the last bit.
static float graph_tanh(float x) {
    x = x > 7.90531110763549805f ? 7.90531110763549805f : x;
    x = x < -7.90531110763549805f ? -7.90531110763549805f : x;
    const float x2 = x * x;
    float p = -2.76076847742355e-16f;
    p = p * x2 + 2.00018790482477e-13f;
    p = p * x2 + -8.60467152213735e-11f;
    p = p * x2 + 5.12229709037114e-08f;
    p = p * x2 + 1.48572235717979e-05f;
    p = p * x2 + 6.37261928875436e-04f;
    p = p * x2 + 4.89352455891786e-03f;
    p = x * p;
    float q = 1.19825839466702e-06f;
    q = q * x2 + 1.18534705686654e-04f;
    q = q * x2 + 2.26843463243900e-03f;
    q = q * x2 + 4.89352518554385e-03f;
    return p / q;
}

static void run_tanh(const audiodsp_graph_t *graph, audiodsp_graph_node_t *n,
    int16_t *out, uint32_t frames) {
    const int16_t *in = n->inputs[0] == AUDIODSP_GRAPH_NONE
        ? NULL : graph->bufs[n->inputs[0]];
    if (in == NULL) {
        memset(out, 0, frames * sizeof(int16_t));
        return;
    }
    const float drive = n->param[AUDIODSP_GRAPH_TANH_DRIVE] / AUDIODSP_GRAPH_UNIT;
    const float level = n->param[AUDIODSP_GRAPH_TANH_LEVEL] * AUDIODSP_GRAPH_UNIT;
    for (uint32_t i = 0; i < frames; ++i) {
        out[i] = sat16(graph_tanh((float)in[i] * drive) * level);
    }
}

// 2^y in double without libm, for the cutoff: the integer part into the
// scale, the fraction (within half) through a Taylor series to x^14 of
// e^(f ln 2), good to about 1e-19. Once or twice a block, so it stays double.
static double graph_exp2d(double y) {
    y = y > 60.0 ? 60.0 : (y < -60.0 ? -60.0 : y);
    const int32_t whole = y >= 0.0 ? (int32_t)(y + 0.5) : -(int32_t)(0.5 - y);
    const double x = (y - (double)whole) * 0.693147180559945309417232121458;
    double r = 1.0 / 87178291200.0;       // 1/14!
    r = r * x + 1.0 / 6227020800.0;       // 1/13!
    r = r * x + 1.0 / 479001600.0;        // 1/12!
    r = r * x + 1.0 / 39916800.0;         // 1/11!
    r = r * x + 1.0 / 3628800.0;          // 1/10!
    r = r * x + 1.0 / 362880.0;           // 1/9!
    r = r * x + 1.0 / 40320.0;            // 1/8!
    r = r * x + 1.0 / 5040.0;             // 1/7!
    r = r * x + 1.0 / 720.0;              // 1/6!
    r = r * x + 1.0 / 120.0;              // 1/5!
    r = r * x + 1.0 / 24.0;               // 1/4!
    r = r * x + 1.0 / 6.0;                // 1/3!
    r = r * x + 0.5;
    r = r * x + 1.0;
    r = r * x + 1.0;
    double scale = 1.0;
    double step = whole >= 0 ? 2.0 : 0.5;
    uint32_t k = (uint32_t)(whole >= 0 ? whole : -whole);
    while (k) {
        if (k & 1u) {
            scale *= step;
        }
        step *= step;
        k >>= 1;
    }
    return r * scale;
}

// min(tan(theta), 0.95) in double without libm, for the filter coefficient
// g = tan(pi fc / sr). tan reaches 0.95 at 0.7595 rad, so a larger angle is
// the clamp and a smaller one takes the sine and cosine series to x^17 / x^16
// (about 1e-19 at 0.77).
static double graph_tan_clamped(double theta) {
    if (!(theta < 0.77)) {
        return 0.95;
    }
    const double x2 = theta * theta;
    double s = 1.0 / 355687428096000.0;   // 1/17!
    s = s * x2 - 1.0 / 1307674368000.0;   // 1/15!
    s = s * x2 + 1.0 / 6227020800.0;      // 1/13!
    s = s * x2 - 1.0 / 39916800.0;        // 1/11!
    s = s * x2 + 1.0 / 362880.0;          // 1/9!
    s = s * x2 - 1.0 / 5040.0;            // 1/7!
    s = s * x2 + 1.0 / 120.0;             // 1/5!
    s = s * x2 - 1.0 / 6.0;               // 1/3!
    s = s * x2 + 1.0;
    s *= theta;
    double c = 1.0 / 20922789888000.0;    // 1/16!
    c = c * x2 - 1.0 / 87178291200.0;     // 1/14!
    c = c * x2 + 1.0 / 479001600.0;       // 1/12!
    c = c * x2 - 1.0 / 3628800.0;         // 1/10!
    c = c * x2 + 1.0 / 40320.0;           // 1/8!
    c = c * x2 - 1.0 / 720.0;             // 1/6!
    c = c * x2 + 1.0 / 24.0;              // 1/4!
    c = c * x2 - 0.5;
    c = c * x2 + 1.0;
    const double t = s / c;
    return t > 0.95 ? 0.95 : t;
}

// dsp/twopole.py's _coefficients() at one end of the block: the cutoff CV
// (counts, UNIT to 1.0) moves cutoff_hz by depth * 10 octaves per unit, held
// between 20 Hz and 0.45 of the sample rate, and g = min(tan(pi fc / sr), 0.95).
//
// vrack reads the CV from a numpy float32 block, so with a CV connected every
// step of that arithmetic stays in float32 (a float32 scalar beats a Python
// float): the product with the depth, the times ten, the power of two, the
// cutoff, and the angle. Done in double instead, g differs in the seventh
// digit, and these filters clip hard enough in the loop to turn that into
// several counts. So this does the same float32 steps, with the power of two
// from the double series above (powf's value to the float) and the tangent
// from double, as math.tan is. With no CV connected it is all double, as there.
static double twopole_g(float cutoff_hz, bool has_cv, int32_t cv, float depth,
    uint32_t sample_rate) {
    const double rate = (double)sample_rate;
    const double top = rate * 0.45;
    double fc_d = (double)cutoff_hz;
    float fc_f = 0.0f;
    bool is_float = false;
    if (has_cv) {
        float t = (float)cv * (1.0f / 16384.0f);
        t = t * depth;
        t = t * 10.0f;
        fc_f = cutoff_hz * (float)graph_exp2d((double)t);
        fc_d = (double)fc_f;
        is_float = true;
    }
    if (fc_d > top) {
        fc_d = top;
        is_float = false;
    }
    if (!(fc_d > 20.0)) {
        fc_d = 20.0;
        is_float = false;
    }
    if (is_float) {
        const float a = 3.14159274f * fc_f;
        const float theta = a * (float)(1.0 / rate);
        return graph_tan_clamped((double)theta);
    }
    return graph_tan_clamped(3.14159265358979323846 * fc_d * (1.0 / rate));
}

// _diode_clipper(): tanh(1.5 v) / 1.5.
static inline float twopole_diode(float v) {
    return graph_tanh(v * 1.5f) / 1.5f;
}

// _cmos_clip(): v (1.2 - 0.2 v^2), held to +-1.
static inline float twopole_cmos(float v) {
    const float w = v * (1.2f - 0.2f * (v * v));
    return w > 1.0f ? 1.0f : (w < -1.0f ? -1.0f : w);
}

static void run_twopole(const audiodsp_graph_t *graph, audiodsp_graph_node_t *n,
    int16_t *out, uint32_t frames) {
    graph_twopole_t *d = n->data;
    const int16_t *in = n->inputs[0] == AUDIODSP_GRAPH_NONE
        ? NULL : graph->bufs[n->inputs[0]];
    const int16_t *cv = n->ninputs > 1 && n->inputs[1] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[1]] : NULL;
    // The nearest type; anything out of range is the Sallen-Key low-pass.
    const float pick = n->param[AUDIODSP_GRAPH_TWOPOLE_TYPE] + 0.5f;
    const uint32_t type = pick >= 1.0f && pick < (float)AUDIODSP_GRAPH_TWOPOLE_TYPES
        ? (uint32_t)pick : (uint32_t)AUDIODSP_GRAPH_TWOPOLE_SK_LOWPASS;
    const float cutoff = n->param[AUDIODSP_GRAPH_TWOPOLE_CUTOFF_HZ];
    const float depth = n->param[AUDIODSP_GRAPH_TWOPOLE_DEPTH];
    const double res = (double)n->param[AUDIODSP_GRAPH_TWOPOLE_RESONANCE];
    const float drive = n->param[AUDIODSP_GRAPH_TWOPOLE_DRIVE];
    // The cutoff CV is read at the block's first and last samples; g walks
    // from the first's value, a step a sample, to the last's.
    const int32_t c0 = cv != NULL ? cv[0] : 0;
    const int32_t c1 = cv != NULL ? cv[frames - 1u] : 0;
    const double g0 = twopole_g(cutoff, cv != NULL, c0, depth, graph->sample_rate);
    const bool walks = c0 != c1;
    const float gf0 = (float)g0;
    // The walk is in double, a step added a sample at a time, as
    // dsp/twopole.py does it: in float it drifts by an ulp or two, and
    // these filters turn that into counts.
    double gd = g0;
    double gstep = 0.0;
    if (walks) {
        gstep = (twopole_g(cutoff, true, c1, depth, graph->sample_rate) - g0) /
            (double)frames;
    }
    float s1 = d->s1;
    float s2 = d->s2;
    const float scale = 1.0f / 16384.0f;
    if (type == AUDIODSP_GRAPH_TWOPOLE_SK_LOWPASS ||
        type == AUDIODSP_GRAPH_TWOPOLE_SK_HIGHPASS) {
        const double kd = res * 2.15;
        const float k = (float)kd;
        if (type == AUDIODSP_GRAPH_TWOPOLE_SK_LOWPASS) {
            float h = (float)(g0 / (1.0 + g0));
            for (uint32_t i = 0; i < frames; ++i) {
                const float x = (in != NULL ? (float)in[i] * scale : 0.0f) * drive;
                if (walks) {
                    gd += gstep;
                    h = (float)(gd / (1.0 + gd));
                }
                const float fb = twopole_diode(s2 * k);
                const float v1 = (x + fb - s1) * h;
                const float node1 = v1 + s1;
                s1 = node1 + v1;
                const float v2 = (node1 - s2) * h;
                const float lp = v2 + s2;
                s2 = lp + v2;
                out[i] = sat16(graph_tanh(lp) * 16384.0f);
            }
        } else {
            const float k_term = (float)(2.0 - kd * 0.5);
            float g = gf0;
            float denom = (float)(1.0 / (1.0 + 2.0 * g0 + g0 * g0));
            for (uint32_t i = 0; i < frames; ++i) {
                const float x = (in != NULL ? (float)in[i] * scale : 0.0f) * drive;
                if (walks) {
                    gd += gstep;
                    g = (float)gd;
                    denom = (float)(1.0 / (1.0 + 2.0 * gd + gd * gd));
                }
                const float fb = twopole_diode(s2 * k);
                const float hp = (x + fb - k_term * s1 - s2) * denom;
                const float v1 = g * hp;
                const float node1 = v1 + s1;
                s1 = node1 + v1;
                const float v2 = g * node1;
                const float lp = v2 + s2;
                s2 = lp + v2;
                out[i] = sat16(graph_tanh(hp) * 16384.0f);
            }
        }
    } else {
        double rd = 1.0 - res * 0.96;
        rd = rd < 0.02 ? 0.02 : rd;
        float g = gf0;
        float rg = (float)(rd + g0);
        float denom = (float)(1.0 / (1.0 + g0 * (g0 + rd)));
        for (uint32_t i = 0; i < frames; ++i) {
            const float x = twopole_cmos((in != NULL ? (float)in[i] * scale : 0.0f) * drive);
            if (walks) {
                gd += gstep;
                g = (float)gd;
                rg = (float)(rd + gd);
                denom = (float)(1.0 / (1.0 + gd * (gd + rd)));
            }
            const float hp = twopole_cmos((x - rg * s1 - s2) * denom);
            const float v1 = g * hp;
            const float bp = twopole_cmos(v1 + s1);
            s1 = bp + v1;
            const float v2 = g * bp;
            const float lp = twopole_cmos(v2 + s2);
            s2 = lp + v2;
            const float y = type == AUDIODSP_GRAPH_TWOPOLE_CMOS_LOWPASS ? lp
                : (type == AUDIODSP_GRAPH_TWOPOLE_CMOS_BANDPASS ? bp : hp);
            out[i] = sat16(y * 16384.0f);
        }
    }
    d->s1 = s1;
    d->s2 = s2;
}

// ---------------------------------------------------------------- LPG --

// log2(v) for v > 0 in double, without libm: v brought into [sqrt(1/2),
// sqrt(2)) by powers of two, then ln through the atanh series of
// s = (v - 1) / (v + 1), |s| under 0.172, to s^25 (about 1e-19).
static double graph_log2d(double v) {
    int32_t e = 0;
    while (v < 2.3283064365386963e-10) {   // 2^-32
        v *= 4294967296.0;
        e -= 32;
    }
    while (v < 0.70710678118654752440) {
        v *= 2.0;
        e -= 1;
    }
    while (v >= 1.41421356237309504880) {
        v *= 0.5;
        e += 1;
    }
    const double s = (v - 1.0) / (v + 1.0);
    const double s2 = s * s;
    double r = 1.0 / 25.0;
    r = r * s2 + 1.0 / 23.0;
    r = r * s2 + 1.0 / 21.0;
    r = r * s2 + 1.0 / 19.0;
    r = r * s2 + 1.0 / 17.0;
    r = r * s2 + 1.0 / 15.0;
    r = r * s2 + 1.0 / 13.0;
    r = r * s2 + 1.0 / 11.0;
    r = r * s2 + 1.0 / 9.0;
    r = r * s2 + 1.0 / 7.0;
    r = r * s2 + 1.0 / 5.0;
    r = r * s2 + 1.0 / 3.0;
    r = r * s2 + 1.0;
    return (double)e + 2.0 * s * r * 1.44269504088896340736;
}

#define LPG_LOG2E 1.44269504088896340736
#define LPG_LOG2_400 8.64385618977472469574
#define LPG_Q 0.7071
#define LPG_PI 3.14159265358979323846
#define LPG_MAX_SHIFT 30
#define LPG_STATE_SHIFT 12
#define LPG_STATE_MAX (32767 * (1 << LPG_STATE_SHIFT))
#define LPG_STATE_MIN (-32768 * (1 << LPG_STATE_SHIFT))

// round(ldexp(v, shift)): half away from zero, as C's round() does, clamped
// to an int32_t as synthio's biquad_scale_arg_float clamps it.
static int32_t lpg_scale(double v, int shift) {
    const double r = v * (double)((int64_t)1 << shift);
    int64_t t = (int64_t)r;
    const double frac = r - (double)t;
    if (frac >= 0.5) {
        ++t;
    } else if (frac <= -0.5) {
        --t;
    }
    return t > INT32_MAX ? INT32_MAX : (t < INT32_MIN ? INT32_MIN : (int32_t)t);
}

static double lpg_fabs(double v) {
    return v < 0 ? -v : v;
}

// synthio's biquad_choose_shift: the most fractional bits the largest
// coefficient leaves room for in an int32_t, up to 30. frexp's exponent is
// counted by doubling.
static int lpg_choose_shift(const double *values, int count) {
    double largest = 1;
    for (int i = 0; i < count; i++) {
        if (lpg_fabs(values[i]) > largest) {
            largest = lpg_fabs(values[i]);
        }
    }
    int exponent = 0;
    for (double power = 1; largest >= power; power *= 2) {
        exponent++;
    }
    int shift = 31 - exponent;
    if (shift > LPG_MAX_SHIFT) {
        shift = LPG_MAX_SHIFT;
    }
    return shift < 1 ? 1 : shift;
}

// synthio.Biquad's LOW_PASS coefficients at Q 0.7071, as
// audiodsp_biquad_cp_configure makes them: CircuitPython's reflected
// fast_sincos, its W0 scale and its per-filter shift. A copy here, without
// libm, because the CircuitPython build does not link audiodsp_biquad.c.
static void lpg_configure(graph_lpg_t *d, double cutoff, uint32_t sample_rate) {
    const double reciprocal_sr = 1.0 / sample_rate;
    const double w0 = cutoff * ((2 * LPG_PI) * reciprocal_sr);
    const bool negate_cos = w0 > (LPG_PI / 2);
    double t = negate_cos ? (LPG_PI - w0) : w0;
    const bool swap = t > (LPG_PI / 4);
    if (swap) {
        t = (LPG_PI / 2) - t;
    }
    const double t2 = t * t;
    const double ps = t * (1 + t2 * (-0.16666651 + t2 * (0.0083319786 + t2 * -0.00019495635)));
    const double pc = 1 + t2 * (-0.5 + t2 * (0.041666647 + t2 * (-0.0013887368 + t2 * 0.000024438452)));
    const double s = swap ? pc : ps;
    const double c = negate_cos ? -(swap ? ps : pc) : (swap ? ps : pc);
    const double alpha = s / (2 * LPG_Q);
    const double a0 = 1 + alpha;
    const double reciprocal = 1 / a0;
    const double a1 = -2 * c * reciprocal;
    const double a2 = (1 - alpha) * reciprocal;
    const double b0 = (1 - c) * .5 * reciprocal;
    const double b1 = (1 - c) * reciprocal;
    const double values[] = {a1, a2, b0, b1, b0};
    const int shift = lpg_choose_shift(values, 5);
    d->shift = shift;
    d->a1 = lpg_scale(a1, shift);
    d->a2 = lpg_scale(a2, shift);
    d->b0 = lpg_scale(b0, shift);
    d->b1 = lpg_scale(b1, shift);
    d->b2 = lpg_scale(b0, shift);
    d->cutoff = cutoff;
}

// synthio's biquad_filter_sample: the int64_t sum, rounded to nearest, kept
// with LPG_STATE_SHIFT bits below the sample and clamped to the sample range.
static inline int32_t lpg_filter(const graph_lpg_t *d, int32_t input,
    int32_t x0, int32_t x1, int32_t y0, int32_t y1) {
    int64_t acc = ((int64_t)d->b0 * input + (int64_t)d->b1 * x0
        + (int64_t)d->b2 * x1) * (1 << LPG_STATE_SHIFT)
        - (int64_t)d->a1 * y0 - (int64_t)d->a2 * y1;
    acc = (acc + ((int64_t)1 << (d->shift - 1))) >> d->shift;
    return acc > LPG_STATE_MAX ? LPG_STATE_MAX
        : (acc < LPG_STATE_MIN ? LPG_STATE_MIN : (int32_t)acc);
}

// LowPassGateCore.render. The vactrol steps once a block toward the manual
// level plus the control's and 1.5 times the strike's first samples: up in
// about 2 ms, down with the decay. VCA scales the input by it; VCF runs the
// input through synthio.Biquad's low-pass at 30 * 400^v Hz inside
// audiofilters.Filter, its fixed point and the soft knee past +-28000 included; BOTH does
// that and scales by v^1.3. The level multiplies last.
static void run_lpg(const audiodsp_graph_t *graph, audiodsp_graph_node_t *n,
    int16_t *out, uint32_t frames) {
    graph_lpg_t *d = n->data;
    const int16_t *in = n->inputs[0] == AUDIODSP_GRAPH_NONE
        ? NULL : graph->bufs[n->inputs[0]];
    const int16_t *control = n->ninputs > 1 && n->inputs[1] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[1]] : NULL;
    const int16_t *strike = n->ninputs > 2 && n->inputs[2] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[2]] : NULL;
    const double sr = (double)graph->sample_rate;
    const double inv = 1.0 / (double)AUDIODSP_GRAPH_UNIT;

    double target = (double)n->param[AUDIODSP_GRAPH_LPG_MANUAL];
    if (control != NULL) {
        target += (double)control[0] * inv;
    }
    if (strike != NULL) {
        target += 1.5 * ((double)strike[0] * inv);
    }
    target = target < 0.0 ? 0.0 : (target > 1.0 ? 1.0 : target);
    double vs = d->vactrol;
    if (target > vs) {
        vs += (target - vs) * (1.0 - graph_exp2d(-(double)frames / (0.002 * sr) * LPG_LOG2E));
    } else {
        double decay = (double)n->param[AUDIODSP_GRAPH_LPG_DECAY];
        decay = decay > 0.01 ? decay : 0.01;
        vs *= graph_exp2d(-(double)frames / (decay * sr) * LPG_LOG2E);
    }
    d->vactrol = vs;
    const double v = vs < 0.0 ? 0.0 : (vs > 1.0 ? 1.0 : vs);

    const float pick = n->param[AUDIODSP_GRAPH_LPG_MODE] + 0.5f;
    const uint32_t mode = pick >= 0.0f && pick < 3.0f
        ? (uint32_t)pick : (uint32_t)AUDIODSP_GRAPH_LPG_BOTH;
    const float level = n->param[AUDIODSP_GRAPH_LPG_LEVEL];

    if (mode == AUDIODSP_GRAPH_LPG_VCA) {
        const float g = (float)v;
        for (uint32_t i = 0; i < frames; ++i) {
            const float x = in != NULL ? (float)in[i] * g : 0.0f;
            out[i] = sat16(x * level);
        }
        return;
    }

    double cutoff = 30.0 * graph_exp2d(v * LPG_LOG2_400);
    const double top = sr * 0.45;
    cutoff = cutoff < 10.0 ? 10.0 : (cutoff > top ? top : cutoff);
    if (cutoff != d->cutoff) {
        lpg_configure(d, cutoff, graph->sample_rate);
    }
    float g = 1.0f;
    if (mode == AUDIODSP_GRAPH_LPG_BOTH) {
        if (v <= 0.0) {
            g = 0.0f;
        } else {
            const double e = 1.3 * graph_log2d(v);
            g = e < -60.0 ? 0.0f : (float)graph_exp2d(e);
        }
    }
    const int32_t knee = 0x0fffffff / (32768 * 2 - 28000);
    int32_t x0 = d->x0, x1 = d->x1, y0 = d->y0, y1 = d->y1;
    for (uint32_t i = 0; i < frames; ++i) {
        const int32_t sample = in != NULL ? in[i] : 0;
        const int32_t y = lpg_filter(d, sample, x0, x1, y0, y1);
        x1 = x0;
        x0 = sample;
        y1 = y0;
        y0 = y;
        int32_t w = (y + (1 << (LPG_STATE_SHIFT - 1))) >> LPG_STATE_SHIFT;
        if (w < -28000) {
            w = (((w + 28000) * knee) >> 16) - 28000;
        } else if (w > 28000) {
            w = (((w - 28000) * knee) >> 16) + 28000;
        }
        float o = (float)w;
        if (mode == AUDIODSP_GRAPH_LPG_BOTH) {
            o *= g;
        }
        out[i] = sat16(o * level);
    }
    d->x0 = x0;
    d->x1 = x1;
    d->y0 = y0;
    d->y1 = y1;
}

// -------------------------------------------------------------- MODAL --
//
// vrack's dsp/modal.py: an Exciter adds a strike's burst to the excitation,
// the sum is scaled and rounded to int16 as the rack's bridge rounds it, and
// audiomodal.Bank's kernel rings it. The bank is the kernel itself, called on
// the whole block, so the modes match audiomodal.Bank sample for sample; the
// rack hears them 256 frames later, through its ChunkedBridge.

#define MODAL_STRIKE_DECAY 0.85
#define MODAL_STRIKE_SAMPLES 40
#define MODAL_THRESHOLD (0.1f * AUDIODSP_GRAPH_UNIT)

// The kernel keeps pointers to its tables, which live in the node's data.
static void modal_bind(graph_modal_t *d) {
    d->config.modes = d->modes;
    d->config.coeffs = d->coeffs;
    d->state.s1 = d->s1;
    d->state.s2 = d->s2;
    d->state.words = AUDIODSP_GRAPH_MODAL_MODES;
}

static void modal_init(graph_modal_t *d, uint32_t sample_rate) {
    audiodsp_modal_config_init(&d->config, sample_rate, 1u,
        AUDIODSP_GRAPH_MODAL_MODES, d->modes, d->coeffs);
    audiodsp_modal_state_init(&d->state, &d->config, d->s1, d->s2,
        AUDIODSP_GRAPH_MODAL_MODES);
    d->ready = false;
}

// ModalResonatorCore.tune: mode m at frequency * ratio[m] (under 0.45 of
// the rate), decay * (1 - 0.5 * m / 8) seconds, and a gain of
// (0.2 + 0.8 * brightness)^m / (m + 1).
static void modal_tune(graph_modal_t *d, const float *param, uint32_t sample_rate) {
    const double top = (double)sample_rate * 0.45;
    const double freq = (double)param[AUDIODSP_GRAPH_MODAL_FREQUENCY];
    const double decay = (double)param[AUDIODSP_GRAPH_MODAL_DECAY];
    const double tilt = 0.2 + 0.8 * (double)param[AUDIODSP_GRAPH_MODAL_BRIGHTNESS];
    double power = 1.0;
    for (uint32_t m = 0; m < AUDIODSP_GRAPH_MODAL_MODES; ++m) {
        const double ratio = (double)param[AUDIODSP_GRAPH_MODAL_RATIO + m];
        double f = freq * ratio;
        if (f > top) {
            f = top;
        }
        const double t = decay * (1.0 - 0.5 * (double)m / (double)AUDIODSP_GRAPH_MODAL_MODES);
        const double gain = (1.0 / (double)(m + 1u)) * power;
        audiodsp_modal_set_mode(&d->config, m, (float)f, (float)t, (float)gain);
        power *= tilt;
    }
    audiodsp_modal_config_finish(&d->config);
}

static void run_modal(const audiodsp_graph_t *graph, audiodsp_graph_node_t *n,
    int16_t *out, uint32_t frames) {
    graph_modal_t *d = n->data;
    modal_bind(d);
    const int16_t *in = n->ninputs > 0 && n->inputs[0] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[0]] : NULL;
    const int16_t *strike = n->ninputs > 1 && n->inputs[1] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[1]] : NULL;

    // Retune only when a parameter the modes depend on moved.
    bool moved = !d->ready;
    d->ready = true;
    for (uint32_t i = 0; i < 3 + AUDIODSP_GRAPH_MODAL_MODES; ++i) {
        const float v = n->param[i < 3 ? i : AUDIODSP_GRAPH_MODAL_RATIO + (i - 3)];
        if (v != d->tuned[i]) {
            d->tuned[i] = v;
            moved = true;
        }
    }
    if (moved) {
        modal_tune(d, n->param, graph->sample_rate);
    }
    audiodsp_modal_configure(&d->config, AUDIODSP_MODAL_OPT_MIX,
        n->param[AUDIODSP_GRAPH_MODAL_MIX]);

    // The Exciter: the first rising edge of the strike this block restarts
    // the burst there.
    int32_t edge = -1;
    if (strike != NULL) {
        bool high = d->high;
        for (uint32_t i = 0; i < frames; ++i) {
            const bool now = (float)strike[i] > MODAL_THRESHOLD;
            if (now && !high) {
                edge = (int32_t)i;
                break;
            }
            high = now;
        }
        d->high = (float)strike[frames - 1] > MODAL_THRESHOLD;
    }
    const float input = n->param[AUDIODSP_GRAPH_MODAL_INPUT];
    const float inv = 1.0f / AUDIODSP_GRAPH_UNIT;
    int16_t x[AUDIODSP_GRAPH_MAX_FRAMES];
    for (uint32_t i = 0; i < frames; ++i) {
        if ((int32_t)i == edge) {
            d->burst = 1.0;
            d->pending = MODAL_STRIKE_SAMPLES;
        }
        float e = in != NULL ? (float)in[i] * inv : 0.0f;
        if (d->pending > 0) {
            d->burst *= MODAL_STRIKE_DECAY;
            e += (float)d->burst;
            if (--d->pending == 0) {
                d->burst = 0.0;
            }
        }
        // The bridge's cast: scale, clamp to the rails, round half away
        // from zero.
        float v = e * input * AUDIODSP_GRAPH_UNIT;
        v = v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v);
        x[i] = (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
    }
    audiodsp_modal_process_s16(&d->config, &d->state, out, x, frames);
}

// ------------------------------------------------------------- STRUCK --
//
// vrack's dsp/keys.py: HammerExciter and StruckBarCore fused around one
// 24-mode audiomodal.Bank kernel (mix = 1.0, gain = 1/16). A rising edge on
// `strike` resets the bank and starts a half-sine hammer or ramp pluck pulse;
// a falling edge with `release > 0` starts a release pluck without resetting
// the bank.

static const float struck_tine_default[6 * 3] = {
    1.0f, 1.6f, 1.0f,
    2.0f, 0.55f, 0.14f,
    4.0f, 0.09f, 0.20f,
    6.27f, 0.07f, 0.30f,
    10.7f, 0.025f, 0.12f,
    15.8f, 0.012f, 0.05f,
};

static void struck_bind(graph_struck_t *d) {
    d->config.modes = d->modes;
    d->config.coeffs = d->coeffs;
    d->state.s1 = d->s1;
    d->state.s2 = d->s2;
    d->state.words = AUDIODSP_GRAPH_STRUCK_MODES;
}

static void struck_init(graph_struck_t *d, uint32_t sample_rate) {
    audiodsp_modal_config_init(&d->config, sample_rate, 1u,
        AUDIODSP_GRAPH_STRUCK_MODES, d->modes, d->coeffs);
    audiodsp_modal_configure(&d->config, AUDIODSP_MODAL_OPT_MIX, 1.0f);
    audiodsp_modal_configure(&d->config, AUDIODSP_MODAL_OPT_GAIN, 0.0625f);
    audiodsp_modal_state_init(&d->state, &d->config, d->s1, d->s2,
        AUDIODSP_GRAPH_STRUCK_MODES);
    memcpy(d->table, struck_tine_default, sizeof(struck_tine_default));
    d->table_modes = 6u;
    d->has_table = false;
    d->ready = false;
    d->tuned_f0 = 0.0f;
    d->high = false;
    d->pulse_active = false;
    d->pulse_len = 0u;
    d->pulse_pos = 0u;
    d->pulse_amp = 0.0f;
    d->pulse_shape = AUDIODSP_GRAPH_STRUCK_HAMMER;
    d->pre.valid = false;
}

static void struck_tune_config(audiodsp_modal_config_t *config,
    const float *table, uint32_t table_modes, float f0, uint32_t sample_rate) {
    const double nyq = (double)sample_rate * 0.45;
    const double base = (double)f0;
    const uint32_t used = table_modes <= AUDIODSP_GRAPH_STRUCK_MODES
        ? table_modes : AUDIODSP_GRAPH_STRUCK_MODES;
    for (uint32_t m = 0; m < used; ++m) {
        const double ratio = (double)table[m * 3u + 0u];
        const double decay = (double)table[m * 3u + 1u];
        double gain = (double)table[m * 3u + 2u];
        double freq = base * ratio;
        if (freq >= nyq) {
            freq = nyq;
            gain = 0.0;
        }
        const double t = decay > 0.001 ? decay : 0.001;
        audiodsp_modal_set_mode(config, m, (float)freq, (float)t, (float)gain);
    }
    for (uint32_t m = used; m < AUDIODSP_GRAPH_STRUCK_MODES; ++m) {
        audiodsp_modal_set_mode(config, m, f0, 0.001f, 0.0f);
    }
    audiodsp_modal_config_finish(config);
}

static void struck_tune(graph_struck_t *d, float f0, uint32_t sample_rate) {
    if (d->pre.valid && d->pre.f0 == f0) {
        // table() worked this out already, from this table at this
        // frequency, with struck_tune_config(): copy it.
        memcpy(d->modes, d->pre.modes, sizeof(d->modes));
        memcpy(d->coeffs, d->pre.coeffs, sizeof(d->coeffs));
        d->config.derived = true;
        return;
    }
    struck_tune_config(&d->config, d->table, d->table_modes, f0, sample_rate);
}

static uint32_t struck_round_even(double x) {
    if (!(x >= 4.0)) {
        return 4u;
    }
    if (x >= 65536.0) {
        return 65536u;
    }
    uint32_t n = (uint32_t)x;
    const double diff = x - (double)n;
    if (diff > 0.5 || (diff == 0.5 && (n & 1u) != 0u)) {
        n += 1u;
    }
    return n < 4u ? 4u : n;
}

static void struck_start_pulse(graph_struck_t *d, float hardness, float level,
    float f0, uint32_t shape, uint32_t sample_rate) {
    double h = (double)hardness;
    if (!(h >= 0.0)) {
        h = 0.0;
    } else if (h > 1.0) {
        h = 1.0;
    }
    const double sr = (double)sample_rate;
    const double scale = sr / 48000.0;
    const double soft = 128.0 * scale;
    const double hard = 16.0 * scale;
    double length = soft * graph_exp2d(-3.0 * h);
    double amp = (double)level * (hard / length);
    if (f0 > 0.0f) {
        const double f = (double)f0 > 1.0 ? (double)f0 : 1.0;
        const double limit = 0.5 * (sr / f);
        if (length > limit) {
            amp *= length / limit;
            length = limit;
        }
    }
    d->pulse_len = struck_round_even(length);
    d->pulse_pos = 0u;
    d->pulse_amp = (float)amp;
    d->pulse_active = true;
    d->pulse_shape = shape;
}

static void struck_render_pulse(graph_struck_t *d, float *dst, uint32_t start,
    uint32_t end) {
    if (!d->pulse_active || start >= end) {
        return;
    }
    const uint32_t rem = d->pulse_len > d->pulse_pos
        ? d->pulse_len - d->pulse_pos : 0u;
    const uint32_t span = end - start;
    const uint32_t k = rem < span ? rem : span;
    if (k == 0u) {
        d->pulse_active = false;
        return;
    }
    const uint32_t pos = d->pulse_pos;
    const uint32_t n = d->pulse_len;
    const float amp = d->pulse_amp;
    if (d->pulse_shape == AUDIODSP_GRAPH_STRUCK_PLUCK) {
        const float step = (float)(1.0 / (double)n);
        for (uint32_t j = 0; j < k; ++j) {
            float t = (float)(pos + j);
            t *= step;
            t *= amp;
            dst[start + j] += t;
        }
    } else {
        const float step = (float)(3.14159265358979323846 / (double)n);
        for (uint32_t j = 0; j < k; ++j) {
            float t = (float)(pos + j);
            t *= step;
            t = graph_sin(t);
            t *= amp;
            dst[start + j] += t;
        }
    }
    d->pulse_pos += k;
    if (d->pulse_pos >= d->pulse_len) {
        d->pulse_active = false;
    }
}

static float struck_velocity(const int16_t *vel, int32_t idx) {
    if (vel == NULL || idx < 0) {
        return 1.0f;
    }
    float v = (float)vel[idx] * (1.0f / AUDIODSP_GRAPH_UNIT);
    if (v < 0.0f) {
        v = 0.0f;
    } else if (v > 1.0f) {
        v = 1.0f;
    }
    return v;
}

static void run_struck(const audiodsp_graph_t *graph, audiodsp_graph_node_t *n,
    int16_t *out, uint32_t frames) {
    graph_struck_t *d = n->data;
    struck_bind(d);
    const int16_t *strike = n->ninputs > 0 && n->inputs[0] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[0]] : NULL;
    const int16_t *vel = n->ninputs > 1 && n->inputs[1] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[1]] : NULL;

    const float raw_f0 = n->param[AUDIODSP_GRAPH_STRUCK_FREQUENCY];
    const float f0 = raw_f0 > 20.0f ? raw_f0 : 20.0f;
    if (!d->ready || f0 != d->tuned_f0) {
        d->ready = true;
        d->tuned_f0 = f0;
        struck_tune(d, f0, graph->sample_rate);
    }

    int32_t rising = -1;
    if (strike != NULL) {
        bool high = d->high;
        for (uint32_t i = 0; i < frames; ++i) {
            const bool now = (float)strike[i] > MODAL_THRESHOLD;
            if (now && !high && rising < 0) {
                rising = (int32_t)i;
            }
            high = now;
        }
        d->high = high;
    }

    float hardness = n->param[AUDIODSP_GRAPH_STRUCK_HARDNESS];
    if (!(hardness >= 0.0f)) {
        hardness = 0.0f;
    } else if (hardness > 1.0f) {
        hardness = 1.0f;
    }
    const float level = n->param[AUDIODSP_GRAPH_STRUCK_LEVEL];
    const uint32_t shape = n->param[AUDIODSP_GRAPH_STRUCK_SHAPE] >= 0.5f
        ? (uint32_t)AUDIODSP_GRAPH_STRUCK_PLUCK
        : (uint32_t)AUDIODSP_GRAPH_STRUCK_HAMMER;

    float exc[AUDIODSP_GRAPH_MAX_FRAMES];
    for (uint32_t i = 0; i < frames; ++i) {
        exc[i] = 0.0f;
    }

    if (rising >= 0) {
        audiodsp_modal_reset(&d->state);
        d->pulse_active = false;
        const float lv = level * struck_velocity(vel, rising);
        if (lv > 0.0f) {
            struck_start_pulse(d, hardness, lv, raw_f0, shape, graph->sample_rate);
        }
        struck_render_pulse(d, exc, (uint32_t)rising, frames);
    } else {
        struck_render_pulse(d, exc, 0u, frames);
    }

    int16_t x[AUDIODSP_GRAPH_MAX_FRAMES];
    for (uint32_t i = 0; i < frames; ++i) {
        x[i] = sat16(exc[i] * AUDIODSP_GRAPH_UNIT);
    }
    audiodsp_modal_process_s16(&d->config, &d->state, out, x, frames);
}

// ------------------------------------------------------------- PICKUP --
//
// vrack's dsp/keys.py PickupCore: electromagnetic `y = x / (1 + 0.5 * x^2)`
// or electrostatic `y = x / (1 - x^2 + x^4)` transducer nonlinearity with
// drive, asymmetry offset and steady-state DC removal, in float32 operation
// for operation as numpy runs it.

static void run_pickup(const audiodsp_graph_t *graph,
    const audiodsp_graph_node_t *n, int16_t *out, uint32_t frames) {
    const int16_t *in = n->ninputs > 0 && n->inputs[0] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[0]] : NULL;
    const bool electrostatic = n->param[AUDIODSP_GRAPH_PICKUP_MODE] >= 0.5f;
    const float drive = n->param[AUDIODSP_GRAPH_PICKUP_DRIVE];
    const float offset = n->param[AUDIODSP_GRAPH_PICKUP_OFFSET];
    const float level = n->param[AUDIODSP_GRAPH_PICKUP_LEVEL];
    const float inv = 1.0f / AUDIODSP_GRAPH_UNIT;
    float dc = 0.0f;
    if (offset != 0.0f) {
        const double o = (double)offset;
        if (electrostatic) {
            const double o2 = o * o;
            dc = (float)(o / (o2 * o2 - o2 + 1.0));
        } else {
            dc = (float)(o / (1.0 + 0.5 * o * o));
        }
    }
    for (uint32_t i = 0; i < frames; ++i) {
        float dst = in != NULL ? (float)in[i] * inv : 0.0f;
        if (drive != 1.0f) {
            dst *= drive;
        }
        if (offset != 0.0f) {
            dst += offset;
        }
        float tmp = dst * dst;
        if (electrostatic) {
            tmp -= 1.0f;
            tmp *= dst;
            tmp *= dst;
            tmp += 1.0f;
            dst /= tmp;
        } else {
            tmp *= 0.5f;
            tmp += 1.0f;
            dst /= tmp;
        }
        if (offset != 0.0f) {
            dst -= dc;
        }
        if (level != 1.0f) {
            dst *= level;
        }
        out[i] = sat16(dst * AUDIODSP_GRAPH_UNIT);
    }
}

// ------------------------------------------------------------ TREMOLO --
//
// vrack's dsp/tremolo.py TremoloCore, operation for operation in float32 as
// numpy does it: a sine LFO lfo = sin(phase + i dphi), the depth gliding
// from last block's to this one's across the block, then either the
// equal-power pan theta = (pi/4) d lfo, left = cos theta - sin theta,
// right = cos theta + sin theta (unity both sides at depth 0, sqrt 2 at a
// hard side), or the amplitude gain 1 - d/2 (1 - lfo) on both sides. At
// depth 0 both ends of the block the input is copied to both sides. The
// rate and depth CVs (inputs 1 and 2, read at the block's first sample) add
// to the knobs before TremoloCore.set's clamps.

// cos(x) for |x| <= pi/2, the LFO's polynomial a quarter turn on.
static float graph_cos(float x) {
    return lfo_sine(x * 0.159154943091895336f + 0.25f);
}

static void run_tremolo(const audiodsp_graph_t *graph, audiodsp_graph_node_t *n,
    int16_t *left, uint32_t frames) {
    graph_tremolo_t *d = n->data;
    int16_t *right = graph->bufs[d->right];
    const int16_t *in = n->ninputs > 0 && n->inputs[0] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[0]] : NULL;
    const int16_t *rate_cv = n->ninputs > 1 && n->inputs[1] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[1]] : NULL;
    const int16_t *depth_cv = n->ninputs > 2 && n->inputs[2] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[2]] : NULL;

    double rate = (double)n->param[AUDIODSP_GRAPH_TREMOLO_RATE];
    if (rate_cv != NULL) {
        rate += (double)rate_cv[0] * (1.0 / (double)AUDIODSP_GRAPH_UNIT);
    }
    // TremoloCore.set's clamps (a NaN goes where Python's max/min send it),
    // and the rate held to the sample rate so the phase wrap stays bounded.
    if (!(rate >= 0.01)) {
        rate = 0.01;
    } else if (rate > (double)graph->sample_rate) {
        rate = (double)graph->sample_rate;
    }

    double depth = (double)n->param[AUDIODSP_GRAPH_TREMOLO_DEPTH];
    if (depth_cv != NULL) {
        depth += (double)depth_cv[0] * (1.0 / (double)AUDIODSP_GRAPH_UNIT);
    }
    if (!(depth <= 1.0)) {
        depth = 1.0;
    } else if (depth < 0.0) {
        depth = 0.0;
    }

    const bool amp_mode = n->param[AUDIODSP_GRAPH_TREMOLO_MODE] >= 0.5f;
    const double two_pi = 6.283185307179586;
    const double d0 = d->depth_now;
    const double d1 = depth;
    d->depth_now = d1;
    const double d_phase = two_pi * rate / (double)graph->sample_rate;
    const double phase = d->phase;
    // (phase + n dphi) % 2 pi: one subtraction is exact (Sterbenz) while a
    // block's advance stays under a turn, rates below sample_rate / frames.
    double next = phase + (double)frames * d_phase;
    if (next >= two_pi) {
        next -= two_pi;
        if (next >= two_pi) {
            next -= two_pi * (double)(int64_t)(next / two_pi);
            if (!(next >= 0.0 && next < two_pi)) {
                next = 0.0;
            }
        }
    }
    d->phase = next;

    if (d0 <= 0.0 && d1 <= 0.0) {
        for (uint32_t i = 0; i < frames; ++i) {
            const int16_t s = in != NULL ? in[i] : 0;
            left[i] = s;
            right[i] = s;
        }
        return;
    }

    const float inv = 1.0f / AUDIODSP_GRAPH_UNIT;
    const bool glide = d0 != d1;
    // numpy rounds each Python scalar to float32 before the operation.
    const float step = (float)d_phase;
    const float base = (float)(phase - d_phase);
    const float glide_step = (float)((d1 - d0) / (double)frames);
    const float glide_from = (float)d0;
    const float pan_k = (float)(0.7853981633974483 * (glide ? 1.0 : d1));
    const float amp_k = (float)(0.5 * (glide ? 1.0 : d1));
    const float amp_step = (float)(-0.5 * (d1 - d0) / (double)frames);
    const float amp_from = (float)(1.0 - 0.5 * (glide ? d0 : d1));

    for (uint32_t i = 0; i < frames; ++i) {
        // lfo = ramp * dphi + (phase - dphi), the ramp 1..n.
        const float ramp = (float)(i + 1u);
        float lfo = ramp * step;
        lfo += base;
        lfo = graph_sin(lfo);
        if (glide) {
            float g = ramp * glide_step;
            g += glide_from;
            lfo *= g;
        }
        float gl;
        float gr;
        if (amp_mode) {
            lfo *= amp_k;
            if (glide) {
                float g = ramp * amp_step;
                g += amp_from;
                lfo += g;
            } else {
                lfo += amp_from;
            }
            gl = lfo;
            gr = lfo;
        } else {
            lfo *= pan_k;
            const float c = graph_cos(lfo);
            const float s = graph_sin(lfo);
            gl = c - s;
            gr = c + s;
        }
        const float x = in != NULL ? (float)in[i] * inv : 0.0f;
        left[i] = sat16(x * gl * AUDIODSP_GRAPH_UNIT);
        right[i] = sat16(x * gr * AUDIODSP_GRAPH_UNIT);
    }
}

// --------------------------------------------------------------- DECAY --
//
// vrack's dsp/drums.py OneShotEnvelopeCore, the burst envelopes inside
// HandClapCore, JingleCore and GuiroCore, CowbellCore's two-stage decay,
// MaracasCore's linear attack and the amp-minus-rise strike of
// BridgedTDrumCore and SweptOscillatorDrumCore. A rising edge of the trigger
// starts the envelope at the velocity; a burst table re-strikes it. Only the
// first rising edge in a block retriggers, as EdgeDetector reports one.

#define DECAY_THRESHOLD (0.1f * AUDIODSP_GRAPH_UNIT)
#define DECAY_SILENT 1e-4f
// CowbellCore drops a two-stage envelope at 2e-4, a count above where the
// one-shot cores stop.
#define DECAY_SILENT_FAST 2e-4f

// The rack's exp(-4.60517 / (sr * seconds)): a fall of 40 dB over `seconds`,
// which is exp(-1 / (sr * seconds / 4.60517)).
static float decay_coeff(float seconds, uint32_t sample_rate) {
    float steps = seconds * (float)sample_rate * (1.0f / 4.60517f);
    if (steps < 1.0f) {
        steps = 1.0f;
    }
    return expf(-1.0f / steps);
}

// The burst's spike at `delay` seconds, as the rack counts it (nearest sample).
static uint32_t decay_spike(float delay, uint32_t sample_rate) {
    if (!(delay > 0.0f)) {
        return 0u;
    }
    const float at = delay * (float)sample_rate + 0.5f;
    if (!(at < 4294967295.0f)) {
        return 0x7FFFFFFFu;
    }
    return (uint32_t)at;
}

static void run_decay(const audiodsp_graph_t *graph, audiodsp_graph_node_t *n,
    int16_t *out, uint32_t frames) {
    graph_decay_t *d = n->data;
    const int16_t *gate = n->ninputs > 0 && n->inputs[0] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[0]] : NULL;
    const int16_t *vel = n->ninputs > 1 && n->inputs[1] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[1]] : NULL;

    float decay_sec = n->param[AUDIODSP_GRAPH_DECAY_DECAY];
    if (!(decay_sec >= 0.001f)) {
        decay_sec = 0.001f;
    }
    float restrike_sec = n->param[AUDIODSP_GRAPH_DECAY_RESTRIKE];
    if (!(restrike_sec >= 0.001f)) {
        restrike_sec = 0.001f;
    }
    float attack_sec = n->param[AUDIODSP_GRAPH_DECAY_ATTACK];
    if (!(attack_sec > 0.0f)) {
        attack_sec = 0.0f;
    }
    float fast_sec = n->param[AUDIODSP_GRAPH_DECAY_FAST];
    if (!(fast_sec > 0.0f)) {
        fast_sec = 0.0f;
    }
    float fast_len = n->param[AUDIODSP_GRAPH_DECAY_FAST_SEC];
    if (!(fast_len > 0.0f)) {
        fast_len = 0.0f;
    }
    float rise_sec = n->param[AUDIODSP_GRAPH_DECAY_RISE];
    if (!(rise_sec > 0.0f)) {
        rise_sec = 0.0f;
    }
    const uint32_t rate = graph->sample_rate;
    const float d_tail = decay_coeff(decay_sec, rate);
    const float d_burst = decay_coeff(restrike_sec, rate);
    // MaracasCore's attack_n = int(0.008 * sr); CowbellCore's fast_end is
    // int(0.015 * sr) counted from the trigger.
    uint32_t attack_n = 0u;
    if (attack_sec > 0.0f) {
        const float n_atk = attack_sec * (float)rate;
        // Clamped before the cast: a float past UINT32_MAX (or inf) has no
        // defined conversion.
        attack_n = n_atk >= 4294967040.0f ? 4294967040u
            : n_atk >= 1.0f ? (uint32_t)n_atk : 1u;
    }
    uint32_t fast_n = 0u;
    float d_fast = d_tail;
    if (fast_sec > 0.0f && fast_len > 0.0f) {
        const float n_fast = fast_len * (float)rate;
        fast_n = n_fast >= 4294967040.0f ? 4294967040u
            : n_fast >= 1.0f ? (uint32_t)n_fast : 1u;
        d_fast = decay_coeff(fast_sec, rate);
    }
    const bool use_rise = rise_sec > 0.0f;
    const float d_rise = use_rise ? decay_coeff(rise_sec, rate) : 0.0f;
    const float silent = fast_n > 0u ? DECAY_SILENT_FAST : DECAY_SILENT;

    // The first rising edge of the trigger this block, as MODAL finds it.
    int32_t rising = -1;
    if (gate != NULL) {
        bool high = d->high;
        for (uint32_t i = 0; i < frames; ++i) {
            const bool now = (float)gate[i] > DECAY_THRESHOLD;
            if (now && !high && rising < 0) {
                rising = (int32_t)i;
            }
            high = now;
        }
        d->high = high;
    }

    float level = d->level;
    float rise = d->rise;
    float velocity = d->velocity;
    uint32_t sample = d->sample;
    bool ringing = d->ringing;
    const bool burst = d->has_table && d->strikes > 0u;
    const uint32_t strikes = burst ? d->strikes : 0u;
    // Where each spike sits, counted from the trigger's sample, and which one
    // comes next. The table is in delay order, as the rack keeps it.
    uint32_t spike_at[AUDIODSP_GRAPH_DECAY_STRIKES];
    uint32_t next_spike = 0u;
    if (burst) {
        for (uint32_t s = 0; s < strikes; ++s) {
            spike_at[s] = decay_spike(d->table[s * 2u], rate);
        }
        while (next_spike < strikes && spike_at[next_spike] < sample) {
            ++next_spike;
        }
    }

    for (uint32_t i = 0; i < frames; ++i) {
        if (rising == (int32_t)i) {
            // A new strike replaces the envelope, as trigger() does.
            velocity = vel != NULL
                ? (float)vel[i] * (1.0f / AUDIODSP_GRAPH_UNIT) : 1.0f;
            sample = 0u;
            ringing = true;
            rise = use_rise && !burst ? velocity : 0.0f;
            next_spike = 0u;
            // linear_segment starts at -v/n so the first ramp sample is 0;
            // exp_segment starts at the velocity and writes v * coeff first.
            if (attack_n > 0u && !burst) {
                level = -velocity / (float)attack_n;
            } else {
                level = velocity;
            }
            rising = -1;
        }
        if (!ringing) {
            out[i] = 0;
            continue;
        }
        float written;
        if (burst) {
            if (next_spike < strikes && spike_at[next_spike] == sample) {
                // A spike this sample replaces the level and writes it.
                level = velocity * d->table[next_spike * 2u + 1u];
                written = level;
                ++next_spike;
            } else {
                // Between spikes the level falls to -40 dB over the re-strike
                // time; after the last spike, over the decay time.
                const bool past = next_spike >= strikes;
                level *= past ? d_tail : d_burst;
                written = level;
            }
            ++sample;
        } else if (attack_n > 0u && sample < attack_n) {
            // linear_segment: start + slope * (k + 1), slope = v / attack_n.
            level += velocity / (float)attack_n;
            written = level;
            ++sample;
        } else {
            const uint32_t decay_from = attack_n + fast_n;
            level *= (fast_n > 0u && sample < decay_from) ? d_fast : d_tail;
            written = level;
            ++sample;
        }
        if (use_rise && !burst) {
            rise *= d_rise;
            written -= rise;
        }
        out[i] = sat16(written * AUDIODSP_GRAPH_UNIT);
    }
    // The rack cores check silence once per block, after the block is written
    // (OneShotEnvelopeCore at 1e-4, CowbellCore at 2e-4 past the fast stage).
    // Stopping mid-block would leave their last few samples off the wire.
    if (ringing) {
        const bool past = !burst || next_spike >= strikes;
        const bool past_stage = burst ? past : (sample >= attack_n + fast_n);
        if (past_stage && level < silent && (!use_rise || rise < silent)) {
            level = 0.0f;
            rise = 0.0f;
            ringing = false;
            sample = 0u;
        }
    }
    d->level = level;
    d->rise = rise;
    d->velocity = velocity;
    d->sample = sample;
    d->ringing = ringing;
}

// ---------------------------------------------------------------- PINK --
//
// vrack's dsp/noise.py NoiseGenerator.pink: Paul Kellet's seven-state
// pinking filter on a white block, the sum scaled by 0.15.

static void run_pink(const audiodsp_graph_t *graph, const audiodsp_graph_node_t *n,
    int16_t *out, uint32_t frames) {
    graph_pink_t *d = n->data;
    const int16_t *in = n->ninputs > 0 && n->inputs[0] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[0]] : NULL;
    const float level = n->param[AUDIODSP_GRAPH_PINK_LEVEL];
    float b0 = d->b0;
    float b1 = d->b1;
    float b2 = d->b2;
    float b3 = d->b3;
    float b4 = d->b4;
    float b5 = d->b5;
    float b6 = d->b6;
    const float inv = 1.0f / AUDIODSP_GRAPH_UNIT;
    for (uint32_t i = 0; i < frames; ++i) {
        const float w = in != NULL ? (float)in[i] * inv : 0.0f;
        b0 = 0.99886f * b0 + w * 0.0555179f;
        b1 = 0.99332f * b1 + w * 0.0750759f;
        b2 = 0.96900f * b2 + w * 0.1538520f;
        b3 = 0.86650f * b3 + w * 0.3104856f;
        b4 = 0.55000f * b4 + w * 0.5329522f;
        b5 = -0.7616f * b5 - w * 0.0168980f;
        // b6 is the previous sample's share, as the rack keeps it.
        const float y = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f) * 0.15f;
        b6 = w * 0.115926f;
        out[i] = sat16(y * level * AUDIODSP_GRAPH_UNIT);
    }
    d->b0 = b0;
    d->b1 = b1;
    d->b2 = b2;
    d->b3 = b3;
    d->b4 = b4;
    d->b5 = b5;
    d->b6 = b6;
}

// --------------------------------------------------------------- CRUSH --
//
// The quantise in vrack's dsp/drums.py BitDepthRateCore: each sample rounded
// half away from zero to a step of 2^(1-bits) in units of 1.0. The hold that
// follows it is SAMPLE_HOLD's; the anti-alias filter before it is a filter
// node's.

static void run_crush(const audiodsp_graph_t *graph,
    const audiodsp_graph_node_t *n, int16_t *out, uint32_t frames) {
    const int16_t *in = n->ninputs > 0 && n->inputs[0] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[0]] : NULL;
    const float bits_f = n->param[AUDIODSP_GRAPH_CRUSH_BITS];
    // BitDepthRateCore takes int(bits) and holds it to 1..15; 16 and up pass
    // the audio through unquantised. A NaN bits is a pass-through too, so a
    // bad CV cannot turn into an out-of-range shift.
    if (!(bits_f < 16.0f)) {
        for (uint32_t i = 0; i < frames; ++i) {
            out[i] = in != NULL ? in[i] : 0;
        }
        return;
    }
    // Clamped in float first: -inf, or below INT32_MIN, has no defined cast.
    int32_t bits = bits_f < 1.0f ? 1 : (int32_t)bits_f;
    if (bits > 15) {
        bits = 15;
    }
    // A step of 1 << (15 - bits) counts, which is 2^(1-bits) in units of 1.0.
    const int32_t step = 1 << (15 - bits);
    const int32_t half = step >> 1;
    for (uint32_t i = 0; i < frames; ++i) {
        const int32_t x = in != NULL ? (int32_t)in[i] : 0;
        // Round half away from zero to a whole step, as _hold_numpy does on
        // the rack: trunc(x / step + copysign(0.5, x)) * step. C division
        // truncates toward zero, which is that trunc.
        const int32_t q = x >= 0 ? (x + half) / step : (x - half) / step;
        out[i] = sat16((float)(q * step));
    }
    (void)graph;
}

// ----------------------------------------------------------------- ACID --
//
// vrack's dsp/ladder.py AcidFilterCore: four one-pole stages, the feedback
// high-passed so bass thins as resonance rises, and an accent capacitor that
// pushes cutoff and drive for a short burst. The cutoff coefficient is
// refreshed every four samples of each run, as the rack refreshes it every
// four samples of each render.

static void run_acid(const audiodsp_graph_t *graph,
    const audiodsp_graph_node_t *n, int16_t *out, uint32_t frames) {
    graph_acid_t *d = n->data;
    const int16_t *in = n->inputs[0] == AUDIODSP_GRAPH_NONE
        ? NULL : graph->bufs[n->inputs[0]];
    const int16_t *env = n->ninputs > 1 && n->inputs[1] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[1]] : NULL;
    const int16_t *acc_in = n->ninputs > 2 && n->inputs[2] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[2]] : NULL;
    const float cutoff_hz = n->param[AUDIODSP_GRAPH_ACID_CUTOFF_HZ];
    // Held to 0..1, a NaN to 0: an infinite resonance or env_mod times a
    // zero state is NaN, which the stages would keep until a reset.
    float resonance = n->param[AUDIODSP_GRAPH_ACID_RESONANCE];
    resonance = !(resonance >= 0.0f) ? 0.0f : (resonance > 1.0f ? 1.0f : resonance);
    float env_mod = n->param[AUDIODSP_GRAPH_ACID_ENVMOD];
    env_mod = !(env_mod >= 0.0f) ? 0.0f : (env_mod > 1.0f ? 1.0f : env_mod);
    const float res_gain = resonance * 4.2f;
    const float decay = d->accent_decay;
    const float inv = 1.0f / AUDIODSP_GRAPH_UNIT;
    float s0 = d->s0;
    float s1 = d->s1;
    float s2 = d->s2;
    float s3 = d->s3;
    float hp = d->hp;
    float acc_env = d->accent_env;
    float g = 0.5f;
    for (uint32_t i = 0; i < frames; ++i) {
        const float x = in != NULL ? (float)in[i] * inv : 0.0f;
        const float e = env != NULL ? (float)env[i] * inv : 0.0f;
        const float acc = acc_in != NULL ? (float)acc_in[i] * inv : 0.0f;
        if (acc > 0.3f) {
            acc_env = 1.0f;
        } else {
            acc_env *= decay;
        }
        if ((i & 3u) == 0u) {
            const float total_env = e + acc_env * 0.4f;
            float f = cutoff_hz * (float)graph_exp2d((double)(total_env * env_mod * 4.0f));
            // A NaN cutoff takes the floor.
            f = !(f >= 30.0f) ? 30.0f : (f > 14000.0f ? 14000.0f : f);
            // math.tan in double, as the rack has it, from the series every
            // target computes alike (libm's tanf differs in the last place).
            float t = (float)graph_tan_clamped(
                3.14159265358979323846 * (double)f * (1.0 / (double)graph->sample_rate));
            g = t < 0.001f ? 0.001f : t;
        }
        const float res_hp = s3 - hp;
        hp += 0.95f * res_hp;
        const float u = graph_tanh(x - graph_tanh(res_hp * res_gain));
        s0 += g * (u - s0);
        s1 += g * (s0 - s1);
        s2 += g * (s1 - s2);
        s3 += g * (s2 - s3);
        float y = s3;
        if (acc_env > 0.01f) {
            y = graph_tanh(y * (1.0f + acc_env * 1.5f));
        }
        out[i] = sat16(y * AUDIODSP_GRAPH_UNIT);
    }
    d->s0 = s0;
    d->s1 = s1;
    d->s2 = s2;
    d->s3 = s3;
    d->hp = hp;
    d->accent_env = acc_env;
}

// ---------------------------------------------------------------- SPRING --
//
// vrack's dsp/reverb.py SpringReverbCore: two mono Freeverb tanks with
// slightly different room sizes, mixed against the dry signal in Python.
// The wet side is freeverb at mix 1.0 (dry 0, wet 32767), then
// out = (1 - mix) * src + mix * wet, as the rack mixes it.

static void run_spring(const audiodsp_graph_t *graph,
    const audiodsp_graph_node_t *n, int16_t *left, uint32_t frames) {
    graph_spring_t *d = n->data;
    int16_t *right = graph->bufs[d->right];
    const int16_t *in = n->ninputs > 0 && n->inputs[0] != AUDIODSP_GRAPH_NONE
        ? graph->bufs[n->inputs[0]] : NULL;
    float mix = n->param[AUDIODSP_GRAPH_SPRING_MIX];
    if (!(mix >= 0.0f)) {
        mix = 0.0f;
    } else if (mix > 1.0f) {
        mix = 1.0f;
    }
    float decay = n->param[AUDIODSP_GRAPH_SPRING_DECAY];
    if (!(decay >= 0.0f)) {
        decay = 0.0f;
    } else if (decay > 1.0f) {
        decay = 1.0f;
    }
    // SpringReverbCore.set's clamp, then the right tank's extra 0.05.
    float room_l = decay < 0.1f ? 0.1f : (decay > 0.98f ? 0.98f : decay);
    float room_r = decay + 0.05f;
    room_r = room_r < 0.1f ? 0.1f : (room_r > 0.98f ? 0.98f : room_r);
    float damp = n->param[AUDIODSP_GRAPH_SPRING_DAMP];
    if (!(damp >= 0.0f)) {
        damp = 0.0f;
    } else if (damp > 1.0f) {
        damp = 1.0f;
    }
    const float dry_g = 1.0f - mix;
    int16_t wet[AUDIODSP_GRAPH_MAX_FRAMES];
    if (in == NULL) {
        memset(left, 0, frames * sizeof(int16_t));
        memset(right, 0, frames * sizeof(int16_t));
        return;
    }
    audiodsp_freeverb_process_s16(wet, in, frames, 1,
        d->left.comb, d->left.comb_index, d->left.comb_filter,
        d->left.allpass, d->left.allpass_index, (double)room_l, (double)damp, 1.0);
    for (uint32_t i = 0; i < frames; ++i) {
        left[i] = sat16((float)in[i] * dry_g + (float)wet[i] * mix);
    }
    audiodsp_freeverb_process_s16(wet, in, frames, 1,
        d->right_tank.comb, d->right_tank.comb_index, d->right_tank.comb_filter,
        d->right_tank.allpass, d->right_tank.allpass_index,
        (double)room_r, (double)damp, 1.0);
    for (uint32_t i = 0; i < frames; ++i) {
        right[i] = sat16((float)in[i] * dry_g + (float)wet[i] * mix);
    }
}

uint32_t audiodsp_graph_run(audiodsp_graph_t *graph, int16_t *out,
    uint32_t enable) {
    const uint32_t frames = graph->frames;
    float acc[AUDIODSP_GRAPH_MAX_FRAMES];
    uint32_t sounding = 0;
    for (uint32_t index = 0; index < graph->nnodes; ++index) {
        audiodsp_graph_node_t *n = audiodsp_graph_node(graph, index);
        int16_t *dst = n->output == AUDIODSP_GRAPH_NONE
            ? NULL : graph->bufs[n->output];
        if (n->group != AUDIODSP_GRAPH_ALWAYS && !((enable >> n->group) & 1u)) {
            if (n->ran) {
                n->ran = false;
                if (dst != NULL) {
                    memset(dst, 0, frames * sizeof(int16_t));
                }
                if (n->kind == AUDIODSP_GRAPH_PAN) {
                    memset(graph->bufs[((graph_pan_t *)n->data)->right], 0,
                        frames * sizeof(int16_t));
                } else if (n->kind == AUDIODSP_GRAPH_TREMOLO) {
                    memset(graph->bufs[((graph_tremolo_t *)n->data)->right], 0,
                        frames * sizeof(int16_t));
                } else if (n->kind == AUDIODSP_GRAPH_SPRING) {
                    memset(graph->bufs[((graph_spring_t *)n->data)->right], 0,
                        frames * sizeof(int16_t));
                }
            }
            continue;
        }
        n->ran = true;
        if (n->dirty) {
            derive(graph, n);
        }
        const int16_t *in0 = n->inputs[0] == AUDIODSP_GRAPH_NONE
            ? NULL : graph->bufs[n->inputs[0]];
        switch (n->kind) {
            case AUDIODSP_GRAPH_CONST: {
                const int16_t v = sat16(n->param[0]);
                for (uint32_t i = 0; i < frames; ++i) {
                    dst[i] = v;
                }
                break;
            }
            case AUDIODSP_GRAPH_OSC: {
                graph_osc_t *d = n->data;
                const int16_t *live[AUDIODSP_OSCILLATOR_INPUTS];
                uint32_t strides[AUDIODSP_OSCILLATOR_INPUTS];
                for (uint32_t k = 0; k < AUDIODSP_OSCILLATOR_INPUTS; ++k) {
                    live[k] = (k < n->ninputs && n->inputs[k] != AUDIODSP_GRAPH_NONE)
                        ? graph->bufs[n->inputs[k]] : NULL;
                    strides[k] = 1;
                }
                audiodsp_oscillator_process_s16(&d->config, &d->state, dst,
                    frames, live, strides);
                break;
            }
            case AUDIODSP_GRAPH_LADDER: {
                graph_ladder_t *d = n->data;
                const int16_t *cutoff = (n->ninputs > 1 && n->inputs[1] != AUDIODSP_GRAPH_NONE)
                    ? graph->bufs[n->inputs[1]] : NULL;
                if (in0 == NULL) {
                    memset(dst, 0, frames * sizeof(int16_t));
                    in0 = dst;
                }
                audiodsp_ladder_process_cutoff_s16(&d->config, &d->state, dst,
                    in0, frames, cutoff, 1);
                break;
            }
            case AUDIODSP_GRAPH_BIQUAD: {
                graph_biquad_t *d = n->data;
                if (in0 == NULL) {
                    memset(dst, 0, frames * sizeof(int16_t));
                    in0 = dst;
                }
                audiodsp_biquad_f32_process_s16(&d->config, &d->state, dst,
                    in0, frames);
                break;
            }
            case AUDIODSP_GRAPH_FBDELAY: {
                graph_fbdelay_t *d = n->data;
                if (in0 == NULL) {
                    memset(dst, 0, frames * sizeof(int16_t));
                    in0 = dst;
                }
                audiodsp_feedback_delay_process_s16(&d->config, &d->state, dst,
                    in0, frames);
                break;
            }
            case AUDIODSP_GRAPH_ENV: {
                run_env(n, in0, dst, frames);
                if (((graph_env_t *)n->data)->stage != ENV_IDLE &&
                    n->group != AUDIODSP_GRAPH_ALWAYS) {
                    sounding |= 1u << n->group;
                }
                break;
            }
            case AUDIODSP_GRAPH_ADSR2: {
                run_adsr2(n, in0, dst, frames);
                if (((graph_env_t *)n->data)->stage != ENV_IDLE &&
                    n->group != AUDIODSP_GRAPH_ALWAYS) {
                    sounding |= 1u << n->group;
                }
                break;
            }
            case AUDIODSP_GRAPH_LFO:
                run_lfo(n, dst, frames, graph->sample_rate);
                break;
            case AUDIODSP_GRAPH_NOISE:
                run_noise(n, dst, frames);
                break;
            case AUDIODSP_GRAPH_MIX:
                run_mix(graph, n, dst, frames, acc);
                break;
            case AUDIODSP_GRAPH_VCA:
                run_vca(graph, n, dst, frames);
                break;
            case AUDIODSP_GRAPH_OUT:
                run_out(graph, n, out, frames);
                break;
            case AUDIODSP_GRAPH_PHASER: {
                graph_phaser_t *d = n->data;
                if (in0 == NULL) {
                    memset(dst, 0, frames * sizeof(int16_t));
                    in0 = dst;
                }
                audiodsp_allpass_f32_process_s16(&d->config, &d->state, dst,
                    in0, frames);
                break;
            }
            case AUDIODSP_GRAPH_LAG:
                run_lag(graph, n, dst, frames);
                break;
            case AUDIODSP_GRAPH_GLISS:
                run_gliss(graph, n, dst, frames);
                break;
            case AUDIODSP_GRAPH_FOLLOW:
                run_follow(graph, n, dst, frames);
                break;
            case AUDIODSP_GRAPH_SAMPLE_HOLD:
                run_sample_hold(graph, n, dst, frames);
                break;
            case AUDIODSP_GRAPH_TWOPOLE:
                run_twopole(graph, n, dst, frames);
                break;
            case AUDIODSP_GRAPH_TANH:
                run_tanh(graph, n, dst, frames);
                break;
            case AUDIODSP_GRAPH_PAN:
                run_pan(graph, n, dst, frames, acc);
                break;
            case AUDIODSP_GRAPH_WAVEFOLD:
                run_wavefold(graph, n, dst, frames);
                break;
            case AUDIODSP_GRAPH_LPG:
                run_lpg(graph, n, dst, frames);
                break;
            case AUDIODSP_GRAPH_MODAL:
                run_modal(graph, n, dst, frames);
                break;
            case AUDIODSP_GRAPH_STRUCK:
                run_struck(graph, n, dst, frames);
                break;
            case AUDIODSP_GRAPH_PICKUP:
                run_pickup(graph, n, dst, frames);
                break;
            case AUDIODSP_GRAPH_TREMOLO:
                run_tremolo(graph, n, dst, frames);
                break;
            case AUDIODSP_GRAPH_DECAY: {
                run_decay(graph, n, dst, frames);
                if (((graph_decay_t *)n->data)->ringing &&
                    n->group != AUDIODSP_GRAPH_ALWAYS) {
                    sounding |= 1u << n->group;
                }
                break;
            }
            case AUDIODSP_GRAPH_PINK:
                run_pink(graph, n, dst, frames);
                break;
            case AUDIODSP_GRAPH_CRUSH:
                run_crush(graph, n, dst, frames);
                break;
            case AUDIODSP_GRAPH_ACID:
                run_acid(graph, n, dst, frames);
                break;
            case AUDIODSP_GRAPH_SPRING:
                run_spring(graph, n, dst, frames);
                break;
#if AUDIODSP_GRAPH_HAS_OVERDRIVE
            case AUDIODSP_GRAPH_OVERDRIVE:
                audiodsp_graph_overdrive_run(n->data, n->inputs[0] == AUDIODSP_GRAPH_NONE
                    ? NULL : graph->bufs[n->inputs[0]], dst, frames);
                break;
#endif
            default:
                break;
        }
    }
    graph->sounding = sounding;
    return sounding;
}

int32_t audiodsp_graph_peak(const audiodsp_graph_t *graph, uint32_t buffer) {
    if (buffer >= graph->nbufs) {
        return -1;
    }
    const int16_t *b = graph->bufs[buffer];
    int32_t peak = 0;
    for (uint32_t i = 0; i < graph->frames; ++i) {
        int32_t v = b[i] < 0 ? -(int32_t)b[i] : b[i];
        if (v > peak) {
            peak = v;
        }
    }
    return peak;
}

// --- the control queue ------------------------------------------------------

void audiodsp_graph_queue_init(audiodsp_graph_t *graph,
    audiodsp_graph_record_t **records, uint32_t capacity, float **shadow) {
    graph->records = records;
    graph->capacity = capacity;
    graph->shadow = shadow;
    if (shadow != NULL) {
        for (uint32_t i = 0; i < graph->nnodes; ++i) {
            const audiodsp_graph_node_t *n = audiodsp_graph_node(graph, i);
            memcpy(audiodsp_graph_shadow(graph, i), n->param, sizeof(n->param));
        }
    }
}

bool audiodsp_graph_valid(const audiodsp_graph_t *graph, uint32_t node,
    uint32_t index) {
    return node < graph->nnodes && index < AUDIODSP_GRAPH_MAX_PARAMS;
}

static void apply(audiodsp_graph_t *graph, const audiodsp_graph_record_t *r) {
    switch (r->op) {
        case AUDIODSP_GRAPH_OP_SET:
            audiodsp_graph_set(graph, r->node, r->index, r->arg.value);
            break;
        case AUDIODSP_GRAPH_OP_RESET:
            audiodsp_graph_reset(graph, r->node);
            break;
        case AUDIODSP_GRAPH_OP_ENABLE:
            graph->enable = r->arg.mask;
            break;
        default:
            break;
    }
}

uint32_t audiodsp_graph_drain(audiodsp_graph_t *graph) {
    const uint32_t end = GRAPH_LOAD_ACQ(&graph->published);
    uint32_t at = graph->tail;
    const uint32_t n = end - at;
    for (; at != end; ++at) {
        apply(graph, audiodsp_graph_record(graph, at));
    }
    if (n) {
        graph->applied += n;
        GRAPH_STORE_REL(&graph->tail, end);
    }
    return n;
}

static void publish(audiodsp_graph_t *graph) {
    GRAPH_STORE_REL(&graph->published, graph->head);
}

void audiodsp_graph_enqueue(audiodsp_graph_t *graph, uint8_t op,
    uint32_t node, uint32_t index, float value, uint32_t mask,
    void (*lock)(void), void (*unlock)(void)) {
    uint32_t waiting = graph->head - GRAPH_LOAD_ACQ(&graph->tail);
    if (waiting >= graph->capacity) {
        // Full. Publish what is waiting (a held batch splits here) and drain
        // it from this side, with the renderer kept out.
        graph->overflows++;
        if (graph->held) {
            graph->splits++;
        }
        publish(graph);
        if (lock != NULL) {
            lock();
        }
        audiodsp_graph_drain(graph);
        if (unlock != NULL) {
            unlock();
        }
        waiting = graph->head - GRAPH_LOAD_ACQ(&graph->tail);
    }
    audiodsp_graph_record_t *r = audiodsp_graph_record(graph, graph->head);
    r->op = op;
    r->index = (uint8_t)index;
    r->node = (uint16_t)node;
    if (op == AUDIODSP_GRAPH_OP_ENABLE) {
        r->arg.mask = mask;
    } else {
        r->arg.value = value;
    }
    graph->head++;
    graph->pushed++;
    if (waiting + 1u > graph->high_water) {
        graph->high_water = waiting + 1u;
    }
    if (op == AUDIODSP_GRAPH_OP_SET && graph->shadow != NULL) {
        audiodsp_graph_shadow(graph, node)[index] = value;
    }
    if (!graph->held) {
        publish(graph);
    }
}

void audiodsp_graph_hold(audiodsp_graph_t *graph) {
    graph->held = true;
}

void audiodsp_graph_commit(audiodsp_graph_t *graph) {
    graph->held = false;
    publish(graph);
}

static uint32_t render(audiodsp_graph_t *graph, int16_t *out, bool own,
    uint32_t enable) {
    if (graph->records != NULL) {
        audiodsp_graph_drain(graph);
    }
    const uint32_t sounding = audiodsp_graph_run(graph, out,
        own ? graph->enable : enable);
    GRAPH_STORE_REL(&graph->pub_sounding, sounding);
    GRAPH_STORE_REL(&graph->pub_frame, graph->pub_frame + graph->frames);
    GRAPH_STORE_REL(&graph->pub_applied, graph->applied);
    return sounding;
}

uint32_t audiodsp_graph_render(audiodsp_graph_t *graph, int16_t *out) {
    return render(graph, out, true, 0);
}

uint32_t audiodsp_graph_render_mask(audiodsp_graph_t *graph, int16_t *out,
    uint32_t enable) {
    return render(graph, out, false, enable);
}

void audiodsp_graph_swap(audiodsp_graph_t *a, audiodsp_graph_t *b) {
    audiodsp_graph_t t = *a;
    *a = *b;
    *b = t;
    // the frame count belongs to the stream, not to the patch
    const uint32_t frame = a->pub_frame;
    a->pub_frame = b->pub_frame;
    b->pub_frame = frame;
}

void audiodsp_graph_published(const audiodsp_graph_t *graph,
    uint32_t *sounding, uint32_t *frame, uint32_t *applied) {
    *applied = GRAPH_LOAD_ACQ(&graph->pub_applied);
    *frame = GRAPH_LOAD_ACQ(&graph->pub_frame);
    *sounding = GRAPH_LOAD_ACQ(&graph->pub_sounding);
}

bool audiodsp_graph_get_shadow(const audiodsp_graph_t *graph, uint32_t node,
    uint32_t index, float *value) {
    if (!audiodsp_graph_valid(graph, node, index)) {
        return false;
    }
    if (graph->shadow == NULL) {
        return audiodsp_graph_get(graph, node, index, value);
    }
    *value = audiodsp_graph_shadow(graph, node)[index];
    return true;
}
