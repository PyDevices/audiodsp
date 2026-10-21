// Runtime-neutral graph runner: one C call renders a whole patch per block.
//
// New code -- not a CircuitPython port. A polyphonic patch built from the
// kernels in this directory runs in real time on an ESP32-P4 once the
// per-node Python between them is gone: this is the C that runs between
// them instead. The binding is src/audiograph/ (MicroPython).
//
// The runner owns the block buffers. A graph is a list of nodes in the
// order they run; each node names the buffers it reads and the buffer it
// writes, and holds the config and state of one kernel. The kernels are the
// ones the audio nodes already call -- the oscillator, the ladder, the float
// biquad, the feedback delay -- plus a few small ones a voice needs that only
// existed in Python: an ADSR, a triangle LFO, white noise, a mixer with
// ramped gains, a VCA and the output stage.
//
// Buffers carry signed 16-bit samples on the same wire the bridges use:
// 16384 counts is 1.0 (vrack's +10 V), so audio at +-0.5 is a quarter of
// full scale. Every node is mono.
//
// Nodes belong to groups (a voice each), and a run takes a mask of the
// groups to process. A node whose group is masked out is skipped, and its
// outputs are zeroed once so the mixers read silence from it. The run
// returns the mask of groups with an envelope still sounding.
//
// No pump lock in here. The runner's buffers and node state are touched only
// by whoever calls run(), set() and reset(), and nothing in this file is
// reachable from the pump's pull funnel: the output goes into a buffer the
// caller owns (an audiopump.Ring takes it from there, under its own
// synchronisation). The kernels themselves never lock either -- the locks
// in the bindings guard the bindings' own {source, pending} words, which the
// pull funnel reads from the pump thread, and this runner has no such words.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shared/audiodsp_filter_f32.h"
#include "shared/audiodsp_feedback_delay.h"
#include "shared/audiodsp_ladder.h"
#include "shared/audiodsp_oscillator.h"

//: The longest block a run renders. The kernels' own block ceiling.
#define AUDIODSP_GRAPH_MAX_FRAMES 256u

//: Buffer id for "nothing connected".
#define AUDIODSP_GRAPH_NONE (-1)

//: The group that always runs.
#define AUDIODSP_GRAPH_ALWAYS 255u

//: Counts per 1.0 on the wire (vrack's BRIDGE_SCALE).
#define AUDIODSP_GRAPH_UNIT 16384.0f

//: Inputs a node may read, and float parameters it may hold. The feedback
//: delay has the most options (16); the mixer has 8 gains and an offset.
#define AUDIODSP_GRAPH_MAX_INPUTS 8u
#define AUDIODSP_GRAPH_MAX_PARAMS 16u

typedef enum {
    // out = param[0], every sample. A scalar CV (pitch, gate, velocity).
    AUDIODSP_GRAPH_CONST = 0,
    // inputs: pitch, fm, pwm, sync. params: audiodsp_oscillator_option_t.
    AUDIODSP_GRAPH_OSC,
    // inputs: audio, cutoff. params: audiodsp_ladder_option_t.
    AUDIODSP_GRAPH_LADDER,
    // inputs: audio. params: audiodsp_biquad_f32_option_t.
    AUDIODSP_GRAPH_BIQUAD,
    // inputs: audio. params: audiodsp_feedback_delay_option_t. The node
    // is built with `arg` = max_delay_ms.
    AUDIODSP_GRAPH_FBDELAY,
    // inputs: gate. params: attack s, decay s, sustain 0..1, release s.
    // out: 0..1 on the wire. vrack's EnvelopeCore, sample by sample.
    AUDIODSP_GRAPH_ENV,
    // params: rate Hz, amplitude (counts). out: a triangle, +-amplitude.
    AUDIODSP_GRAPH_LFO,
    // params: amplitude (counts). out: uniform white noise, +-amplitude.
    AUDIODSP_GRAPH_NOISE,
    // inputs: up to 8. params: gain[8], offset (counts). out = offset +
    // sum(gain[i] * in[i]). A gain that changed since the last block ramps
    // across this one, so a voice fades rather than steps.
    AUDIODSP_GRAPH_MIX,
    // inputs: audio, cv_a, cv_b. params: gain, select. out = audio *
    // (select < 0.5 ? cv_a : cv_b) / UNIT * gain.
    AUDIODSP_GRAPH_VCA,
    // inputs: left, right. params: scale. Writes the run's output buffer:
    // interleaved stereo, or the mean of the two when the run is mono.
    AUDIODSP_GRAPH_OUT,
    AUDIODSP_GRAPH_KINDS,
} audiodsp_graph_kind_t;

