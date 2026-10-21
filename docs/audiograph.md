# The graph runner: `audiograph`

`audiograph.Graph` renders a whole patch in one C call per block. You build
the graph once, from nodes that call the kernels audiodsp already has (the
oscillator, the ladder, the float biquad, the feedback delay) and a few small
ones a voice needs (an envelope, an LFO, noise, a mixer, a VCA), then call
`run()` once a block. Nothing in Python runs between the nodes, which is what
lets a six-voice polysynth built from these kernels play in real time on an
ESP32-P4, where the same patch run node by node from Python could not.

It is MicroPython only for now; the CPython and CircuitPython bindings are
on the [roadmap](../ROADMAP.md).

```python
import audiograph as ag

g = ag.Graph(sample_rate=48000, frames=128, channel_count=2)
pitch, gate, env, saw, cv, vcf, out = (g.buffer() for _ in range(7))

p = g.add(ag.CONST, 0, (), pitch)                 # the key, in octaves from C4
k = g.add(ag.CONST, 0, (), gate)
e = g.add(ag.ENV, 0, (gate,), env)
o = g.add(ag.OSC, 0, (pitch, None, None, None), saw)
g.set(o, ag.OSC_WAVEFORM, 1)
f = g.add(ag.LADDER, 0, (saw, env), vcf)          # the envelope sweeps the cutoff
g.set(f, ag.LADDER_CUTOFF_HZ, 600.0)
v = g.add(ag.VCA, 0, (vcf, env, None), out)
g.add(ag.OUT, ag.ALWAYS, (out, out), None)

pcm = bytearray(128 * 2 * 2)
g.set(k, 0, ag.UNIT)                              # key down
while True:
    sounding = g.run(pcm, 0)                      # one block into pcm
    ring.write(pcm)                               # an audiopump.Ring, say
```

## What a graph is

A graph owns `buffers` blocks of `frames` signed 16-bit samples and a table
of up to `nodes` nodes. Every node reads some buffers, writes one, belongs to
a group, and holds one kernel's settings behind up to sixteen float
parameters. `buffer()` hands out a fresh block's id; `add(kind, group,
inputs, output, arg=0.0)` appends a node and returns its index. Nodes run
in the order you added them, so add a node after the nodes it reads.

The buffers carry one wire: 16384 counts (`UNIT`) is 1.0, the +10 V of a
modular rack, so audio at ±0.5 is a quarter of full scale. Every node is
mono; a stereo patch has two buffers where it needs them. A block is 256
frames at most.

`set(node, index, value)` stores a parameter and marks the node to derive
its kernel's settings before the next run; `get(node, index)` reads one
back. A parameter that has not changed costs nothing per block, so a held
note is one `run()` and nothing else. `reset(node)` clears a node's running
state (the kernel's reset); `read(buffer, dst)` and `write(buffer, src)`
copy a block out of or into the graph, so Python can feed a buffer from
something the graph has no node for; `peak(buffer)` is a block's peak.

## Groups

Nodes belong to groups 0 to 31, or `ALWAYS`. `run(out, block, enable)` runs
every node whose group bit is set in `enable` and every `ALWAYS` node, and
returns the mask of groups whose envelopes are still sounding. Put each
voice of a polysynth in a group of its own: a voice nobody is playing is
masked out and costs nothing, and the block its amplitude envelope goes
idle, the returned mask tells you to park it. A node skipped after it ran
zeroes its output once, so a mixer that reads a parked voice hears silence
rather than its last block.

## The nodes

| kind | inputs | parameters | out |
|---|---|---|---|
| `CONST` | | 0: the value, in counts | that value every sample |
| `OSC` | pitch, fm, pwm, sync | `OSC_FREQUENCY`, `OSC_WAVEFORM` (1 saw, 2 pulse, 3 both), `OSC_PULSE_WIDTH`, `OSC_LEVEL`, `OSC_PITCH_OCTAVES`, `OSC_FM_DEPTH`, `OSC_PWM_DEPTH` | audiooscillator's band-limited waveform |
| `LADDER` | audio, cutoff | `LADDER_CUTOFF_HZ`, `LADDER_RESONANCE`, `LADDER_DRIVE`, `LADDER_POLES`, `LADDER_PASSBAND_COMP`, `LADDER_OVERSAMPLE`, `LADDER_MIX`, `LADDER_CUTOFF_OCTAVES` | audioladder's filter, the cutoff input read every sample |
| `BIQUAD` | audio | `BIQUAD_MODE`, `BIQUAD_FREQUENCY`, `BIQUAD_Q`, `BIQUAD_GAIN_DB`, `BIQUAD_MIX` | audiobiquad's float biquad |
| `FBDELAY` | audio | `DELAY_MS`, `DELAY_FEEDBACK`, `DELAY_MIX`, `DELAY_DAMPING_HZ`, `DELAY_WOW_HZ`, `DELAY_WOW_DEPTH_MS` and audioecho's other options by number; built with `arg` = the line's length in ms; `table(node, int16_array)` sets its wow shape | audioecho's FeedbackDelay |
| `ENV` | gate | `ENV_ATTACK`, `ENV_DECAY`, `ENV_RELEASE` in seconds, `ENV_SUSTAIN` 0..1 | an ADSR, 0..`UNIT`, each stage a one-pole approach, the attack aimed past 1.0 so it arrives with a slope |
| `LFO` | | `LFO_RATE` Hz, `LFO_AMPLITUDE` counts | a triangle |
| `NOISE` | | 0: amplitude in counts | white noise |
| `MIX` | up to eight | 0..7: a gain per input, `MIX_OFFSET` in counts | offset + the gains times the inputs; a gain that moved ramps across the block |
| `VCA` | audio, cv_a, cv_b | `VCA_GAIN`, `VCA_SELECT` (cv_a below 0.5, else cv_b) | audio times the chosen CV over `UNIT`, times the gain |
| `OUT` | left, right | 0: scale | writes the run's output buffer: interleaved stereo, or the mean when the graph is mono |

The kernel nodes take their parameters under the same numbers as the
modules that wrap them (`audiooscillator`, `audioladder`, `audiobiquad`,
`audioecho`), re-exported on `audiograph` so a caller needs no other module.
An input left `None` is open: an oscillator without a pitch input plays
`OSC_FREQUENCY`, a ladder without a cutoff input sits at `LADDER_CUTOFF_HZ`.

## Output

`run(out, block, enable)` writes the `OUT` node's block into `out` at block
`block` (each block is `frames * channel_count` samples), so one buffer
can collect several blocks before it goes to a ring. `out` may be `None`
to run the graph for its state alone. It raises `ValueError` when `out`
cannot hold that block.

## Limits, and what is checked

A block is at most 256 frames, a node has at most eight inputs and sixteen
parameters, and a group is 0 to 31. A graph refuses a node with nothing to
write (every kind but `OUT` writes a buffer), an input or output id past
its buffers, and a node past its table. The runner takes no lock: its
buffers and state are touched only by the thread that calls `run()`, and
its output goes into a buffer you own, so an `audiopump.Ring` downstream
keeps its own synchronisation.

`tests/parity/graph_probe.py` holds the runner to its wiring (a value set
reaches the output in the same run, through every node between), to its
nodes doing their jobs (the ladder closes a sawtooth, a `set()` after the
first run lands), to its groups (sounding is reported while an envelope
sounds and a masked group goes silent at the mixer), to the mixer's gain
ramp and to the output node. Each planted fault, a mixer added before the
node it reads, a filter left out of the chain, a cutoff sent to a node no
output reads, makes it fail; CI runs the faults first.
