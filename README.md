# audiodsp

CircuitPython's audio system for MicroPython and CPython — `audiocore`,
`audiomixer`, `synthio` (including `MidiTrack`), the effects modules
(`audiofilters`, `audiodelays`, `audiofreeverb`, `audiospeed`), `audiomp3`,
and CircuitPython `play()`/`stop()`/`pause()`/`resume()` output semantics.
Import names stay `audiocore`/`synthio`/etc., matching CircuitPython for
source compatibility.

`audiodsp` was named `audioif` until 2026-09-21; that name now belongs to the
audio hardware layer. The modules you import did not change. The helper
package is `audiodsp_util` now, and the distribution on PyPI is
`pydevices-audiodsp`.

## Installation

**MicroPython** consumes this repository as `USER_C_MODULES`, and it is
fully standalone for both build flavors — no other repository is required.
`./scripts/fetch_deps.sh` puts the two pinned native dependencies (`ulab`,
`mp3`) under `.deps/`, and both `micropython.mk` and `micropython.cmake`
look there first, falling back to a sibling checkout beside this repo and
otherwise failing the build with an error naming both paths.

For a CMake port (esp32, rp2), point `USER_C_MODULES` straight at this
checkout:

```sh
git clone https://github.com/PyDevices/audiodsp ~/build/audiodsp
cd ~/build/audiodsp && ./scripts/fetch_deps.sh
idf.py build -DUSER_C_MODULES=~/build/audiodsp
```

For a Make port (unix, windows, webassembly), MicroPython's own build glob
looks one level down, so point `USER_C_MODULES` at this checkout's
*parent* directory instead:

```sh
git clone https://github.com/PyDevices/audiodsp ~/build/audiodsp
cd ~/build/audiodsp && ./scripts/fetch_deps.sh
git clone https://github.com/micropython/micropython ~/build/micropython
cd ~/build/micropython/mpy-cross && make
cd ~/build/micropython/ports/unix && make submodules
make USER_C_MODULES=~/build
```

