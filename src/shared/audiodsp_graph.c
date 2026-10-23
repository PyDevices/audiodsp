// The graph runner. See audiodsp_graph.h.
// SPDX-License-Identifier: MIT

// First, so no multiply-add here is fused on one target and not another: a
// graph renders the same samples on every board (audiodsp_fp_contract.h).
#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_graph.h"

#include <math.h>
#include <string.h>

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
    uint32_t rng;
} graph_noise_t;

typedef struct {
    bool primed;
    float gain_now[AUDIODSP_GRAPH_MAX_INPUTS];
} graph_mix_t;

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
            return sizeof(graph_env_t);
        case AUDIODSP_GRAPH_LFO:
            return sizeof(graph_lfo_t);
        case AUDIODSP_GRAPH_NOISE:
            return sizeof(graph_noise_t);
        case AUDIODSP_GRAPH_MIX:
            return sizeof(graph_mix_t);
        case AUDIODSP_GRAPH_PHASER:
            return sizeof(graph_phaser_t) + (size_t)phaser_stages(arg) * sizeof(float);
        case AUDIODSP_GRAPH_LAG:
            return sizeof(graph_lag_t);
        case AUDIODSP_GRAPH_SAMPLE_HOLD:
            return sizeof(graph_sh_t);
        default:
            return 0;
    }
}

void audiodsp_graph_init(audiodsp_graph_t *graph, uint32_t sample_rate,
    uint32_t frames, uint32_t channel_count, int16_t *bufs, uint32_t nbufs,
    audiodsp_graph_node_t *nodes, uint32_t max_nodes) {
    graph->sample_rate = sample_rate;
    graph->frames = frames;
    graph->channel_count = channel_count;
    graph->bufs = bufs;
    graph->nbufs = nbufs;
    graph->nodes = nodes;
    graph->max_nodes = max_nodes;
    graph->nnodes = 0;
    graph->sounding = 0;
    memset(bufs, 0, (size_t)nbufs * frames * sizeof(int16_t));
}

static float env_coeff(float seconds, float scale, uint32_t sample_rate) {
    float steps = seconds * (float)sample_rate * scale;
    if (steps < 1.0f) {
        steps = 1.0f;
    }
    return expf(-1.0f / steps);
}

int audiodsp_graph_add(audiodsp_graph_t *graph, audiodsp_graph_kind_t kind,
    uint32_t group, const int16_t *inputs, uint32_t ninputs, int16_t output,
    void *data, float arg) {
    if (graph->nnodes >= graph->max_nodes || kind >= AUDIODSP_GRAPH_KINDS ||
        ninputs > AUDIODSP_GRAPH_MAX_INPUTS) {
        return -1;
    }
    if (output != AUDIODSP_GRAPH_NONE && (output < 0 || (uint32_t)output >= graph->nbufs)) {
        return -1;
    }
    if (output == AUDIODSP_GRAPH_NONE && kind != AUDIODSP_GRAPH_OUT) {
        return -1;
    }
    for (uint32_t i = 0; i < ninputs; ++i) {
        if (inputs[i] != AUDIODSP_GRAPH_NONE &&
            (inputs[i] < 0 || (uint32_t)inputs[i] >= graph->nbufs)) {
            return -1;
        }
    }
    audiodsp_graph_node_t *node = &graph->nodes[graph->nnodes];
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
        case AUDIODSP_GRAPH_SAMPLE_HOLD: {
            graph_sh_t *d = data;
            audiodsp_samplehold_config_init(&d->config, 1u, 1u);
            audiodsp_samplehold_state_init(&d->state, &d->config);
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
        case AUDIODSP_GRAPH_VCA:
            node->param[AUDIODSP_GRAPH_VCA_GAIN] = 1.0f;
            break;
        case AUDIODSP_GRAPH_OUT:
            node->param[0] = 1.0f;
            break;
        default:
            break;
    }
    return (int)graph->nnodes++;
}

