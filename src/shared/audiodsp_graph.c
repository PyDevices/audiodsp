// The graph runner. See audiodsp_graph.h.
// SPDX-License-Identifier: MIT

#include "shared/audiodsp_graph.h"

#include <math.h>
#include <string.h>

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
} graph_lfo_t;

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
        case AUDIODSP_GRAPH_LFO:
            node->param[AUDIODSP_GRAPH_LFO_RATE] = 1.0f;
            node->param[AUDIODSP_GRAPH_LFO_AMPLITUDE] = 0.5f * AUDIODSP_GRAPH_UNIT;
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
        case AUDIODSP_GRAPH_LFO:
            ((graph_lfo_t *)n->data)->phase = 0.0f;
            break;
        case AUDIODSP_GRAPH_MIX:
            ((graph_mix_t *)n->data)->primed = false;
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
    return (int16_t)lrintf(v);
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

static void run_lfo(audiodsp_graph_node_t *n, int16_t *out, uint32_t frames,
    uint32_t sample_rate) {
    graph_lfo_t *d = n->data;
    const float dt = n->param[AUDIODSP_GRAPH_LFO_RATE] / (float)sample_rate;
    const float amp = n->param[AUDIODSP_GRAPH_LFO_AMPLITUDE];
    float phase = d->phase;
    for (uint32_t i = 0; i < frames; ++i) {
        phase += dt;
        phase -= floorf(phase);
        // 1 - |4p - 2|
        float tri = 4.0f * phase - 2.0f;
        tri = 1.0f - (tri < 0.0f ? -tri : tri);
        out[i] = sat16(tri * amp);
    }
    d->phase = phase;
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