The two dependencies are `ulab` (so `synthtools`'s `import ulab.numpy`
works) and `mp3` (the `audiomp3` tier's decoder), pinned by
[DEPENDENCIES.lock](DEPENDENCIES.lock). To build without them, skip the
fetch and set `AUDIODSP_OPTIONAL_DEPS=1` on the **build** step instead — the
variable is read by `micropython.mk`/`micropython.cmake`, not by
`fetch_deps.sh`, and on CMake ports it is read as an *environment*
variable, not as a `-D` cache entry:

```sh
make USER_C_MODULES=~/build AUDIODSP_OPTIONAL_DEPS=1              # Make ports
AUDIODSP_OPTIONAL_DEPS=1 idf.py build -DUSER_C_MODULES=~/build/audiodsp   # CMake
```

That builds every module except `audiomp3` with no clone beyond this
repository; `synthtools`' `import ulab.numpy` then fails at runtime, which
is the trade the flag buys. See [docs/porting-plan.md](docs/porting-plan.md)
for the architecture, module tiers, phased plan, and testing strategy.

**CPython 3.11+** installs from TestPyPI:

```sh
python -m pip install --index-url https://test.pypi.org/simple/ pydevices-audiodsp
```

This gets you `audiocore`, `synthio`, `audiomixer`, `audiofilters`,
`audiodelays`, `audiofreeverb`, `audiospeed`, `audiodynamics`, `audioroute`,
`audiomath`, `audioecho`, `audioshaper`, `audioladder`, `audioconvolve`,
`audiobiquad`, `audioverb`, `audiomodal`, and the
`audiorender` package.
`audiomp3` remains firmware-only. The distribution declares no *required*
runtime dependencies and does not itself publish an `audiodsp` import; its
version is the `VERSION` file, which is also what `_audiodsp.__version__`
reports.

`audiorender` is the exception, and it is opt-in: it is numpy throughout,
so numpy comes with the `render` extra rather than with the wheel, keeping
the core dependency-free for boards and wasm. TestPyPI carries no usable
numpy, so the extra needs PyPI as a second index:

```sh
python -m pip install --index-url https://test.pypi.org/simple/ \
    --extra-index-url https://pypi.org/simple/ "pydevices-audiodsp[render]"
```

The instrument and effect libraries — `audioinstruments` (53 synthesizers,
keyboards and drum machines) and `audioeffects` (46 effect classes, racks
included) — are not part of this distribution. They live in the
[audiocomponents](https://github.com/PyDevices/audiocomponents) repository
as their own packages, each depending on `pydevices-audiodsp`; see that
repository for how to install them.

## What's here

Two pure-Python modules sit beside the CircuitPython-compatible core:

- **`lib/audiodsp_util/`** — `float32(value)` and `float32_bits(value)`,
  `struct` and nothing else. A Python float is the interpreter's own width:
  a double here, a **single** on every board. So a setting written in Python
  — `node.mix = 0.35` — is two different numbers, and the node renders
  different bytes on a board than on a desktop before its kernel is reached.
  Put a derived setting through `float32` and both targets hold the number
  the board would have held. See
  [docs/correctness-standard.md](docs/correctness-standard.md).
- **`lib/audiorender/`** — renders a whole composition offline: tracks,
  tempo map, notes and automation in, a mixed stereo master and a level
  report out. This is the one part of the repository written for a desktop
  rather than a board — numpy throughout, a whole song in memory — so it
  ships in the wheel and is never frozen into firmware. It has its own
  README.

The instrument and effect libraries that used to sit beside it —
`audioinstruments` and `audioeffects` — now live in the
[audiocomponents](https://github.com/PyDevices/audiocomponents) repository,
together with the audio component contract they implement: the metadata
manifest
([docs/audio-components.md](https://github.com/PyDevices/audiocomponents/blob/main/docs/audio-components.md))
and the runtime API
([docs/audio-component-api.md](https://github.com/PyDevices/audiocomponents/blob/main/docs/audio-component-api.md)).
`audiorender` drives any component that speaks that API and does not need
those packages installed.

## Additions beyond CircuitPython

Ten things here are not CircuitPython's. `audiodynamics` (compression,
limiting, expansion, gating, transient shaping) and `audioroute.Splitter`
(fan one stream out to parallel branches) come from micropython-vst3's audio
engine, which had them and CircuitPython does not. `audiomath` (multiply one
stream by another — ring and amplitude modulation; and divide one down in
frequency — the analog octave divider), `audioecho` (a delay with a filter,
a soft-clip and a cross-feed inside its feedback loop), `audioshaper` (a
waveshaper whose curve is data, applied above the sample rate, and a
sample-and-hold at an exact rational ratio),
`audioladder` (a transistor ladder filter — four one-pole stages round a
feedback loop with an odd saturator inside it), `audioconvolve` (apply a
measured or synthesized impulse response, by partitioned FFT), `audioverb`
(a reverberation tank whose line lengths and output taps come from Python),
`audiomodal` (a bank of resonators, which is what a struck object is),
`audioroute.MidSide` (scale the difference between a stereo pair's channels)
and `audiobiquad` (below) have no ancestor anywhere and are audiodsp's own.
`apply_cp_patches.sh` adds every one of them to a CircuitPython tree too.

### `audiobiquad` — filters whose tails reach exact zero

`audiofilters.Filter` (over `synthio.Biquad`) and `audiofilters.Phaser` are
ported CircuitPython and their recursions are integer. Both have states that
reproduce themselves: fed silence after a burst, they hold a constant for ever.
`audiobiquad` is the same two filters with `float` state and a flush of anything
below 1e-20, so a tail decays to zero and stays there — and its all-pass
feedback is not clamped to `0.1..0.9`, so zero is zero and a feedback-free
phaser's notches are true nulls. It is also the one to reach for below about
100 Hz, where the integer coefficients run out of resolution —
[which filter, and why](docs/upstream-diff.md#which-filter-to-reach-for).

```python
import audiobiquad, synthio

eq = audiobiquad.Biquad(mode=audiobiquad.PEAKING_EQ, frequency=3200,
                        Q=1.2, gain_db=-4.0, sample_rate=48000)
eq.play(source)

sweep = synthio.LFO(rate=0.4, scale=400.0, offset=900.0)
phase = audiobiquad.AllPass(stages=4, frequency=sweep, feedback=0.0,
                            mix=0.5, sample_rate=48000)
phase.play(eq)
```

| | `Biquad` | `AllPass` |
|---|---|---|
| constructor | `mode`, `frequency`, `Q`, `gain_db`, `mix`, `sample_rate`, `channel_count` | `stages`, `frequency`, `feedback`, `mix`, `sample_rate`, `channel_count` |
| block inputs | `frequency`, `Q`, `gain_db`, `mix` | `frequency`, `feedback`, `mix` |
| fixed at construction | `sample_rate`, `channel_count` | those, plus `stages` (it sizes the state) |
| methods | `play`, `stop`, `clear` | `play`, `stop`, `clear` |
| read-only | `playing`, `coefficients` | `playing`, `stages`, `coefficient` |
| latency | 0 samples | 0 samples |

`mode` is one of `LOW_PASS`, `HIGH_PASS`, `BAND_PASS`, `NOTCH`,
`PEAKING_EQ`, `LOW_SHELF`, `HIGH_SHELF` — numbered exactly as
`synthio.FilterMode` numbers them, so either can be handed to either.
`mix` crossfades: 0 is a wire, 1 is the filtered signal alone; for an
all-pass cascade 0.5 is the equal sum a script phaser makes, and where its
notches are deepest. `AllPass`'s `frequency` really is the frequency at
which one stage's phase passes −90°, so nothing has to pre-warp it. Both
nodes hand out 256 frames at a time, sit in an audiosample chain like any
other effect, and never report themselves finished — a starved chain gets
silence. `Q` is bounded to 0.05..60 and `feedback` to ±0.99: a pole on the
unit circle is a filter that never stops ringing, which is the one thing
this module exists to avoid. See
[docs/upstream-diff.md](docs/upstream-diff.md) for the measurements.

`audiodynamics.Dynamics` carries twenty-one options beyond what the engine
gave it, added for the effects program and every one of them default-off: an
RMS detector, a feedback detector topology and an external key input with Key
Listen, a 4x true-peak reconstruction, a two-pole key band with both corners
settable, a settable expander/gate depth, a four-stage gate envelope with
hold and hysteresis, a relative-threshold gain computer, a program-dependent
attack, and the transient shaper's four detector time constants, a second
envelope pair and a peak-hold on the slow one. What each is for, what it was
measured doing, and what it cost:
[docs/upstream-diff.md](docs/upstream-diff.md).

`audioecho.FeedbackDelay` takes four further options, each off at its
default. `wow_shape` swaps the built-in modulation sine for one period of
your own — `int16` Q15, a power-of-two count from 2 to 4096 — because a
bucket brigade's delay is its line length over its clock, so a triangle on
the clock is a reciprocal on the delay and no sine is that. `delay_slew`
walks the read head to a new `delay_ms` at a constant rate (delay-seconds
per second) instead of jumping to it, which is both an Echoplex's varispeed
and the reason a delay-time knob can be turned mid-take without a click.
`wow_am_depth` puts the same oscillator on the wet gain, dipping only.
`loop_semitones` pitch-shifts the line read *inside* the loop, so every
repeat rises again — a shimmer, which chaining `audiodelays.PitchShift`
after a delay is not. See
[docs/upstream-diff.md](docs/upstream-diff.md) for what each was asked for
and what it measures.

`audioroute.MidSide(source, width=1.0, sample_rate=48000, channel_count=2)`
takes a stereo pair apart into its mono sum and the difference between its
channels, scales the difference, and puts the pair back together. `width=0`
collapses to mono, `1` passes through, `2` doubles the sides; values outside
0..2 clamp to those rails. `play(sample)` sets the source, `set(width=...)`
moves the width mid-stream. There is no state and no latency — and at
`width=1` the output bytes are the input bytes, exactly, for every int16 pair,
so the node costs nothing to leave in a chain that is not using it. A mono
source passes through: there is no difference to scale. It exists for the
drive classes as much as for stereo width: a nonlinearity applied to a stereo
pair intermodulates its channels, so a saturator that wants to keep its image
drives the mid and leaves the side alone.

`audioshaper.Waveshaper(sample_rate=…, curve=…, oversample=4,
channel_count=2, **options)` is the newest of them, and everything but the
first four moves in `set()`. `curve` is int16 Q15, at least two points,
spanning −1..+1 of input: compute it once on a desktop and ship it as data,
never rebuild it on a board, whose float is single-precision where the
desktop's is double. `oversample` is 1, 2, 4 or 8 — the shaping happens there,
between a matched pair of polyphase all-pass half-bands, because a
nonlinearity at the base rate folds the harmonics it makes above Nyquist
straight back onto the signal. `pre_gain` is the drive knob (gain into one
normalised curve, never a curve rebuilt per knob move), `bias` moves the
operating point, `post_gain` follows the curve, and `mix` is a straight 0..1
crossfade. `hysteresis` is **off by default**; above zero it gives the curve a
memory, so a slow signal in and out traces two paths and encloses an area,
with `hysteresis_width` its half-width as a fraction of full scale *at the
input* and `hysteresis_bias` splitting that between the rising and falling
branches. `audioshaper.GROUP_DELAY_SAMPLES` carries the measured group delay
per factor — 0, 2.2, 3.3 and 3.9 base samples — for a component that has to
report its latency. A curve that reaches the rails has a ceiling of its
own: the decimator can ring past them on a hard edge, so keep
`post_gain * max(abs(curve))` at or below `audioshaper.CLIP_HEADROOM`
(0.74) and put the rest of the wanted level on a mixer voice after this
node. Why each of those is the shape it is, and what the
oversampling measures:
[docs/upstream-diff.md](docs/upstream-diff.md), "`audioshaper`: audiodsp's own,
and the two things a fixed curve cannot be".

`audioshaper.SampleHold(source, num=…, den=…)` is the same module's other
node: a zero-order hold in which `num` source frames carry `den` new values,
so 26 040 Hz at 48 kHz is 400/217 and is **exact** — one frame in, one frame
out, at the source's own rate, channels and bit depth, with an integer
accumulator that cannot drift however long the render runs. `num == den` is a
wire byte for byte, `latency` is 0 (what a hold displaces is an event landing
on a frame it drops, up to `ceil(num/den) − 1` frames, which is the effect
and the caller's to report), and `set(num, den)` moves the ratio mid-stream.
It exists because the palette's sample-and-hold was a pair of
`audiospeed.SpeedChanger` nodes whose 16.16 rates cannot be made reciprocal:
measured on that hold, the pair ran 0.9999947 of the rate it asked for and
flanged a static setting by 10.74 dB over twelve seconds. The rate form was
not added to `SpeedChanger` because that module is CircuitPython's and an
argument added here would not exist on a board —
[docs/upstream-diff.md](docs/upstream-diff.md), "`audioshaper.SampleHold`:
lo-fi's other half".

`audioladder` is the newest, and the one whose reason for existing is least
obvious next to a module CircuitPython already has. `audiofilters.Filter`
is a better *resonant low-pass* than this: a cascade of biquads tracks the
analytic response to a fraction of a decibel. It is also linear, so it never
sustains a tone of its own, has no harmonics to speak of, and sounds exactly
the same however hard you hit it — and those three are what a ladder is. The
whole surface:

```python
import audioladder

ladder = audioladder.Ladder(
    sample_rate=48000,      # and channel_count=1 or 2
    cutoff_hz=800.0,        # where the filter turns over
    resonance=3.6,          # 0..4.2; at 4 it sustains a tone at the cutoff
    drive=2.5,              # linear gain into the loop, 1 being unity
    poles=4,                # tap stage 1..4: 6, 12, 18 or 24 dB an octave
    passband_comp=0.0,      # 0..1, how much of the passband to give back
    oversample=2,           # 1 or 2; 2 folds back fewer of its harmonics
    mix=1.0,                # 0 a wire, 1 the filter
)
ladder.play(source)
ladder.set(cutoff_hz=1200.0)   # any option, mid-stream, once a block
ladder.clear()                 # empty the integrators; stops a sustained tone
```

See [docs/upstream-diff.md](docs/upstream-diff.md) for why its feedback loop
is solved rather than delayed, and what that was measured to be worth.

### `audiomodal.Bank`

A bank of resonators, which is what a struck object is. Hit a drum head, a
marimba bar, a bell or a wine glass and it rings as a sum of decaying
sinusoids at frequencies that are not harmonics of anything; hand `Bank` that
list and a short noise burst for the stick, and it is that object.

```python
kick = audiomodal.Bank(modes=9, sample_rate=48000, channel_count=1)
kick.set_modes(((58.0, 0.55, 1.00), (92.4, 0.24, 0.38),
                (123.9, 0.15, 0.24), (2200.0, 0.010, 0.30)))
kick.play(stick)          # a short noise burst
audio_out.play(kick)
```

Construction fixes the allocation: `sample_rate`, `channel_count` (1 or 2) and
`modes` (1 to 64). `set_mode(index, frequency, decay, gain)` places one mode
and `set_modes(table)` places the whole list; `decay` is a **60 dB time in
seconds**, not a Q, and `gain` is the peak of that mode's impulse response, so
a modal table read out of a paper goes in as published. `set()` moves `mix`
and `gain` mid-stream without stopping anything ringing, `clear()` stops
everything at once, and `ringing` says whether any mode still holds energy.

Two behaviours worth knowing before you use it. A bare `Bank()` is **silence**,
not a passthrough — the default `mix` is fully wet and an unconfigured bank has
no modes — so reach for `mix=0.0` if you want the untouched signal. And unlike
`audioecho.FeedbackDelay` and `audioverb.Tank`, the tail keeps ringing after
the source stops, because a drum whose decay ended the instant the stick left
would be the one thing this node exists not to be.

See [docs/upstream-diff.md](docs/upstream-diff.md) for why this is a node
rather than a chain of `audiobiquad.Biquad`, measured rather than argued.

### `audioverb.Tank`

Dattorro's plate reverberator, with the network handed in rather than compiled
in — which is the difference from `audiofreeverb.Freeverb`, whose comb and
all-pass lengths are constants in a CircuitPython-ported kernel.

```python
plate = audioverb.Tank(
    sample_rate=48000, channel_count=2, max_predelay_ms=200,
    decay=0.7, diffusion=0.75, bandwidth_hz=9000, damping_hz=4500,
    mod_rate_hz=1.0, mod_depth_ms=0.27, mix=0.35)
plate.play(source)
audio_out.play(plate)
```

Construction fixes the allocation and the topology: `sample_rate`,
`channel_count` (1 or 2), `max_predelay_ms`, `delays` and `taps`. `delays` is
twelve line lengths in frames — the four input diffusers, then each tank
half's modulated all-pass, delay, all-pass and delay. `taps` is four values
per tap (channel, line index, offset in frames, gain), at most 32. Both
default to Dattorro's published table, scaled from 29761 Hz to `sample_rate`.

`set()` moves the rest, mid-stream and without emptying the lines: `decay`,
`diffusion`, `damping_hz`, `bandwidth_hz`, `low_cut_hz`, `predelay_ms`,
`mod_depth_ms`, `mod_rate_hz`, `drive`, `width`, `tone_db`, `mix`. Every
filter is out of the path at zero, so a bare `Tank()` is that network with
nothing added to it; `mix` follows `audiodelays.Echo`'s 0..2 convention, dry
at unity until 1. `clear()` empties every line and every filter. Latency is
zero dry-to-wet, and the tail reaches *exact* zero rather than sitting at one
LSB — every line write is a magnitude truncation, which is the standard cure
for a recirculating fixed-point network's limit cycles. See
[docs/upstream-diff.md](docs/upstream-diff.md) for the measurement, the cost
and the memory the default table needs.

### `audioroute.Port` — the wire a consumer holds

A zero-copy pass-through you can re-point while it is playing. Its
`get_buffer` hands back its source's own pointer, length and result; its
`play(source)` swaps that source atomically against a pull in flight. Hold a
port, wire it into a mixer voice or a rack, and change what is behind it as
often as you like — the object the consumer took never changes.

```python
import audioroute

port = audioroute.Port(overdrive)
mixer.voice[0].play(port)       # take it once

port.play(bitcrusher)           # heard on the next block; nothing rewires
port.source                     # what it is playing right now
```

That is the problem it was built for. About twenty effect classes rebuild
part of their graph when a knob crosses a threshold and hand out a new node
afterwards — and a consumer holding the old one goes on playing the graph the
class has finished with, or goes silent. A port turns that into a re-point.
`play()` does the protocol lookup, the deinit check and the format match
first, on the calling thread where raising is free, then takes the pump lock
for three stores, so a pull in flight sees the whole old source or the whole
new one and nothing in between.

It costs about **20 ns a block on x86-64**, measured as the slope of 0, 25, 50
and 100 stacked ports over the same 2000-block probe; against a 256-frame
block at 48 kHz that is four ten-thousandths of one per cent. On an ESP32-P4
it has not been measured; the estimate from four classes timed on both
machines (a 32–35x ratio) is about 700 ns.

`Port` has no `stop()`, deliberately — a port always has a source. A port
pulled while it is already inside itself, which is what a component wrapping
its own output builds, publishes a loop fault and hands back an error in
17 us instead of recursing until the stack is gone.

## `audiopump` — the audio pull, off the interpreter thread

`audiopump` runs the same pull a player runs, in C, on a thread the
interpreter is not on: a task pinned to the other core on esp32, a native
thread on unix and Windows, and the calling thread on a port that has no
threads at all. A graph pulled that way keeps an exact clock while a screen
redraws, a USB stack runs and Python does whatever it likes.

```python
import audiopump, audiomixer, synthio

synth = synthio.Synthesizer(sample_rate=48000, channel_count=2)
mixer = audiomixer.Mixer(voice_count=1, sample_rate=48000,
                         channel_count=2, buffer_size=2048)
mixer.voice[0].play(synth)

status = bytearray(audiopump.STATUS_BYTES)
audiopump.spawn(mixer, 0x7FFFFFFF, status, sink=True)

synth.press(60)                # while it plays; no parking, no ceremony
mixer.voice[0].level = 0.6
audiopump.shutdown()           # and a soft reset does this for you
```

`spawn(sample, blocks, status, …)` starts it; `pull()` runs the same loop on
the calling thread, so `micropython.heap_lock()` around it is a real gate on
"the pull does not allocate"; `service()` advances it where there is no
thread; `retarget(sample, loop=…)` points it at a different tail;
`shutdown()` stops it. `status` is a `bytearray` of counters — blocks, bytes,
a digest of everything pulled, the worst block, what the sink clocked, the
error and the fault — read back with `struct.unpack`. Everything that can
refuse happens on the calling thread. The pull itself never raises, because
there is no interpreter on that thread to raise on, so it publishes a fault
code and stops.

`retarget`'s `loop=` travels **with** the swap rather than being stored when
you call it: the old tail is pulled until the next block boundary, so setting
the flag early ends the pump one block before the new tail ever runs. Leave it
out and the flag stays as `spawn()` set it, which is what every caller before
the argument existed wanted. Get it wrong the other way and a looping client
left alone on a live pump stops at the end of its lap.

### `backpressure()` — whether a full ring makes the pump wait

```python
>>> audiopump.backpressure()
True
```

True where a full output ring makes the pump **wait** for room instead of
dropping the block; False where a free-running pump would lose audio. It is
the question a driver asks before it decides whether to park the pump between
ticks, and it is True in two different ways: on a threaded port whose driver
fills in `park_spin`, where a wake ends the wait, and in service mode, where
the loop hands the thread back on a full ring and the caller's next
`service()` is the wake. It is False only on a threaded build with no
`park_spin` — a driver that is not finished, which now says so rather than
quietly losing blocks.

It is worth asking because parking costs the caller. On the **desktop unix
build**, ten seconds of a synth through an Overdrive and a TapeDelay with an
app doing 3.5 ms of work a tick: the interpreter spends **52 ms per 10 s on
audio against 862 ms on the old interpreter-thread path**, where a parked pump
cost 923 ms. With no app work at all the two are level (758 ms against 713).
A parked pump used to spin 94 % of one core and now sleeps at 1 %. A WAV
plays back byte-identically ten times out of ten with all eight cores of that
box in a busy loop, overflow count 0 by construction; with the drop put back,
0 of 10.

Three kinds of source feed it and they are all the same kind of thing to it:
a graph; `audiopump.Ring`, an audiosample node Python writes PCM into, so a
pushed stream can sit behind a Mixer with effects on it like anything else;
and `audiopump.Events`, a queue of frame-stamped presses, plays and levels
the pump applies at block boundaries, which is what puts a sequenced bar on
the audio's clock instead of the interpreter's. `audiopump.Tap` reads what is
going out, for a meter or a scope, without being in the path.

**Which builds carry it.** Every MicroPython port: unix, Windows,
WebAssembly and esp32. The **CPython wheel does not** — it is a MicroPython C
module through and through (`MP_REGISTER_MODULE`, `mp_obj_t`, a VM root
array), so it is excluded from the wheel rather than stubbed, and a desktop
Python program has threads of its own. **CircuitPython does not either**: its
playback layer already pulls natively from its own audio thread, and a second
pull loop would be a second owner of the same graph. What both of those still
take is the lock and the hook table, on default hooks, which cost a load and
a branch.

The hardware — the channel a board writes into, and a live microphone as a
source — is not here. It arrives as a separate module, `_audioif`, from a
platform driver, and `audiopump.driver()` tells you which one bound:

```python
>>> audiopump.driver()
'esp32'          # or 'pthread', 'win32', 'none'
```

`'none'` is a real answer rather than a failure: the default hook table is
all NULLs, which means one thread and no hardware, and that is exactly how
the WebAssembly build runs. Writing a driver for a platform that has none yet
is [docs/pump-ports.md](docs/pump-ports.md).

## Status

**MicroPython:** all module tiers ported and oracle-diffed byte-for-byte
against `bin/circuitpython` on unix; DSP parity re-verified on windows and
wasm; built and measured on two real mcu targets, ESP32-P4
(hardware-confirmed by ear) and RP2040 (build-only). See
[docs/porting-plan.md](docs/porting-plan.md) for the full phased history
and [docs/upstream-diff.md](docs/upstream-diff.md) for every deliberate
deviation from upstream CircuitPython.

**CPython:** the public surface and wheel plumbing are present, and the
committed synthesis, mixer, MIDI, streaming, and effects fixtures match
CircuitPython 10.3.0 PCM byte-for-byte — built at the same voice ceiling this
port ships, which is the only comparison worth making. Import/API smoke success
is not used as a substitute for those comparisons.

`ulab` and `mp3` (the vendored Adafruit_MP3/Helix decoder `audiomp3`
depends on, RPSL/RCSL-licensed — not MIT, carried unmodified per upstream's
own terms) are consumed as cloned sibling dependencies in the parent
workspace, same pattern as `pygraphics`/`displayif`, not vendored into this
repo.

The playback-facing pull protocol (`audiocore.get_buffer`/`reset_buffer`)
is consumed by `pydevices`' `lib/audiodev` package (`AudioOut` in
`sample_out.py`), which pumps any audiosample into any of `audiodev`'s
existing push-PCM transports (sdl2/win/wasm/i2s/emulated) — see that
package's own docs for the playback-side contract this repo's protocol
implementation is built to satisfy.

Acceptance target: todbot's
[`synthtools`](https://github.com/todbot/CircuitPython_SynthTools) running
unmodified, with rendered PCM diffed against `bin/circuitpython`'s own unix
coverage build (which already contains the entire DSP stack — the parity
oracle for this whole port).

## Sound stability

The API is our contract with you: class names, signatures, metadata, and
macro surfaces stay stable and change only deliberately. The *sound* is
not part of that contract. These components sound great, but they are not
all as accurate as they could be, and implementations will keep being
refined as the library matures — so a component may render audibly
differently from one release to the next. If a composition depends on the
exact sound of a release, pin that release rather than tracking the
latest; the code of every release stays available for exactly this
reason.

Beneath the components sits a harder guarantee: the audiodsp core — the
CircuitPython-compatible `synthio`/`audiocore`/effects-module layer — is
held bit-exact to CircuitPython itself, verified by parity gates, and
that never changes release to release. Where we find CircuitPython and
audiodsp disagree, we treat it as a bug and report it upstream. The
components are where the sound evolves; the floor they stand on does
not.

## License

audiodsp is MIT licensed — see [LICENSE](LICENSE).

The attribution that goes with it lives in [NOTICE](NOTICE): substantial
portions of this code are ported from CircuitPython under its own MIT
license, CircuitPython is a trademark of Adafruit Industries and this
project is not affiliated with or endorsed by them, and the vendored test
corpus under `tests/vendor/` carries its own licenses. The two are separate
files on purpose — GitHub and PyPI only detect the license when LICENSE holds
the MIT text and nothing else, so please do not fold NOTICE back into it.
`NOTICE` is in setuptools' default `license-files` glob, so it ships in the
wheel and sdist alongside `LICENSE`.

The Helix MP3 decoder that `audiomp3` wraps is RPSL/RCSL-licensed, not MIT;
it is a cloned sibling dependency, not part of this repository. See
[docs/upstream-diff.md](docs/upstream-diff.md).