enum {
    AUDIODSP_GRAPH_ENV_ATTACK = 0,
    AUDIODSP_GRAPH_ENV_DECAY,
    AUDIODSP_GRAPH_ENV_SUSTAIN,
    AUDIODSP_GRAPH_ENV_RELEASE,
};

enum {
    AUDIODSP_GRAPH_LFO_RATE = 0,
    AUDIODSP_GRAPH_LFO_AMPLITUDE,
};

enum {
    AUDIODSP_GRAPH_MIX_OFFSET = 8,
};

enum {
    AUDIODSP_GRAPH_VCA_GAIN = 0,
    AUDIODSP_GRAPH_VCA_SELECT,
};

typedef struct {
    uint8_t kind;
    uint8_t group;
    uint8_t ninputs;
    uint8_t noutputs;
    // Buffer ids; AUDIODSP_GRAPH_NONE for an input left open.
    int16_t inputs[AUDIODSP_GRAPH_MAX_INPUTS];
    int16_t output;
    float param[AUDIODSP_GRAPH_MAX_PARAMS];
    // A parameter moved: derive the kernel's config before the next run.
    bool dirty;
    // Ran last block. A node skipped after running zeroes its output once.
    bool ran;
    // The kernel's config and state, sized by audiodsp_graph_node_data_size.
    void *data;
} audiodsp_graph_node_t;

typedef struct {
    uint32_t sample_rate;
    uint32_t frames;
    uint32_t channel_count;
    uint32_t nbufs;
    uint32_t nnodes;
    uint32_t max_nodes;
    // nbufs * frames samples, one block per buffer id.
    int16_t *bufs;
    audiodsp_graph_node_t *nodes;
    // Written by run(): groups with an envelope still sounding.
    uint32_t sounding;
} audiodsp_graph_t;

// Bytes a node of `kind` needs for its config and state. `arg` is the
// kind's build-time argument (the feedback delay's line length in ms).
size_t audiodsp_graph_node_data_size(audiodsp_graph_kind_t kind,
    uint32_t sample_rate, float arg);

// `bufs` holds nbufs * frames samples and `nodes` holds max_nodes entries;
// both are the caller's.
void audiodsp_graph_init(audiodsp_graph_t *graph, uint32_t sample_rate,
    uint32_t frames, uint32_t channel_count, int16_t *bufs, uint32_t nbufs,
    audiodsp_graph_node_t *nodes, uint32_t max_nodes);

// Appends a node. `data` is the caller's, of audiodsp_graph_node_data_size
// bytes, zeroed. Returns the node's index, or -1 when the graph is full, a
// buffer id is out of range, or a node other than OUT has no output buffer
// (every other kind writes one; OUT writes the caller's buffer instead).
int audiodsp_graph_add(audiodsp_graph_t *graph, audiodsp_graph_kind_t kind,
    uint32_t group, const int16_t *inputs, uint32_t ninputs, int16_t output,
    void *data, float arg);

// Stores one parameter. False for a bad node or index.
bool audiodsp_graph_set(audiodsp_graph_t *graph, uint32_t node,
    uint32_t index, float value);

// Reads one back.
bool audiodsp_graph_get(const audiodsp_graph_t *graph, uint32_t node,
    uint32_t index, float *value);

// The feedback delay's wow table; borrowed, as the kernel borrows it.
bool audiodsp_graph_set_table(audiodsp_graph_t *graph, uint32_t node,
    const int16_t *table, uint32_t length);

// Clears a node's running state (the kernel's reset).
bool audiodsp_graph_reset(audiodsp_graph_t *graph, uint32_t node);

// Renders one block. `out` takes frames * channel_count samples from the
// OUT node (and is untouched when there is none). Nodes whose group bit is
// clear in `enable` are skipped; AUDIODSP_GRAPH_ALWAYS nodes always run.
// Returns the mask of groups whose envelopes are still sounding.
uint32_t audiodsp_graph_run(audiodsp_graph_t *graph, int16_t *out,
    uint32_t enable);

// A buffer's peak, for the caller's silence detection and for tests.
int32_t audiodsp_graph_peak(const audiodsp_graph_t *graph, uint32_t buffer);
