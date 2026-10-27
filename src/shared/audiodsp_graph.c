// The graph runner. See audiodsp_graph.h.
// SPDX-License-Identifier: MIT

// First, so no multiply-add here is fused on one target and not another: a
// graph renders the same samples on every board (audiodsp_fp_contract.h).
#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_graph.h"

#include "shared/audiodsp_biquad.h"
#include "shared/audiodsp_distortion.h"
#include "shared/audiodsp_synth_dsp.h"

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
    // audiofilters.Distortion's gains, linear, and the shelf after it: the
    // coefficients synthio.Biquad would hold and its filter state.
    double pre_gain;
    double post_gain;
    audiodsp_biquad_cp_coefficients_t shelf;
    audiodsp_biquad_state_t state;
} graph_overdrive_t;

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
        case AUDIODSP_GRAPH_TWOPOLE:
            return sizeof(graph_twopole_t);
        case AUDIODSP_GRAPH_OVERDRIVE:
            return sizeof(graph_overdrive_t);
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
    memset(bufs, 0, (size_t)nbufs * frames * sizeof(int16_t));
}

// OverdriveCore's tone control: synthio.Biquad(HIGH_SHELF, 1500.0, Q=0.7071).
#define OVERDRIVE_SHELF_MODE 6
#define OVERDRIVE_SHELF_HZ 1500.0
#define OVERDRIVE_SHELF_Q 0.7071

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
    if (graph->shadow != NULL) {
        memcpy(graph->shadow + (size_t)graph->nnodes * AUDIODSP_GRAPH_MAX_PARAMS,
            node->param, sizeof(node->param));
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
        case AUDIODSP_GRAPH_TWOPOLE: {
            graph_twopole_t *d = n->data;
            d->s1 = 0.0f;
            d->s2 = 0.0f;
            break;
        }
        case AUDIODSP_GRAPH_OVERDRIVE:
            audiodsp_biquad_reset(&((graph_overdrive_t *)n->data)->state);
            break;
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
        case AUDIODSP_GRAPH_OVERDRIVE: {
            graph_overdrive_t *d = n->data;
            // OverdriveCore.render, term for term, in double as it is: drive
            // sets a pre-gain of up to 20 dB, level a post-gain in dB, and
            // tone the shelf's gain from -12 dB to +12 dB. The two gains are
            // clamped as audiofilters.Distortion clamps them.
            const double drive = (double)n->param[AUDIODSP_GRAPH_OVERDRIVE_DRIVE];
            const double tone = (double)n->param[AUDIODSP_GRAPH_OVERDRIVE_TONE];
            double level = (double)n->param[AUDIODSP_GRAPH_OVERDRIVE_LEVEL];
            if (!(level > 1e-4)) {
                level = 1e-4;
            }
            double pre_db = 20.0 * drive;
            pre_db = pre_db < -60.0 ? -60.0 : (pre_db > 60.0 ? 60.0 : pre_db);
            double post_db = 20.0 * log10(level);
            post_db = post_db < -80.0 ? -80.0 : (post_db > 24.0 ? 24.0 : post_db);
            d->pre_gain = exp(pre_db * 0.11512925464970228420089957273422);
            d->post_gain = exp(post_db * 0.11512925464970228420089957273422);
            const double t = tone < 0.0 ? 0.0 : (tone > 1.0 ? 1.0 : tone);
            const double db = (t - 0.5) * 24.0;
            audiodsp_biquad_cp_configure(&d->shelf, OVERDRIVE_SHELF_MODE,
                audiodsp_biquad_cp_w0(OVERDRIVE_SHELF_HZ, graph->sample_rate),
                OVERDRIVE_SHELF_Q, pow(10.0, db / 40.0));
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
        ? NULL : graph->bufs + (size_t)n->inputs[0] * graph->frames;
    const int16_t *cv = n->ninputs > 1 && n->inputs[1] != AUDIODSP_GRAPH_NONE
        ? graph->bufs + (size_t)n->inputs[1] * graph->frames : NULL;
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

// ----------------------------------------------------------- OVERDRIVE --

static void run_overdrive(const audiodsp_graph_t *graph, audiodsp_graph_node_t *n,
    int16_t *out, uint32_t frames) {
    graph_overdrive_t *d = n->data;
    const int16_t *in = n->inputs[0] == AUDIODSP_GRAPH_NONE
        ? NULL : graph->bufs + (size_t)n->inputs[0] * graph->frames;
    // audiofilters.Filter's last step: a sample past +-28000 is squeezed
    // toward the rail (SYNTHIO_MIX_DOWN_SCALE(2)).
    const int32_t knee = 0x0fffffff / (32768 * 2 - 28000);
    for (uint32_t i = 0; i < frames; ++i) {
        const int32_t sample = in != NULL ? in[i] : 0;
        // audiofilters.Distortion in OVERDRIVE mode with soft clipping and
        // the mix at 1; the drive is not read in this mode.
        const int32_t clipped = (int16_t)audiodsp_distortion_sample(sample, 0.0,
            d->pre_gain, d->post_gain, AUDIODSP_DISTORTION_OVERDRIVE, true,
            1.0, 0u);
        const int32_t shelved = audiodsp_biquad_cp_sample(&d->shelf, &d->state,
            clipped);
        out[i] = audiodsp_mix_down_sample(shelved, knee, -28000, 28000);
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
            case AUDIODSP_GRAPH_TWOPOLE:
                run_twopole(graph, n, dst, frames);
                break;
            case AUDIODSP_GRAPH_OVERDRIVE:
                run_overdrive(graph, n, dst, frames);
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

// --- the control queue ------------------------------------------------------

void audiodsp_graph_queue_init(audiodsp_graph_t *graph,
    audiodsp_graph_record_t *records, uint32_t capacity, float *shadow) {
    graph->records = records;
    graph->capacity = capacity;
    graph->shadow = shadow;
    if (shadow != NULL) {
        for (uint32_t i = 0; i < graph->nnodes; ++i) {
            memcpy(shadow + (size_t)i * AUDIODSP_GRAPH_MAX_PARAMS,
                graph->nodes[i].param, sizeof(graph->nodes[i].param));
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
        apply(graph, &graph->records[at % graph->capacity]);
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
    audiodsp_graph_record_t *r = &graph->records[graph->head % graph->capacity];
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
        graph->shadow[(size_t)node * AUDIODSP_GRAPH_MAX_PARAMS + index] = value;
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
    *value = graph->shadow[(size_t)node * AUDIODSP_GRAPH_MAX_PARAMS + index];
    return true;
}
