// Runtime-neutral graph runner: one C call renders a whole patch per block.
//
// New code -- not a CircuitPython port. A polyphonic patch built from the
// kernels in this directory runs in real time on an ESP32-P4 once the
// per-node Python between them is gone: this is the C that runs between
// them instead. The bindings are src/audiograph/ (MicroPython),
// src/circuitpython_spike/shared-bindings/audiograph/ (CircuitPython) and the
// Graph type in src/cpython/_audiodsp.c (CPython).
//
// The runner owns the block buffers. A graph is a list of nodes in the
// order they run; each node names the buffers it reads and the buffer it
// writes, and holds the config and state of one kernel. The kernels are the
// ones the audio nodes already call -- the oscillator, the ladder, the float
// biquad, the feedback delay, the all-pass cascade, the sample-and-hold --
// plus a few small ones a voice needs that only existed in Python: an ADSR,
// an LFO, white noise, a mixer with ramped gains, a VCA, a glide and the
// output stage.
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
// Two ways to drive a graph. A caller can render it where it likes with
// run(), into a buffer it owns. Or the bindings hand it to an audio output as
// an audiosample, and the output's pump renders it on its own thread, one
// block per pull. Either way every block goes through audiodsp_graph_render,
// so the two paths are the same code.
//
// The control side never writes a node's parameters directly. set(), reset()
// and enable() push fixed records onto a single-producer, single-consumer
// queue, and the renderer drains the queue at the top of each block. So each
// change lands on a block boundary, a batch held and committed together lands
// in one block, and the producer never waits for a render to finish. The
// renderer publishes, after each block, the groups still sounding, a frame
// counter and how many records it has applied (32-bit words, release
// stores), which the control side reads without a lock.
//
// No pump lock in here: this file builds into CircuitPython, which has none.
// When the queue is full the producer drains it itself, and the binding says
// how to exclude the renderer while it does (audiodsp_graph_enqueue's `lock`
// and `unlock`; the pump lock on MicroPython and CPython, nothing on
// CircuitPython, where the output pulls on the interpreter's own thread).

// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shared/audiodsp_filter_f32.h"
#include "shared/audiodsp_feedback_delay.h"
#include "shared/audiodsp_ladder.h"
#include "shared/audiodsp_oscillator.h"
#include "shared/audiodsp_samplehold.h"

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
    // params: rate Hz, amplitude (counts), shape. out: +-amplitude, the
    // shape one of AUDIODSP_GRAPH_LFO_TRIANGLE..RANDOM from one phase.
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
    // Added after OUT so the kinds above keep their numbers.
    //
    // inputs: audio. params: audiodsp_allpass_f32_option_t (frequency,
    // feedback, mix). audiobiquad.AllPass on one channel: first-order
    // all-pass stages with one feedback around the chain and a dry/wet mix.
    // Built with `arg` = stages (0 takes AllPass's default of 4).
    AUDIODSP_GRAPH_PHASER,
    // inputs: note, pitch, legato. params: time s, mode, shape. A glide on a
    // pitch CV: the note's first sample each block is the target, and the
    // bend (pitch - note) rides on top unslid. vrack's dsp/glide.py
    // GlideCore (the linear shape) or dsp/utility.py's (the one-pole shape).
    AUDIODSP_GRAPH_LAG,
    // inputs: signal, clock. params: lag coefficient, rate Hz. Holds the
    // signal at each rising edge of the clock (vrack's SampleAndHoldCore);
    // with the clock open and a rate set, audioshaper's sample-and-hold
    // kernel refreshes it that many times a second instead.
    AUDIODSP_GRAPH_SAMPLE_HOLD,
    // inputs: audio, cutoff CV. params: type, cutoff Hz, depth, resonance,
    // drive. The three two-pole filters of vrack's dsp/twopole.py, which
    // clip inside their loops: the Sallen-Key low-pass and high-pass with
    // diode-clipped peak feedback, and the CMOS state-variable filter in
    // low-pass, band-pass and high-pass. The cutoff CV is read at the
    // block's first and last samples and the coefficient walks between them.
    AUDIODSP_GRAPH_TWOPOLE,
    // inputs: audio. params: drive, tone, level. vrack's OverdriveCore:
    // audiofilters.Distortion's soft-clipped overdrive, then a high shelf
    // (synthio.Biquad, CircuitPython's own Q15 arithmetic) whose gain tone
    // sets.
    AUDIODSP_GRAPH_OVERDRIVE,
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
    AUDIODSP_GRAPH_LFO_SHAPE,
};

