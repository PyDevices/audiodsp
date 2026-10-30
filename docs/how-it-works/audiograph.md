# The graph runner: `audiograph`

`audiograph.Graph` renders a whole patch in one C call per block. You build
the graph once, from nodes that call the kernels audiodsp already has (the
oscillator, the ladder, the float biquad, the feedback delay, the all-pass
cascade, the sample-and-hold) and a few small ones a voice needs (an envelope,
an LFO, noise, a mixer, a VCA, a glide, a clipping two-pole filter, an
overdrive), then call `run()` once a block. Nothing in Python runs between the nodes, which is what
lets a six-voice polysynth built from these kernels play in real time on an
ESP32-P4, where the same patch run node by node from Python could not.

It is the same module on CPython (in the `pydevices-audiodsp` wheel),
MicroPython and CircuitPython, and the same C underneath, so a graph renders
the same samples on all three: you can build and listen to a patch on the
desktop and get the board's render, sample for sample.

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
| `OSC` | pitch, fm, pwm, sync | `OSC_FREQUENCY`, `OSC_WAVEFORM` (1 saw, 2 pulse, 4 triangle, 8 sine; a sum plays those added), `OSC_PULSE_WIDTH`, `OSC_LEVEL`, `OSC_PITCH_OCTAVES`, `OSC_FM_DEPTH`, `OSC_PWM_DEPTH` | audiooscillator's band-limited waveform |
| `LADDER` | audio, cutoff | `LADDER_CUTOFF_HZ`, `LADDER_RESONANCE`, `LADDER_DRIVE`, `LADDER_POLES`, `LADDER_PASSBAND_COMP`, `LADDER_OVERSAMPLE`, `LADDER_MIX`, `LADDER_CUTOFF_OCTAVES` | audioladder's filter, the cutoff input read every sample |
| `BIQUAD` | audio | `BIQUAD_MODE`, `BIQUAD_FREQUENCY`, `BIQUAD_Q`, `BIQUAD_GAIN_DB`, `BIQUAD_MIX` | audiobiquad's float biquad |
| `FBDELAY` | audio | `DELAY_MS`, `DELAY_FEEDBACK`, `DELAY_MIX`, `DELAY_DAMPING_HZ`, `DELAY_WOW_HZ`, `DELAY_WOW_DEPTH_MS` and audioecho's other options by number; built with `arg` = the line's length in ms; `table(node, int16_array)` sets its wow shape | audioecho's FeedbackDelay |
| `ENV` | gate | `ENV_ATTACK`, `ENV_DECAY`, `ENV_RELEASE` in seconds, `ENV_SUSTAIN` 0..1 | an ADSR, 0..`UNIT`, each stage a one-pole approach, the attack aimed past 1.0 so it arrives with a slope |
| `LFO` | | `LFO_RATE` Hz, `LFO_AMPLITUDE` counts, `LFO_SHAPE` (`LFO_TRIANGLE`, `LFO_SINE`, `LFO_SAW`, `LFO_SQUARE`, `LFO_RANDOM`) | that shape, ±amplitude, [below](#lfo-shapes) |
| `NOISE` | | 0: amplitude in counts | white noise |
| `MIX` | up to eight | 0..7: a gain per input, `MIX_OFFSET` in counts | offset + the gains times the inputs; a gain that moved ramps across the block |
| `VCA` | audio, cv_a, cv_b | `VCA_GAIN`, `VCA_SELECT` (cv_a below 0.5, else cv_b), `VCA_CURVE` (`VCA_LINEAR`, `VCA_EXPONENTIAL`) | audio times the chosen CV over `UNIT`, times the gain; exponential is 60 dB over the CV, `10^(3·(cv/UNIT − 1))`, unity at `UNIT` |
| `OUT` | left, right | 0: scale | writes the run's output buffer: interleaved stereo, or the mean when the graph is mono |
| `PHASER` | audio | `PHASER_FREQUENCY` Hz, `PHASER_FEEDBACK` −0.99..0.99, `PHASER_MIX` 0..1; built with `arg` = stages (0 is 4) | audiobiquad's `AllPass` on one channel: first-order all-pass stages, one feedback around the chain, a dry/wet mix |
| `LAG` | note, pitch, legato | `LAG_TIME` s, `LAG_MODE` (`LAG_OFF`, `LAG_ON`, `LAG_AUTO`), `LAG_SHAPE` (`LAG_LINEAR`, `LAG_ONE_POLE`) | the note glided, with the bend (pitch − note) on top, [below](#glide) |
| `SAMPLE_HOLD` | signal, clock | `SH_LAG` coefficient 0..1, `SH_RATE` Hz | the signal held at each rising edge of the clock, or `SH_RATE` times a second with the clock open, [below](#sample-and-hold) |
| `TWOPOLE` | audio, cutoff CV | `TWOPOLE_TYPE` (`TWOPOLE_SK_LOWPASS`, `TWOPOLE_SK_HIGHPASS`, `TWOPOLE_CMOS_LOWPASS`, `TWOPOLE_CMOS_BANDPASS`, `TWOPOLE_CMOS_HIGHPASS`), `TWOPOLE_CUTOFF_HZ`, `TWOPOLE_DEPTH`, `TWOPOLE_RESONANCE` 0..1, `TWOPOLE_DRIVE` | a 12 dB/octave filter with its nonlinearity inside the loop, [below](#two-pole-filters) |
| `OVERDRIVE` | audio | `OVERDRIVE_DRIVE` 0..1, `OVERDRIVE_TONE` 0..1, `OVERDRIVE_LEVEL` (linear, 1 is unity) | `audiofilters.Distortion`'s soft-clipped overdrive, then a high shelf, [below](#overdrive) |
| `TANH` | audio | `TANH_DRIVE` (a gain before the tanh, 1 is unity), `TANH_LEVEL` (a gain after it, 1 is unity) | `TANH_LEVEL · tanh(TANH_DRIVE · x)` with `x` in units of `UNIT`, [below](#tanh) |
| `PAN` | up to eight | 0..7: a gain per input, `PAN_PAN + i`: input `i`'s pan, −1 (left) to 1 (right); built with `arg` = the right side's buffer | a stereo mixer, the left side to the node's output and the right to the buffer `arg` names, [below](#pan) |
| `WAVEFOLD` | audio, fold CV | `WAVEFOLD_FOLD` 0..1, `WAVEFOLD_SYMMETRY` −1..1 | a West Coast wavefolder: three sine folds crossfaded with the input by the fold, [below](#wavefold) |

The kernel nodes take their parameters under the same numbers as the
modules that wrap them (`audiooscillator`, `audioladder`, `audiobiquad`,
`audioecho`, and `audiobiquad.AllPass` for `PHASER`), re-exported on
`audiograph` so a caller needs no other module.
An input left `None` is open: an oscillator without a pitch input plays
`OSC_FREQUENCY`, a ladder without a cutoff input sits at `LADDER_CUTOFF_HZ`.

### LFO shapes

Every shape comes from one phase that starts at 0 and has taken one step by
the first sample, so LFOs of different shapes at one rate stay in step: a saw
and a square at 3 Hz are one LFO's two outputs. The triangle is `1 - |4p - 2|`
(its bottom at the start of the cycle), the sine `sin(2πp)`, the saw `1 - 2p`
(falling), the square +1 for the first half of the cycle and −1 for the
second. `LFO_RANDOM` draws a new level, uniform in ±amplitude, at each rising
edge of that square (the start of each cycle) and holds it; choosing it draws
one at once if the square is high. The sine is a polynomial rather than the
C library's, so every target computes the same one.

The phase adds a float step each sample, which runs up to 0.06% off the rate
over a long render: at 1 Hz, 0.0065 of a cycle in ten seconds.

### Glide

`LAG` glides a pitch CV to each new note. The target is the note input's first
sample in each block, so a note that steps at a block boundary glides exactly.
With `LAG_LINEAR` (the default) the glide is a straight line that takes
`LAG_TIME` seconds wherever it starts from, and a new note in mid-glide starts
a new one from where the pitch is. `LAG_MODE` is `LAG_ON` (the default),
`LAG_OFF`, or `LAG_AUTO`, which glides only when the legato input is above 0.5
at the start of the block. `LAG_ONE_POLE` is a one-pole approach instead, with
the coefficient `exp(-1 / (time * sample_rate * 0.35))`; it ignores the mode,
and passes the note through at a time of a millisecond or less. When the pitch
input is connected, the output is the glided note plus (pitch − note), so a
bend rides on top of the glide without being slowed by it. `reset(node)` makes
the next block start on its note.

### Sample and hold

`SAMPLE_HOLD` latches its signal at every rising edge of its clock (above 0.1
on the wire, as an envelope's gate is) and holds it, two edges in one block
included. `SH_LAG` above 0 slews the output toward the held value with a
one-pole of that per-sample coefficient (0.999 is very slow). With the clock
input open and `SH_RATE` set, the node refreshes that many times a second
instead, through audioshaper's `SampleHold` kernel (whole hertz, exact for
ever); with neither, it holds.

### Two-pole filters

`TWOPOLE` is three filters that keep their character in a clipper inside the
feedback loop, so a biquad cannot stand in for them: the Sallen-Key low-pass
(`TWOPOLE_SK_LOWPASS`) and high-pass (`TWOPOLE_SK_HIGHPASS`), whose peak feedback
runs through a diode clipper and whose output goes through `tanh`; and the CMOS
state-variable filter, with a clipping inverter in every stage, as a low-pass,
band-pass or high-pass (`TWOPOLE_CMOS_*`). They are vrack's `SallenKeyLPFCore`,
`SallenKeyHPFCore` and `CMOSFilterCore`, one node with a type so a patch can
change the type of a running filter and the cutoff code is one function.

`TWOPOLE_RESONANCE` is the Sallen-Key's peak or the CMOS filter's resonance,
both 0..1. `TWOPOLE_DRIVE` multiplies the input before the filter. The cutoff
is `TWOPOLE_CUTOFF_HZ` times two to the power of the cutoff CV over `UNIT`
times `TWOPOLE_DEPTH` times ten, so a depth of 1 is an octave per 1638.4 counts
of CV, 0 ignores the input, and the result is held between 20 Hz and 0.45 of
the sample rate. The CV is read at the block's first and last samples, and the
filter's coefficient walks in a straight line between them, as these filters
have always taken it: what lies between those two samples does not move the
cutoff. An open CV input leaves the cutoff where it is.

### Overdrive

`OVERDRIVE` is `audiofilters.Distortion` in overdrive mode with soft clipping,
followed by a high shelf at 1.5 kHz, the way vrack's `OverdriveCore` chains them.
`OVERDRIVE_DRIVE` 0..1 sets a pre-gain of up to 20 dB, `OVERDRIVE_LEVEL` is the
linear gain after the clipper, and `OVERDRIVE_TONE` 0..1 is the shelf's gain,
from −12 dB through flat at 0.5 to +12 dB. The shelf is `synthio.Biquad`'s: the
node runs CircuitPython's own Q15 arithmetic, as `audiofilters.Filter` does, and
squeezes a sample past ±28000 toward the rail as that filter's last step does,
so a loud signal tops out at 28521.

CircuitPython's `audiograph` has no `OVERDRIVE`. The node computes its gains
in double precision, and the RP2040 build has no double-precision `pow` or
`log10` to link against. Check with `hasattr(audiograph, "OVERDRIVE")`. On
CircuitPython, `audiofilters.Distortion` and `synthio.Biquad` are already built
in, and you can chain them yourself.

### Tanh

`TANH` is `level * tanh(drive * x)`, where `x` is the input over `UNIT`, so a
full-scale input at a drive of 1 is 0.76 of the way to the rail and a drive of 4
or more squares a signal off. It is for a mixer that saturates, or the input stage
in front of a ladder, where `OVERDRIVE` (Q15 distortion and a shelf) is the wrong
shape. The tanh is the graph's own, the one `TWOPOLE` clips with: a rational
function good to a few float ulps, with no libm, so MicroPython, CircuitPython and
CPython render the same bytes. It is present on every target, `OVERDRIVE`'s
RP2040 limit not applying. A drive large enough to push the result past ±32767 is
held at the rail, and an open input is silence.

### Pan

`PAN` is a stereo mixer with a balance on each input, and the one node that
writes two buffers: its output is the left side, and the buffer you pass as
`arg` is the right.

```python
left, right = g.buffer(), g.buffer()
pan = g.add(audiograph.PAN, group, (voice1, voice2), left, right)
g.set(pan, 1, 0.5)                       # voice 2 at half gain
g.set(pan, audiograph.PAN_PAN + 1, 0.25)  # and a quarter to the right
```

The balance is linear: an input panned right by `p` reaches the left side at
`gain * (1 - p)` and the right side at full `gain`, and panned left the other
way round, so a centred input is at `gain` on both sides. A pan past −1 or 1
stops there. Each side's gain ramps across the block when the gain or the pan
moved, as `MIX`'s gains do, so a voice moved across the field or faded out
does not step; `reset(node)` takes the gains as they stand, with no ramp. The
gains start at 1 and the pans at 0. When the node's group is switched off,
both sides fall silent. `add()` refuses a right side that is not a buffer of
the graph, or is the node's own output.

Two `MIX` nodes given the same gains, one a side, render the same samples,
so `PAN` saves a node and a read of every input rather than changing the sound.

### Wavefold

`WAVEFOLD` is a West Coast wavefolder, vrack's `WavefolderCore`. The input,
offset by a quarter of `WAVEFOLD_SYMMETRY`, is driven by `1 + 7 * fold` into
three sine folds, `f1 = sin(x·π/2)`, `f2 = sin((1.5·f1 + 0.5·x)·π/2)` and
`f3 = sin((1.3·f2 + 0.4·f1)·π/2)`, and the output is the input crossfaded with
half of `f3` by the fold. A fold CV on the second input, in units of `UNIT`,
adds to `WAVEFOLD_FOLD` every sample and is not clamped, as the rack's isn't.
With the fold at 0.001 or under and no CV connected, the input passes through
untouched; an open audio input is silence, which the symmetry still offsets.

The arithmetic is float32, operation for operation as the rack's numpy does
it, and the sine is the graph's own polynomial rather than libm's, so every
target renders the same bytes and the node is present on CircuitPython too.
Against the rack's arithmetic with a correctly rounded sine it is within one
count, on about one sample in a thousand.

## Output

`run(out, block, enable)` writes the `OUT` node's block into `out` at block
`block` (each block is `frames * channel_count` samples), so one buffer
can collect several blocks before it goes to a ring. `out` may be `None`
to run the graph for its state alone. It raises `ValueError` when `out`
cannot hold that block.

## Playing a graph from the pump

A graph is also an audio sample. Hand it to the pump, to an output's
`play()`, or to a mixer voice, and it renders a block each time one is pulled,
on whichever thread or core pulls it. On a board whose pump has a core of its
own, the whole patch then renders there, and your code only changes it:

```python
import audiopump

g = ag.Graph(sample_rate=48000, frames=128, channel_count=2)
# ... build it as above ...
status = bytearray(audiopump.STATUS_BYTES)
audiopump.spawn(g, 1 << 30, status, ring=ring)    # the pump pulls it now

g.hold()                                          # a patch change, as one
g.set(f, ag.LADDER_CUTOFF_HZ, 900.0)
g.set(f, ag.LADDER_RESONANCE, 0.6)
g.enable(0b11)                                    # groups 0 and 1 play
g.commit()                                        # all of it on one block
```

`set()`, `reset()` and `enable()` don't touch the running graph. Each one
goes into the graph's queue, and the renderer applies everything waiting at
the top of the next block it renders, so a change never lands halfway through
a block. Between `hold()` and `commit()` nothing is released, so a batch (a
patch change, a chord) lands on one block together. A change made without
`hold()` is released at once and lands on the next block.

`get()` reads back the last value `set()` stored, whether or not a block has
heard it yet. `sounding()` returns `(sounding, frame, applied, pushed)`: the
groups still sounding after the last block, the frames rendered so far, and
how many changes have been applied and queued. A change counted in `pushed`
but not yet in `applied` hasn't been heard. Read `sounding` and `frame`
together and you know which block the mask belongs to, so code that times
notes against the audio can count blocks from `frame`.

The queue holds `queue` changes (`Graph(queue=...)`; by default the larger of
256 and four per node). When it fills, the control side waits for the pump to
finish the block it is rendering and applies what is waiting itself, so
nothing is dropped; a held batch that overflowed lands across two blocks
instead of one. `stats()` counts both, as `overflows` and `splits`, with the
queue's `high_water`. Size the queue for your biggest batch and they stay 0.

While the pump is pulling a graph, the calls that change its shape (`buffer()`,
`add()`, `table()`, `write()`) and a `run()` of your own raise
`RuntimeError`. Build the graph before you hand it over. `read()` and `peak()`
still work, but they can catch a buffer while the pump is rendering into it,
which is fine for a meter and not for anything that needs the whole block.
`deinit()` takes the pump's lock, so a graph released while the pump is
pulling it stops cleanly.

With nobody else pulling, `run(out, block, enable)` still works as it always
has, and drains the queue first, so a graph you drive yourself sees changes
the same way.

## Limits, and what is checked

A block is at most 256 frames, a node has at most eight inputs and sixteen
parameters, and a group is 0 to 31. A graph refuses a node with nothing to
write (every kind but `OUT` writes a buffer), an input or output id past
its buffers, and a node past its table.

[`tests/parity/graph_source_probe.py`](../../tests/parity/graph_source_probe.py) renders one script of changes three
ways: through `run()`, pulled as a sample with batches held and committed,
and pulled with a queue so small that every patch change overflows it. The
three must agree to the sample, block by block, and `verify_graph.py`
compares the pulled render across interpreters as well. Its planted faults
drain the queue after the render instead of before and publish a held batch
in two halves. `tests/pump/pump_probe.py graph` hands a graph to the pump and
changes it from another thread: every block holds one value per channel from
one batch, a held batch is silent until `commit()`, and a committed one sounds
no later than the block after the one in flight. Where the pump has a thread
of its own, 3000 batches are committed while it pulls, and none may tear.
The graph also goes through the pump's identity and heap-locked allocation
gates.

[`tests/parity/graph_probe.py`](../../tests/parity/graph_probe.py) holds the runner to its wiring (a value set
reaches the output in the same run, through every node between), to its
nodes doing their jobs (the ladder closes a sawtooth, a `set()` after the
first run lands), to its groups (sounding is reported while an envelope
sounds and a masked group goes silent at the mixer), to the mixer's gain
ramp and to the output node. Each planted fault, a mixer added before the
node it reads, a filter left out of the chain, a cutoff sent to a node no
output reads, makes it fail; CI runs the faults first.

[`tests/parity/verify_graph.py`](../../tests/parity/verify_graph.py) renders two probes on CPython and on each
interpreter you name, and compares every sample. `graph_parity_probe.py` is one
patch with the runner's first node kinds in it (two voices with their groups
parking, a fed buffer, moved gains and cutoffs, a delay reset mid-tail, a mono
graph); `graph_nodes_probe.py` is the phaser, the glide, every LFO shape, the
sample-and-hold, the five two-pole types, the overdrive, the pan and the
wavefolder. The tolerance is 0 counts: the three compile the same C
and are handed the same floats, so any difference is a defect rather than
rounding. Each planted fault makes it fail: one input sample one count off, one
`set()` a block late, a phaser's frequency a block late, a legato flag flipped,
a shape switched a block late, a clock edge a sample late, a cutoff CV a block
late, an overdrive's tone a block late, a pan a block late, a wavefolder's
symmetry a block late. CI runs it against
MicroPython in both float widths; the three-way with the CircuitPython unix
build is a local run, where the overdrives are left out of CircuitPython's
comparison because it has no `OVERDRIVE`.

[`tests/test_cpython_audiograph_nodes.py`](../../tests/test_cpython_audiograph_nodes.py) holds the newer nodes to what they
stand in for: `PHASER` to `audiobiquad.AllPass` at 0 counts, the glide, the
LFO's shapes, the sample-and-hold and the two-pole filters to the Python cores of
the same name in vrack, and `OVERDRIVE` to `OverdriveCore`'s own chain of
`audiofilters` nodes, with each bar in the file's table. The CMOS filter and
the overdrive match at 0 counts. The Sallen-Key pair matches within one count,
on under one sample in a hundred: the node's `tanh` is a rational function in
float that is the same on every target, where vrack's is the C library's in
double. `WAVEFOLD` is held to `WavefolderCore` transcribed in float32 with a
correctly rounded sine, within one count.