bool audiodsp_graph_set(audiodsp_graph_t *graph, uint32_t node,
    uint32_t index, float value) {
    if (node >= graph->nnodes || index >= AUDIODSP_GRAPH_MAX_PARAMS) {
        return false;
    }
    audiodsp_graph_node_t *n = &graph->nodes[node];
    n->param[index] = value;
    n->dirty = true;
    return true;
}

bool audiodsp_graph_get(const audiodsp_graph_t *graph, uint32_t node,
    uint32_t index, float *value) {
    if (node >= graph->nnodes || index >= AUDIODSP_GRAPH_MAX_PARAMS) {
        return false;
    }
    *value = graph->nodes[node].param[index];
    return true;
}

bool audiodsp_graph_set_table(audiodsp_graph_t *graph, uint32_t node,
    const int16_t *table, uint32_t length) {
    if (node >= graph->nnodes || graph->nodes[node].kind != AUDIODSP_GRAPH_FBDELAY) {
        return false;
    }
    graph_fbdelay_t *d = graph->nodes[node].data;
    return audiodsp_feedback_delay_set_wow_shape(&d->config, table, length);
}

bool audiodsp_graph_reset(audiodsp_graph_t *graph, uint32_t node) {
    if (node >= graph->nnodes) {
        return false;
    }
    audiodsp_graph_node_t *n = &graph->nodes[node];
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
        case AUDIODSP_GRAPH_ENV: {
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
        case AUDIODSP_GRAPH_PHASER:
            audiodsp_allpass_f32_reset(&((graph_phaser_t *)n->data)->state);
            break;
        case AUDIODSP_GRAPH_LAG: {
            graph_lag_t *d = n->data;
            d->primed = false;
            d->left = 0;
            d->slope = 0.0f;
            d->pole = 0.0f;
            break;
        }
        case AUDIODSP_GRAPH_SAMPLE_HOLD: {
            graph_sh_t *d = n->data;
            d->high = false;
            d->held = 0.0f;
            d->out = 0.0f;
            audiodsp_samplehold_reset(&d->state, &d->config);
            break;
        }
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
        case AUDIODSP_GRAPH_ENV: {
            graph_env_t *d = n->data;
            d->a = env_coeff(n->param[AUDIODSP_GRAPH_ENV_ATTACK], 0.35f, graph->sample_rate);
            d->d = env_coeff(n->param[AUDIODSP_GRAPH_ENV_DECAY], 0.25f, graph->sample_rate);
            d->r = env_coeff(n->param[AUDIODSP_GRAPH_ENV_RELEASE], 0.25f, graph->sample_rate);
            break;
        }
        default:
            break;
    }
}