// LFO shapes, all from one phase that starts at 0 and has advanced one step
// by the first sample, so nodes of different shapes at one rate stay in step.
enum {
    // 1 - |4p - 2|: the bottom at p = 0, the top at p = 0.5.
    AUDIODSP_GRAPH_LFO_TRIANGLE = 0,
    // sin(2 pi p).
    AUDIODSP_GRAPH_LFO_SINE,
    // 1 - 2p, falling.
    AUDIODSP_GRAPH_LFO_SAW,
    // +1 for the first half of the cycle, -1 for the second.
    AUDIODSP_GRAPH_LFO_SQUARE,
    // A new uniform level at each rising edge of the square (the start of
    // each cycle), held for the cycle.
    AUDIODSP_GRAPH_LFO_RANDOM,
    AUDIODSP_GRAPH_LFO_SHAPES,
};

enum {
    AUDIODSP_GRAPH_LAG_TIME = 0,
    AUDIODSP_GRAPH_LAG_MODE,
    AUDIODSP_GRAPH_LAG_SHAPE,
};

// LAG modes, GlideCore's numbers: the linear shape only.
enum {
    AUDIODSP_GRAPH_LAG_OFF = 0,
    AUDIODSP_GRAPH_LAG_ON,
    // Glide only when the legato input is above 0.5 at the block's start.
    AUDIODSP_GRAPH_LAG_AUTO,
};

// LAG shapes.
enum {
    // Constant time: a straight line to each new note taking `time` seconds.
    AUDIODSP_GRAPH_LAG_LINEAR = 0,
    // A one-pole approach, the coefficient exp(-1 / (time * rate * 0.35)).
    AUDIODSP_GRAPH_LAG_ONE_POLE,
};

enum {
    // The per-sample one-pole coefficient the output slews with toward the
    // held value: 0 none, 0.999 very slow.
    AUDIODSP_GRAPH_SH_LAG = 0,
    // Refreshes a second when the clock input is open; 0 holds for ever.
    AUDIODSP_GRAPH_SH_RATE,
};

enum {
    AUDIODSP_GRAPH_TWOPOLE_TYPE = 0,
    // Hz at a cutoff CV of 0.
    AUDIODSP_GRAPH_TWOPOLE_CUTOFF_HZ,
    // vrack's mod_depth: the cutoff is CUTOFF_HZ * 2^(cv / UNIT * depth *
    // 10), so 1 is an octave per 1638.4 counts of CV and 0 ignores it.
    AUDIODSP_GRAPH_TWOPOLE_DEPTH,
    // The Sallen-Key's peak, or the CMOS filter's resonance: 0..1.
    AUDIODSP_GRAPH_TWOPOLE_RESONANCE,
    // Gain ahead of the filter, as a multiplier.
    AUDIODSP_GRAPH_TWOPOLE_DRIVE,
};

// TWOPOLE types.
enum {
    // SallenKeyLPFCore: 12 dB/oct, the output through tanh.
    AUDIODSP_GRAPH_TWOPOLE_SK_LOWPASS = 0,
    // SallenKeyHPFCore.
    AUDIODSP_GRAPH_TWOPOLE_SK_HIGHPASS,
    // CMOSFilterCore's modes: CMOS inverter clipping in every stage.
    AUDIODSP_GRAPH_TWOPOLE_CMOS_LOWPASS,
    AUDIODSP_GRAPH_TWOPOLE_CMOS_BANDPASS,
    AUDIODSP_GRAPH_TWOPOLE_CMOS_HIGHPASS,
    AUDIODSP_GRAPH_TWOPOLE_TYPES,
};

