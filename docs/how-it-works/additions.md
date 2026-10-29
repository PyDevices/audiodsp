# Additions beyond CircuitPython

audiodsp has two kinds of modules. The CircuitPython-compatible core is held
to CircuitPython's own output; [the correctness standard](correctness-standard.md)
says how. The additions are audiodsp's, and each has a reason to exist beside
what CircuitPython already carries. [The modules](../reference/modules.md)
lists them with their documentation.

## Where they came from

`audiodynamics` (compression, limiting, expansion, gating, transient shaping)
and `audioroute.Splitter` (fan one stream out to parallel branches) come from
[mpvst](https://github.com/PyDevices/mpvst)'s audio engine, which had them and CircuitPython does not.

These have no ancestor anywhere and are audiodsp's own:

- `audiomath`: multiply one stream by another (ring and amplitude
  modulation), divide one down in frequency (the analog octave divider), and
  `cast_into`, which turns a float block into the int16 a `RawSample` plays
  without allocating.
- `audioecho`: a delay with a filter, a soft-clip and a cross-feed inside its
  feedback loop.
- `audioshaper`: a waveshaper whose curve is data, applied above the sample
  rate, and a sample-and-hold at an exact rational ratio.
- `audioladder`: a transistor ladder filter, four one-pole stages round a
  feedback loop with an odd saturator inside it.
- `audiooscillator`: a band-limited oscillator whose pitch, FM, pulse width
  and hard sync are audio streams, read sample by sample.
- `audioconvolve`: apply a measured or synthesized impulse response, by
  partitioned FFT.
- `audioverb`: a reverberation tank whose line lengths and output taps come
  from Python.
- `audiomodal`: a bank of resonators, which is what a struck object is.
- `audiosampler`: recorded notes played at any pitch, looped, under an
  envelope.
- `audioroute.MidSide`: scale the difference between a stereo pair's channels.
- `audiobiquad`: the biquad and all-pass filters with float state.
- `audiograph`: a whole patch rendered by one C call per block.

Every one of them builds into CircuitPython 11 too, as a user C module; see
[Install](../get-started/install.md#circuitpython-11).

Four more are for cases CircuitPython doesn't cover. `audiometer` (band
levels for a spectrum meter) and `audiofeatures` (speech features for a wake
word model) are the two CircuitPython doesn't get: their sources are a board's
pump and a USB sound card, and CircuitPython has neither. `audiovorbis` plays Ogg Vorbis files, which CircuitPython has no
module for. And `audiopump.Ring` lets CircuitPython play audio your own code
computes.

## Why `audioroute.Port` exists

Many effect classes rebuild part of their graph when a knob crosses a
threshold and hand out a new node afterwards. A consumer holding the old one
goes on playing the graph the class has finished with, or goes silent. A port
turns that into a re-point.

`play()` does the protocol lookup, the deinit check and the format match
first, on the calling thread where raising is free, then takes the pump lock
for three stores, so a pull in flight sees the whole old source or the whole
new one and nothing in between. How to use it:
[Change what plays without rewiring](../guides/swap-what-plays.md).

## Where each reason is written down

[upstream-diff](upstream-diff.md) holds the measurement behind each module
and each difference from CircuitPython: why `audiobiquad` exists beside
`audiofilters.Filter`, what the dynamics options cost, why `audioshaper`
oversamples, why `audiomodal` is a node rather than a chain of biquads, how
the sampler's pitch is verified, and the memory the reverb tank needs.
