# Additions beyond CircuitPython

audiodsp has two kinds of modules. The CircuitPython-compatible core is held
to CircuitPython's own output; [the correctness standard](correctness-standard.md)
says how. The additions are audiodsp's, each there because CircuitPython has
nothing that does its job. [The modules](../reference/modules.md) lists every
one with what it does and where it's documented. This page says where they
came from and why they exist.

## Where they came from

`audiodynamics` (compression, limiting, expansion, gating, transient shaping)
and `audioroute.Splitter` come from [mpvst](https://github.com/PyDevices/mpvst)'s
audio engine, which had them and CircuitPython doesn't.

The rest have no ancestor anywhere: `audiomath`, `audioecho`, `audioshaper`,
`audioladder`, `audiooscillator`, `audioconvolve`, `audioverb`, `audiomodal`,
`audiosampler`, `audioroute.MidSide` and `Port`, `audiobiquad` and
`audiograph`. Each one does something no arrangement of CircuitPython's
modules can: a filter or a shaper inside a delay's feedback loop, a curve that
is data, a resonant loop that sustains, a reverb whose network you choose, a
bank of resonators, a sample played at any pitch. Every one of them builds into
CircuitPython 11 too, as a user C module; see
[Install](../get-started/install.md#circuitpython-11).

Four more are for cases CircuitPython doesn't cover. `audiometer` (band levels
for a spectrum meter) and `audiofeatures` (speech features for a wake word
model) take their audio from a board's pump or a USB sound card, which
CircuitPython has neither of, so they don't build for it. `audiovorbis` plays
Ogg Vorbis files, which CircuitPython has no module for. And `audiopump.Ring`
lets CircuitPython play audio your own code computes, with
`audiopump.DriftResampler` to play one filled on another device's clock
([the drift resampler](drift-resampler.md)).

## Why `audioroute.Port` exists

Many effect classes rebuild part of their graph when a knob crosses a
threshold and hand out a new node afterwards. A consumer holding the old one
goes on playing the graph the class has finished with, or goes silent. A port
turns that into a re-point.

`play()` does the protocol lookup, the deinit check and the format match
first, on the calling thread where raising is free, then takes the pump lock
for three stores, so a pull in flight sees the whole old source or the whole
new one and nothing in between. It moves no bytes: it hands back its source's
own buffer. How to use it:
[Change what plays without a click](../guides/change-without-a-click.md#swap-one-part-of-the-graph-through-a-port).

The measurements behind each module (why `audiobiquad` exists beside
`audiofilters.Filter`, what the dynamics options cost, why `audioshaper`
oversamples, the memory the reverb tank needs) are in
[the upstream record](https://github.com/PyDevices/audiodsp/blob/main/docs/dev/upstream-record.md).