enum {
    // 0..1; drive 0..1 is a pre-gain of 0..20 dB.
    AUDIODSP_GRAPH_OVERDRIVE_DRIVE = 0,
    // 0..1: a high shelf from -12 dB through flat at 0.5 to +12 dB.
    AUDIODSP_GRAPH_OVERDRIVE_TONE,
    // A linear gain after the clipper, down to 1e-4.
    AUDIODSP_GRAPH_OVERDRIVE_LEVEL,
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

// The queue's records. 8 bytes each.
enum {
    AUDIODSP_GRAPH_OP_SET = 0,
    AUDIODSP_GRAPH_OP_RESET,
    AUDIODSP_GRAPH_OP_ENABLE,
};

typedef struct {
    uint8_t op;
    uint8_t index;
    uint16_t node;
    union {
        float value;
        uint32_t mask;
    } arg;
} audiodsp_graph_record_t;

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
    // The groups audiodsp_graph_render runs. Only the renderer writes it,
    // from an ENABLE record; 0xFFFFFFFF until one arrives.
    uint32_t enable;
    // Published after each render, release-stored in this order: sounding,
    // then frame, then applied. A reader that loads applied first and
    // sounding last gets a mask at least as new as the count.
    uint32_t pub_sounding;
    uint32_t pub_frame;
    uint32_t pub_applied;
    // Records applied so far; the renderer's own copy of pub_applied.
    uint32_t applied;
    // The queue, attached by audiodsp_graph_queue_init. Free-running
    // indices; a record lives at index % capacity.
    audiodsp_graph_record_t *records;
    uint32_t capacity;
    uint32_t head;       // producer only: written, published or not
    uint32_t published;  // producer release-stores, renderer acquire-loads
    uint32_t tail;       // renderer release-stores, producer acquire-loads
    bool held;           // producer only: hold() is in force
    // Producer-side counts: records pushed, drains forced by a full queue,
    // held batches split by one, and the most records ever waiting.
    uint32_t pushed;
    uint32_t overflows;
    uint32_t splits;
    uint32_t high_water;
    // max_nodes * AUDIODSP_GRAPH_MAX_PARAMS floats: what set() last stored,
    // so get() reads back the same value whoever renders. NULL for none.
    float *shadow;
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

// --- the control queue ------------------------------------------------------

//: The queue's size when the caller names none: max(256, 4 * nodes).
#define AUDIODSP_GRAPH_QUEUE_MIN 256u

// Attaches the queue's `capacity` records and the shadow table (max_nodes *
// AUDIODSP_GRAPH_MAX_PARAMS floats, or NULL), both the caller's. Call once,
// after audiodsp_graph_init; the shadow takes each node's defaults as it is
// added.
void audiodsp_graph_queue_init(audiodsp_graph_t *graph,
    audiodsp_graph_record_t *records, uint32_t capacity, float *shadow);

// True when (node, index) names a parameter set() may store, and
// (node) a node reset() may clear when `index` is 0.
bool audiodsp_graph_valid(const audiodsp_graph_t *graph, uint32_t node,
    uint32_t index);

// Pushes one record from the control side. The caller has validated it.
// When the queue is full, publishes what is waiting, takes `lock` (NULL for
// none), drains the queue itself, and drops it: stores only, so it holds
// the pump lock's contract. Unless hold() is in force, the record is
// published at once. A SET also writes the shadow.
void audiodsp_graph_enqueue(audiodsp_graph_t *graph, uint8_t op,
    uint32_t node, uint32_t index, float value, uint32_t mask,
    void (*lock)(void), void (*unlock)(void));

// hold() and commit(): records pushed between the two are published
// together, so they land in one block. commit() without hold() is harmless.
void audiodsp_graph_hold(audiodsp_graph_t *graph);
void audiodsp_graph_commit(audiodsp_graph_t *graph);

// The renderer's side: applies every published record. Returns how many.
uint32_t audiodsp_graph_drain(audiodsp_graph_t *graph);

// Drains, renders one block with the graph's own enable mask, and publishes
// sounding, the frame counter and the applied count. Returns sounding.
uint32_t audiodsp_graph_render(audiodsp_graph_t *graph, int16_t *out);

// The same, with the caller's mask for this block (run()'s `enable`).
uint32_t audiodsp_graph_render_mask(audiodsp_graph_t *graph, int16_t *out,
    uint32_t enable);

// The published words, loaded applied first and sounding last.
void audiodsp_graph_published(const audiodsp_graph_t *graph,
    uint32_t *sounding, uint32_t *frame, uint32_t *applied);

// get() for the bindings: the shadow's value when there is a shadow.
bool audiodsp_graph_get_shadow(const audiodsp_graph_t *graph, uint32_t node,
    uint32_t index, float *value);