static inline int16_t sat16(float v) {
    if (v > 32767.0f) {
        return 32767;
    }
    if (v < -32768.0f) {
        return -32768;
    }
    return (int16_t)round_even(v);
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
        ? NULL : graph->bufs + (size_t)n->inputs[0] * graph->frames;
    const int16_t *pitch = n->ninputs > 1 && n->inputs[1] != AUDIODSP_GRAPH_NONE
        ? graph->bufs + (size_t)n->inputs[1] * graph->frames : NULL;
    const int16_t *legato = n->ninputs > 2 && n->inputs[2] != AUDIODSP_GRAPH_NONE
        ? graph->bufs + (size_t)n->inputs[2] * graph->frames : NULL;
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

static void run_sample_hold(const audiodsp_graph_t *graph,
    audiodsp_graph_node_t *n, int16_t *out, uint32_t frames) {
    graph_sh_t *d = n->data;
    const int16_t *signal = n->inputs[0] == AUDIODSP_GRAPH_NONE
        ? NULL : graph->bufs + (size_t)n->inputs[0] * graph->frames;
    const int16_t *clock = n->ninputs > 1 && n->inputs[1] != AUDIODSP_GRAPH_NONE
        ? graph->bufs + (size_t)n->inputs[1] * graph->frames : NULL;
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
    const float inv = 1.0f / (float)frames;
    for (uint32_t k = 0; k < n->ninputs; ++k) {
        if (n->inputs[k] == AUDIODSP_GRAPH_NONE) {
            continue;
        }
        const int16_t *in = graph->bufs + (size_t)n->inputs[k] * graph->frames;
        const float g1 = n->param[k];
        const float g0 = d->gain_now[k];
        d->gain_now[k] = g1;
        if (g0 == g1) {
            if (g1 == 0.0f) {
                continue;
            }
            for (uint32_t i = 0; i < frames; ++i) {
                acc[i] += g1 * (float)in[i];
            }
        } else {
            const float step = (g1 - g0) * inv;
            float g = g0;
            for (uint32_t i = 0; i < frames; ++i) {
                g += step;
                acc[i] += g * (float)in[i];
            }
        }
    }
    for (uint32_t i = 0; i < frames; ++i) {
        out[i] = sat16(acc[i]);
    }
}

static void run_vca(const audiodsp_graph_t *graph, audiodsp_graph_node_t *n,
    int16_t *out, uint32_t frames) {
    const int16_t *audio = n->inputs[0] == AUDIODSP_GRAPH_NONE
        ? NULL : graph->bufs + (size_t)n->inputs[0] * graph->frames;
    const uint32_t which = n->param[AUDIODSP_GRAPH_VCA_SELECT] < 0.5f ? 1u : 2u;
    const int16_t *cv = n->inputs[which] == AUDIODSP_GRAPH_NONE
        ? NULL : graph->bufs + (size_t)n->inputs[which] * graph->frames;
    const float k = n->param[AUDIODSP_GRAPH_VCA_GAIN] / AUDIODSP_GRAPH_UNIT;
    if (audio == NULL || cv == NULL) {
        memset(out, 0, frames * sizeof(int16_t));
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
        ? NULL : graph->bufs + (size_t)n->inputs[0] * graph->frames;
    const int16_t *right = n->ninputs > 1 && n->inputs[1] != AUDIODSP_GRAPH_NONE
        ? graph->bufs + (size_t)n->inputs[1] * graph->frames : left;
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

uint32_t audiodsp_graph_run(audiodsp_graph_t *graph, int16_t *out,
    uint32_t enable) {
    const uint32_t frames = graph->frames;
    float acc[AUDIODSP_GRAPH_MAX_FRAMES];
    uint32_t sounding = 0;
    for (uint32_t index = 0; index < graph->nnodes; ++index) {
        audiodsp_graph_node_t *n = &graph->nodes[index];
        int16_t *dst = n->output == AUDIODSP_GRAPH_NONE
            ? NULL : graph->bufs + (size_t)n->output * frames;
        if (n->group != AUDIODSP_GRAPH_ALWAYS && !((enable >> n->group) & 1u)) {
            if (n->ran) {
                n->ran = false;
                if (dst != NULL) {
                    memset(dst, 0, frames * sizeof(int16_t));
                }
            }
            continue;
        }
        n->ran = true;
        if (n->dirty) {
            derive(graph, n);
        }
        const int16_t *in0 = n->inputs[0] == AUDIODSP_GRAPH_NONE
            ? NULL : graph->bufs + (size_t)n->inputs[0] * frames;
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
                        ? graph->bufs + (size_t)n->inputs[k] * frames : NULL;
                    strides[k] = 1;
                }
                audiodsp_oscillator_process_s16(&d->config, &d->state, dst,
                    frames, live, strides);
                break;
            }
            case AUDIODSP_GRAPH_LADDER: {
                graph_ladder_t *d = n->data;
                const int16_t *cutoff = (n->ninputs > 1 && n->inputs[1] != AUDIODSP_GRAPH_NONE)
                    ? graph->bufs + (size_t)n->inputs[1] * frames : NULL;
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
            case AUDIODSP_GRAPH_SAMPLE_HOLD:
                run_sample_hold(graph, n, dst, frames);
                break;
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
    const int16_t *b = graph->bufs + (size_t)buffer * graph->frames;
    int32_t peak = 0;
    for (uint32_t i = 0; i < graph->frames; ++i) {
        int32_t v = b[i] < 0 ? -(int32_t)b[i] : b[i];
        if (v > peak) {
            peak = v;
        }
    }
    return peak;
}
