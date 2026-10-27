# Filters and effects

What the added filters and effects take and return. For a task-shaped
introduction to the biquad, see [Filter a sound](../guides/filter-a-sound.md).
The reasons these exist beside CircuitPython's own modules are in
[Additions beyond CircuitPython](../how-it-works/additions.md).

## `audiobiquad`


| | `Biquad` | `AllPass` |
|---|---|---|
| constructor | `mode`, `frequency`, `Q`, `gain_db`, `mix`, `sample_rate`, `channel_count` | `stages`, `frequency`, `feedback`, `mix`, `sample_rate`, `channel_count` |
| block inputs | `frequency`, `Q`, `gain_db`, `mix` | `frequency`, `feedback`, `mix` |
| fixed at construction | `sample_rate`, `channel_count` | those, plus `stages` (it sizes the state) |
| methods | `play`, `stop`, `clear` | `play`, `stop`, `clear` |
| read-only | `playing`, `coefficients` | `playing`, `stages`, `coefficient` |
| latency | 0 samples | 0 samples |

When a `Biquad`'s `frequency`, `Q`, `gain_db` or `mode` changes while it plays,
it glides to the new filter over about 20 ms instead of jumping, so a preset
change that drops the corner from 20 kHz to 20 Hz doesn't thump. A block input
glides the same way, so an LFO on `frequency` arrives about a block late.
`mix` still moves at once, and so does a filter that hasn't played yet.

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
[upstream-diff](../how-it-works/upstream-diff.md) for the measurements.

## `audiodynamics`

`audiodynamics.Dynamics` carries options beyond what the engine it came
from had, every one of them default-off: an
RMS detector, a feedback detector topology and an external key input with Key
Listen, a 4x true-peak reconstruction, a two-pole key band with both corners
settable, a settable expander/gate depth, a four-stage gate envelope with
hold and hysteresis, a relative-threshold gain computer, a program-dependent
attack, and the transient shaper's four detector time constants, a second
envelope pair and a peak-hold on the slow one. What each is for, what it was
measured doing, and what it cost:
[upstream-diff](../how-it-works/upstream-diff.md).

## `audioecho`

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
after a delay is not. Shifting up, the climb fades out at the top of the band
rather than folding back past Nyquist. See
[upstream-diff](../how-it-works/upstream-diff.md) for what each was asked for
and what it measures.

If you want the delay in frames rather than milliseconds, give `delay_frames`
instead of `delay_ms`: the line is read exactly that many frames back. At
44.1 and 22.05 kHz some whole frames have no `delay_ms` that lands on them,
so a lossless repeat at those rates needs `delay_frames`.
## `audioroute.MidSide`

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
## `audioshaper`

`audioshaper.Waveshaper(sample_rate=…, curve=…, oversample=4,
channel_count=2, **options)` moves everything but its first four arguments
in `set()`. `curve` is int16 Q15, at least two points,
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
[upstream-diff](../how-it-works/upstream-diff.md), "`audioshaper`: audiodsp's own,
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
[upstream-diff](../how-it-works/upstream-diff.md), "`audioshaper.SampleHold`:
lo-fi's other half".
## `audioladder`

`audioladder` is the one whose reason for existing is least
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
    cutoff_octaves=10.0,    # octaves for a full-scale cutoff input
    buffer_size=None,       # bytes a block, as on the effects; 256 frames
)
ladder.play(source)
ladder.set(cutoff_hz=1200.0)   # any option, mid-stream, once a block
ladder.modulate(cutoff=lfo)    # the cutoff read from a stream, every sample
ladder.clear()                 # empty the integrators; stops a sustained tone
```

`set()` moves the cutoff once a block, which is plenty for an envelope or a
knob. Sweep it with an audio-rate signal that way and you hear the staircase,
a buzz at the block rate. The `cutoff` input reads the cutoff every sample
instead, as `cutoff_hz * 2 ** (v * cutoff_octaves)` for an input at `v` of
full scale (first channel only), so filter FM sounds like filter FM. An input
of zero, or none at all, is the filter exactly as `cutoff_hz` sets it.
## `audioverb`

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
for a recirculating fixed-point network's limit cycles. When the source ends,
the tail rings out on silence to that zero and the node rests, so a source
played into it afterwards meets an empty plate; `audioecho.FeedbackDelay` and
`audioconvolve.Convolver` do the same. See
[upstream-diff](../how-it-works/upstream-diff.md) for the measurement, the cost
and the memory the default table needs.
