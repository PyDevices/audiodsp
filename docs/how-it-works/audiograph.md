# The graph runner: `audiograph`

`audiograph.Graph` renders a whole patch in one C call per block. You build
the graph once, from nodes that call the kernels audiodsp already has (the
oscillator, the ladder, the float biquad, the feedback delay, the all-pass
cascade, the sample-and-hold) and a few small ones a voice needs (an envelope,
an LFO, noise, a mixer, a VCA, a glide, a clipping two-pole filter, an
overdrive), then call `run()` once a block. Nothing in Python runs between the nodes, which is what
lets a six-voice polysynth built from these kernels play in real time on an
ESP32-P4, where the same patch run node by node from Python could not.

It is the same module on CPython (in the `omnipython-audiodsp` wheel),
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
| `BIQUAD` | audio | `BIQUAD_MODE`, `BIQUAD_FREQUENCY`, `BIQUAD_Q`, `BIQUAD_GAIN_DB`, `BIQUAD_MIX`, `BIQUAD_RAMP` ms 0..1000 (default 20) | audiobiquad's float biquad, [walking to new coefficients](#biquad-moves) |
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
| `LPG` | audio, control, strike | `LPG_MODE` (`LPG_VCF`, `LPG_VCA`, `LPG_BOTH`), `LPG_DECAY` seconds, `LPG_MANUAL` 0..1, `LPG_LEVEL` | a vactrol low-pass gate, [below](#low-pass-gate) |
| `MODAL` | excitation, strike | `MODAL_FREQUENCY` Hz, `MODAL_DECAY` seconds, `MODAL_BRIGHTNESS` 0..1, `MODAL_INPUT`, `MODAL_MIX` 0..1, `MODAL_RATIO` + 0..7 | a struck bank of eight modes, [below](#modal-resonator) |
| `STRUCK` | strike, velocity | `STRUCK_FREQUENCY` Hz, `STRUCK_HARDNESS` 0..1, `STRUCK_LEVEL`, `STRUCK_SHAPE` (`STRUCK_HAMMER`, `STRUCK_PLUCK`); `table(node, modes)` sets up to `STRUCK_MODES` (24) `(ratio, decay, gain)` triples | a hammer/pluck exciter into a 24-mode struck bar bank, [below](#struck-bar) |
| `PICKUP` | audio | `PICKUP_MODE` (`PICKUP_ELECTROMAGNETIC`, `PICKUP_ELECTROSTATIC`), `PICKUP_DRIVE`, `PICKUP_OFFSET`, `PICKUP_LEVEL` | electromagnetic or electrostatic pickup nonlinearity with DC removal, [below](#pickup) |
| `TREMOLO` | audio, rate CV, depth CV | `TREMOLO_RATE` Hz, `TREMOLO_DEPTH` 0..1, `TREMOLO_MODE` (`TREMOLO_PAN`, `TREMOLO_AMPLITUDE`); built with `arg` = the right side's buffer | stereo auto-pan or mono amplitude tremolo into `(output, arg)`, [below](#tremolo) |
| `DECAY` | trigger, velocity | `DECAY_DECAY`, `DECAY_RESTRIKE`, `DECAY_ATTACK`, `DECAY_FAST`, `DECAY_FAST_SEC`, `DECAY_RISE` in seconds; `table(node, strikes)` sets up to `DECAY_STRIKES` (16) `(delay s, level)` pairs | a one-shot exponential envelope, a burst of re-strikes, or the two-stage, soft-attack and rise shapes the rack drums use, [below](#decay) |
| `PINK` | white | `PINK_LEVEL` | pink noise: Paul Kellet's pinking filter on the white input, [below](#pink) |
| `CRUSH` | audio | `CRUSH_BITS` 1..15 | the audio rounded to that many bits, [below](#crush) |
| `ACID` | audio, env, accent | `ACID_CUTOFF_HZ`, `ACID_RESONANCE` 0..1, `ACID_ENVMOD` 0..1 | acid303's diode ladder with its accent circuit, [below](#acid) |
| `SPRING` | audio | `SPRING_MIX` 0..1, `SPRING_DECAY` 0..1, `SPRING_DAMP` 0..1; built with `arg` = the right side's buffer | patch2600's two-tank spring reverb over `audiofreeverb`, wet-only into a dry/wet mix on both sides, [below](#spring) |
| `ADSR2` | gate | `ADSR2_ATTACK`, `ADSR2_DECAY`, `ADSR2_RELEASE` in seconds, `ADSR2_SUSTAIN` 0..1, `ADSR2_INITIAL`, `ADSR2_ATTACK_LEVEL` | ENV's contour starting at `ADSR2_INITIAL` and peaking at `ADSR2_ATTACK_LEVEL`: poly80's filter envelope, [below](#adsr2) |
| `GLISS` | note, pitch, legato | `GLISS_TIME` s, `GLISS_MODE` (`LAG_OFF`, `LAG_ON`, `LAG_AUTO`), `GLISS_STEP` (0.1 is a semitone at 1 V/octave) | `LAG`'s linear glide stepped onto a semitone grid once a block: poly80's glissando, [below](#gliss) |
| `FOLLOW` | audio | `FOLLOW_ATTACK`, `FOLLOW_RELEASE` in seconds, `FOLLOW_MODE` (`FOLLOW_PEAK`, `FOLLOW_RMS`) | an envelope follower, one level a block: clavichord's auto-wah peak and vocoder330's band RMS, [below](#follow) |
| `WAVETABLE` | audio | `WAVETABLE_FREQUENCY` in Hz; inputs pitch, gate and an extra signal added before the contacts; `table()` holds one cycle | a drawbar organ's voice, DrawbarToneCore without its latency, [below](#wavetable) |
| `ROTARY` | audio | `ROTARY_SPEED` (`ROTARY_STOP`, `ROTARY_SLOW`, `ROTARY_FAST`), `ROTARY_DRIVE` and `ROTARY_VOLUME` 0 to 10; `arg` is the right side's buffer | a rotary speaker, RotarySpeakerCore, stereo like `PAN`, [below](#rotary) |

The kernel nodes take their parameters under the same numbers as the
modules that wrap them (`audiooscillator`, `audioladder`, `audiobiquad`,
`audioecho`, and `audiobiquad.AllPass` for `PHASER`), re-exported on
`audiograph` so a caller needs no other module.
An input left `None` is open: an oscillator without a pitch input plays
`OSC_FREQUENCY`, a ladder without a cutoff input sits at `LADDER_CUTOFF_HZ`.

### Biquad moves

A new frequency, Q or gain doesn't land on a `BIQUAD` at once: the filter walks
to it, so a jump doesn't thump. `BIQUAD_RAMP` says for how long, in milliseconds
from 0 to 1000, and the default of 20 is what `audiobiquad.Biquad` always does.
The walk is a power of two frames, the one nearest what you asked for: 20 ms at
48 kHz is 1024 frames, and 0 is one frame, the shortest there is.

Set it low when a value moves every block. Each move starts the walk again from
where it had got to, so a filter envelope on a 20 ms walk trails behind what it
is told and opens slowly. One frame follows it the way `synthio.Biquad` does.
The new value takes effect at the next move, and setting it isn't a move.

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

### Low-pass gate

`LPG` is a vactrol low-pass gate, vrack's `LowPassGateCore`. Once a block its
vactrol moves toward `LPG_MANUAL` plus the control input plus 1.5 times the
strike input, each read at the block's first sample in units of `UNIT` and the
sum held to 0..1. It rises in about 2 ms and falls by e every `LPG_DECAY`
seconds, so a strike of a block or two rings and dies away.

`LPG_VCA` scales the input by the vactrol. `LPG_VCF` low-passes it at
`30 * 400^v` Hz, 30 Hz closed to 12 kHz open, through the same Q15 biquad
`synthio.Biquad` runs inside `audiofilters.Filter`, soft knee past ±28000
included. `LPG_BOTH`, the default, filters and scales by `v^1.3`, so the highs
die before the lows. `LPG_LEVEL` multiplies the output last.

The vactrol is double precision and its exp, pow and log are the graph's own,
so the node renders the same bytes on every target, CircuitPython included.
Against the rack's chain of `audiofilters` nodes with libm's exp and pow it
matched at 0 counts over every case in the tests.

### Modal resonator

`MODAL` is eight tuned resonators struck or rung by their input: bells, bars
and drum heads. Mode `m` sits at `MODAL_FREQUENCY` times `MODAL_RATIO + m`
(held under 0.45 of the sample rate), rings for `MODAL_DECAY * (1 - m / 16)`
seconds, and peaks at `(0.2 + 0.8 * MODAL_BRIGHTNESS)^m / (m + 1)`. A ratio of 0
turns its mode off.

The first rising edge of the strike input past 0.1 in a block starts a burst of
forty samples, 0.85 falling by 0.85 a sample, added to the excitation input.
`MODAL_INPUT` scales that sum before it is rounded to the wire and run through
the bank. `MODAL_MIX` goes from the excitation alone (0) to the bank alone (1),
so a node at 0 with every ratio at 0 is just the strike, ready for a string or
a delay.

The bank is `audiomodal.Bank`'s own kernel, called on each block, so its modes
match `audiomodal.Bank` sample for sample. The node retunes only when the
frequency, decay, brightness or a ratio moves.

### Struck bar

`STRUCK` fuses vrack's `HammerExciter` and `StruckBarCore` (`dsp/keys.py`)
around a 24-mode `audiomodal.Bank` kernel (`mix = 1.0`, `gain = 1/16`), for
tine, reed, vibraphone and clavinet voices. A rising edge of the strike input
past 0.1 resets the bank and starts an attack pulse of `STRUCK_SHAPE`
(`STRUCK_HAMMER`, a half-sine, or `STRUCK_PLUCK`, a rising ramp) scaled by
`STRUCK_LEVEL` (and by the velocity input in 0..`UNIT` at that sample when
connected). (A clavichord or clavinet release transient is built from separate
oscillator and envelope nodes, matching the rack's voices.)

`STRUCK_HARDNESS` 0..1 shortens the pulse from 128 samples (at 48 kHz) to 16
while keeping its area constant (`amp = level * (hard / length)`), clamped to a
half-period of `STRUCK_FREQUENCY` with the same energy compensation. On
`add()`, the node holds the 6-mode tine table at middle C; `table(node, modes)`
replaces it with up to `STRUCK_MODES` (24) `(ratio, decay, gain)` triples (or a
flat float array of `3 * m` floats), silencing any mode whose frequency reaches
0.45 of the sample rate.

Retuning the bank for a new note is the expensive part of a strike, and
`table()` does it for you when it's called, at the frequency you last
`set()`. Set `STRUCK_FREQUENCY` first and then call `table()`, and the block
that plays the note only copies the result. In the other order the render
retunes the bank itself, with the same result, in the block that strikes. On a
board without a double-precision FPU, such as the ESP32-P4, that retune is
around a millisecond for a three-node voice.

### Pickup

`PICKUP` is vrack's `PickupCore` (`dsp/keys.py`), the asymmetric transducer
nonlinearity of an electric piano pickup. The input (in units of `UNIT`) is
scaled by `PICKUP_DRIVE`, shifted by `PICKUP_OFFSET`, passed through
`x / (1 + 0.5·x²)` (`PICKUP_ELECTROMAGNETIC`, the default, for a Rhodes coil)
or `x / (1 − x² + x⁴)` (`PICKUP_ELECTROSTATIC`, for a Wurlitzer reed plate),
stripped of the steady-state DC offset that `PICKUP_OFFSET` alone produces, and
scaled by `PICKUP_LEVEL`. With `PICKUP_OFFSET` above 0, second-harmonic bark
grows as the bar swings closer to the pickup on one half-cycle. The arithmetic
is float32 with no transcendental calls, matching `PickupCore.render` to 0
counts on every target.

### Tremolo

`TREMOLO` is vrack's `TremoloCore` (`dsp/tremolo.py`), a mono-to-stereo
modulator that writes two buffers as `PAN` does: its output is the left side,
and the buffer named by `arg` is the right. An internal sine LFO
`sin(phase + i·2π·rate/sample_rate)` drives it. In `TREMOLO_PAN` mode (the
default) the angle is `θ = (π/4)·d·sin`, the left side carries
`x·(cos θ − sin θ)` and the right `x·(cos θ + sin θ)`: unity on both sides
at depth 0, and at depth 1 a swing from `√2` on one side and 0 on the other
to the reverse. In `TREMOLO_AMPLITUDE` mode, both sides carry
`x · (1 − 0.5·d·(1 − sin))`. A change of depth glides linearly across the
next block, from the last block's depth, as `TremoloCore` does; a new node
starts at depth 0 and glides to the first block's. At depth 0 at both ends of
a block the input is copied to both sides and the phase still advances.

The second and third inputs are optional rate and depth CVs (read at the
block's first sample in units of `UNIT`, added to `TREMOLO_RATE` and
`TREMOLO_DEPTH`, then clamped as `TremoloCore.set` clamps: `>= 0.01` Hz and
`0..1`; the rate is also held to the sample rate). `reset(node)` rewinds the
phase to 0 and drops the glide, so the next block starts at `TREMOLO_DEPTH`,
as `TremoloCore.reset` does. The arithmetic is float32 as numpy's, the phase
double as Python's; the sine and cosine are the LFO's polynomial, so a
sample can land a count from the rack's.

### Decay

`DECAY` is the one-shot envelope inside vrack's `OneShotEnvelopeCore`
(`dsp/drums.py`), the burst envelopes inside its `HandClapCore`, `JingleCore`
and `GuiroCore`, and the two-stage, soft-attack and rise shapes inside its
`CowbellCore`, `MaracasCore`, `BridgedTDrumCore` and
`SweptOscillatorDrumCore`. A rising edge on the trigger input (0 to above 0.1
of `UNIT`) starts the envelope at the velocity input at that sample, or 1.0
when the velocity input is open. Only the first rising edge in a block
retriggers, as `EdgeDetector` reports one; a second pulse in the same block is
folded away. A retrigger replaces the envelope rather than adding to it.

Without a table the envelope falls by `DECAY_DECAY` seconds to −40 dB —
`level · exp(−4.60517 · sample / (sample_rate · DECAY_DECAY))` — writing
`level · coeff` at the rising sample and never the step itself, as
`exp_segment` does. Three optional stages reshape that fall:

- `DECAY_ATTACK` seconds of linear ramp from 0 up to the velocity before the
  fall begins, as `MaracasCore` softens its strike. The ramp writes
  `velocity · sample / attack_n`, so the first sample is 0.
- `DECAY_FAST` seconds to −40 dB for the first `DECAY_FAST_SEC` seconds after
  the trigger, then `DECAY_DECAY` for the rest, as `CowbellCore` knocks and
  then rings. Silence is then taken at 2e-4, where the core takes it.
- `DECAY_RISE` seconds to −40 dB of a second envelope starting at the velocity
  and subtracted from the first, as `BridgedTDrumCore` and
  `SweptOscillatorDrumCore` let a 1 ms rise bite into the strike. The two
  share the velocity, so the first sample is
  `velocity · (d_decay − d_rise)`.

With a table, the node plays the bursts the three rack cores play: a spike of
`velocity · level` at each strike's sample, a fall of `DECAY_RESTRIKE` seconds
to −40 dB between spikes and `DECAY_DECAY` after the last. Strike times are
`round(delay · sample_rate)` samples from the rising sample, so a clap of
`((0, 0.75), (0.0105, 0.75), (0.021, 0.75), (0.0315, 0.85))` with
`DECAY_RESTRIKE` 0.0028 s and `DECAY_DECAY` 0.065 s is `HandClapCore`'s clap;
rhythm909's tail is 0.090 s. Both times are seconds to −40 dB so one
convention covers every rack constant: the cores' 1/e times are that number
times 4.60517, and `CowbellCore`'s `exp(−6.908 / (sr · 0.250))` tail is
0.250 · 4.60517 / 6.908.

The rack's burst cores start a burst at sample 0 of the block that sees a
rising edge, wherever in the block it is. `DECAY` starts it at the rising
sample, as `OneShotEnvelopeCore` and every other graph node do. A burst that
must match a core exactly should strike on a block boundary.

### Pink

`PINK` is vrack's `NoiseGenerator.pink` (`dsp/noise.py`): Paul Kellet's
seven-state pinking filter on its white input, scaled by `PINK_LEVEL`. The
white input is a `NOISE` node (uniform ±0.5 of `UNIT`) or any other white
block. `b6` in the sum is the previous sample's share, as the rack keeps it.
The filter is the same on every target, so two interpreters running the same
white input give the same pink.

### Crush

`CRUSH` is the quantise in vrack's `BitDepthRateCore._hold_numpy`
(`dsp/drums.py`), the bit-depth half of rhythm909's crusher. Each sample is
rounded half away from zero to a step of `2^(1−CRUSH_BITS)` in units of 1.0 —
`1 << (15 − bits)` counts — and clipped to ±2.0. `CRUSH_BITS` is truncated to
an integer and held to 1..15; 16 and above passes the audio through. The
quantise is memoryless and sits before the hold, so the sample-rate hold is
`SAMPLE_HOLD`'s and the anti-alias filter is a filter node's; nothing in the
quantise needs them.

### Acid

`ACID` is vrack's `dsp/ladder.py` `AcidFilterCore`, the diode ladder acid303
plays: four one-pole stages, the feedback high-passed so bass thins as
resonance rises, and an accent capacitor that pushes cutoff and drive for a
short burst. `ACID_CUTOFF_HZ` is the cutoff in Hz before the envelope and
accent open it (30 to 14000 after); `ACID_RESONANCE` 0..1 times 4.2 into the
high-passed feedback; `ACID_ENVMOD` 0..1, how far the env and accent open the
cutoff in octaves. The `env` input is the filter envelope and `accent` a gate
that charges the capacitor above 0.3; both are in units of `UNIT`. The cutoff
coefficient is refreshed every four samples, as the rack refreshes it.

### Spring

`SPRING` is vrack's `dsp/reverb.py` `SpringReverbCore`, the spring reverb
patch2600 runs after its VCA: two mono `audiofreeverb` tanks at roomsize
`SPRING_DECAY` and `SPRING_DECAY + 0.05` (each clamped to 0.1..0.98), both at
`SPRING_DAMP`, rendered wet-only and mixed against the dry signal by
`SPRING_MIX`. The left tank is the node's output and the right goes to the
buffer `arg` names, as `PAN` and `TREMOLO` do. The tanks hold the freeverb
comb and all-pass lines, so the node is the freeverb kernel rather than a
second reverb; nothing about the spring is simulated beyond the roomsize
offset that makes the two sides differ.

### Adsr2

`ADSR2` is ENV's contour with two more levels: the output starts at
`ADSR2_INITIAL` and peaks at `ADSR2_ATTACK_LEVEL` instead of starting at 0 and
peaking at 1.0. The times and the sustain level are ENV's, so the shape is the
same one-pole attack, decay, sustain and release; only the write differs —
`out = (initial + e · (attack_level − initial)) · UNIT`. That is poly80's
filter envelope: its IL and AL levers are `ADSR2_INITIAL` (−1..0) and
`ADSR2_ATTACK_LEVEL` (0..1), and its caller sets the sustain level to
`clamp(−initial / (attack_level − initial), 0, 1)` so the sustain step lands
on 0. ENV itself cannot express this: it always attacks to 1.0 and rests at 0,
and those are not parameters.

### Gliss

`GLISS` is `LAG`'s linear glide with one more step at the end of each block:
the last glided sample is rounded onto a grid of `GLISS_STEP · UNIT / 12`
(on a 1 V/octave wire `UNIT` is ten octaves, so a semitone is a step of 0.1) and that
one value is written across the block. The glide's own state is left
unquantised, so the next block starts from where the sweep really was and
steps again. That is poly80's glissando — a semitone a step — where the
portamento is a smooth sweep and the glissando is a staircase of one step per
block. The bend (`pitch − note`) still rides on top unslid, so a glissando
note steps while the wheel bends it. `LAG` plus a quantiser cannot express
this: no node quantises a pitch CV onto a step grid (`CRUSH` quantises
amplitude), and the quantise has to see the glide's own state, not the bent
output.

### Follow

`FOLLOW` is an envelope follower, one level a block. The block's level is
its peak absolute sample (`FOLLOW_PEAK`, clavichord's auto-wah) or its RMS
about its own mean (`FOLLOW_RMS`, vocoder330's band follower), and the node
moves toward that level by `exp(−dt / t)` a block — `dt` the block's length
in seconds, `t` `FOLLOW_ATTACK` when the level is above the current one and
`FOLLOW_RELEASE` when it is below. That one value is written across the
block, so a CV that sweeps a filter holds still between blocks and jumps at
the boundary, as the rack's does. clavichord's follower takes its 0.4 and
0.06 per-block coefficients rather than times; at 128 frames those are
`−dt/ln(0.6)` and `−dt/ln(0.94)`, about 5.2 ms and 43 ms.

### Wavetable

`WAVETABLE` plays one cycle of int16 samples as a `synthio.Note` plays it:
the same phase accumulator, the same loudness and the same silence once the
frequency passes Nyquist. Hand it the cycle with `table(node, samples)`, up
to the `arg` the node was built with (0 means 256, and no node takes more
than 16384); the node copies it. It plays at `WAVETABLE_FREQUENCY` times
`2^(10 * pitch / UNIT)` Hz, vrack's 0.1 V an octave, taken at the block's
first sample and held to 10 to 16000 Hz, so a NaN frequency plays at 10 Hz.

The extra input, an organ's percussion, is added to the tone, and the sum
goes through the contacts: a 3 ms linear ramp that follows the gate's first
sample in the block. A key that comes down while the last note still sounds
cuts the ramp to nothing first, as DrawbarToneCore's steal does. The node is
DrawbarToneCore without the bridge's 256 frames of latency, so it matches the
rack to the count everywhere except the block a steal lands in, where the
rack still plays the old note's tail and the node does not.

### Rotary

`ROTARY` is RotarySpeakerCore: a tube overdrive, an 800 Hz crossover, and a
horn and a drum whose rotor speeds glide toward `ROTARY_STOP`, `ROTARY_SLOW`
or `ROTARY_FAST`, each read through a swept delay and picked up by two
microphones on opposite sides. Build it with `arg` set to the right side's
buffer, as `PAN` is; the node's own output is the left. `ROTARY_DRIVE` runs 0
to 10, and 0.01 and under is clean; `ROTARY_VOLUME` 10 is a gain of 2 on the
microphones' sum. A NaN speed stops the rotors, a NaN drive plays clean and a
NaN volume is silence.

The crossover is `synthio.Biquad` in `audiofilters.Filter`, to the count.
Against the rack the node stays within 2 counts. `reset()` clears the
crossover's state too, where the rack's filter keeps ringing.

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

## Changing a patch while it plays

A graph's shape is fixed once it plays, so a patch change builds a second
graph beside the first and swaps it in. `carry(node, src_graph, src_node)`
hands the new graph a running node's state: its voice, envelope, oscillator
phase, filter memory and delay line continue where `src_graph` left them,
and the next `run()` renders what the old graph's next block would have.

```python
new = ag.Graph(sample_rate=48000, frames=128, channel_count=2)
# ... build the changed patch, set its parameters from the panel ...
for old_node, new_node in matched:        # the nodes both patches have
    new.carry(new_node, old, old_node)
# the next block comes from new; drop old
```

Do it between two blocks: after the old graph's last `run()` and before the
new one's first. The new graph keeps its own wiring and parameters. A
parameter that differs from the old node's lands on the next block, as a
`set()` would, and so does a change the old graph had queued but not yet
rendered. A delay keeps its own wow table: an equal copy of the one playing
plays on without a break, and a different one takes over with the dip
`table()` gives. A table the old graph lent stays held while the new graph
might still read it.

`carry()` raises `ValueError` when the two nodes are of different kinds, were
built with different `arg`s (a delay of another length), hold state of a
different size, or belong to graphs at different sample rates, and
`TypeError` when `src_graph` is not a graph. A node it refuses keeps the
fresh state it was built with. A graph the pump is pulling refuses `carry()`
with `RuntimeError` until you park the pump, below.

### Swapping while the pump plays

The pump, an output or a mixer holds on to the graph you handed it, so a
patch change there can't hand it a new object. `swap(other)` trades the two
graphs' patches instead: nodes, buffers, wiring and queue. The graph the pump
pulls keeps its identity and its frame count, and renders `other`'s patch from
the next block on. `other` is left holding the old patch, ready to drop.

```python
new = ag.Graph(sample_rate=48000, frames=128, channel_count=2)
# ... build the changed patch while the pump plays on ...
if audiopump.park():                      # the pump finishes its block and waits
    try:
        for old_node, new_node in matched:
            new.carry(new_node, g, old_node)
        g.swap(new)                       # g plays the changed patch
    finally:
        audiopump.unpark()
new.deinit()                              # the old patch
```

Build the new graph while the pump plays: only the carry and the swap need
it parked, and they take microseconds. `swap()` takes the pump's lock, so even
unparked it never lands inside a block, but `carry()` would read state the pump
is writing, which is why it waits for the park. Where the pump runs inside
`service()` on your own thread, nothing renders between two of your calls,
and you don't need to park.

`swap()` raises `TypeError` when `other` is not a graph and `ValueError` when
the two differ in sample rate, block size or channel count. Node ids are
the patch's, so after a swap use the ids you got from building `new`.

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
sample-and-hold, the five two-pole types, the overdrive, the pan, the
wavefolder and the low-pass gate. The tolerance is 0 counts: the three compile the same C
and are handed the same floats, so any difference is a defect rather than
rounding. Each planted fault makes it fail: one input sample one count off, one
`set()` a block late, a phaser's frequency a block late, a legato flag flipped,
a shape switched a block late, a clock edge a sample late, a cutoff CV a block
late, an overdrive's tone a block late, a pan a block late, a wavefolder's
symmetry a block late, a low-pass gate's strike a block late, a modal
resonator's frequency change a block late. CI runs it against
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
correctly rounded sine, within one count, `LPG` to `LowPassGateCore` with
its `audiofilters.Filter`, at 0 counts measured, and `MODAL` to
`ModalResonatorCore` with its `audiomodal.Bank`, within one count on about one
sample in three hundred: the strike's burst is a running product where vrack
takes an exp.
