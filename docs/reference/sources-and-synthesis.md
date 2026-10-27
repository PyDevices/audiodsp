# Sources and synthesis

What the added sound sources take and return. `audiosampler` is in
[Play recorded notes](../guides/play-recorded-notes.md); the reasons behind
each module are in [Additions beyond CircuitPython](../how-it-works/additions.md).

## `audiooscillator`

`audiooscillator` is the oscillator synthio does not have. synthio's moves its
pitch once per block and reads a wavetable with no band limiting; this one
reads its pitch, linear FM, pulse width and hard sync from audio streams every
sample, so one oscillator can sync to another or be frequency-modulated by it:

```python
import audiooscillator as ao

lead = ao.Oscillator(
    sample_rate=48000,      # and channel_count=1 or 2
    frequency=220.0,        # Hz, before the pitch input
    waveform=ao.SAW,        # any sum of SAW, PULSE, TRIANGLE and SINE
    waveform_right=ao.PULSE,  # the right channel's, from the same phase
    pulse_width=0.5,        # 0.05..0.95
    level=0.5,              # output for a waveform's full swing
    pitch_octaves=10.0,     # octaves for a full-scale pitch input
    fm_depth=300.0,         # Hz for a full-scale FM input, linear
    pwm_depth=0.5,          # pulse width for a full-scale PWM input
    buffer_size=None,       # bytes a block; 256 frames
)
lead.modulate(sync=ao.Oscillator(frequency=110.0, waveform=ao.PULSE))
lead.set(frequency=330.0)   # any option, mid-stream
lead.clear()                # phase back to zero
```

Steps are band-limited with PolyBLEP and the triangle's corners with
PolyBLAMP. The output trails the phase by one sample, which is what lets a
sync edge, which nothing can see coming, still correct the sample before it.

See [upstream-diff](../how-it-works/upstream-diff.md) for why its feedback loop
is solved rather than delayed, and what that was measured to be worth.

## `audiomodal`


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
no modes — so reach for `mix=0.0` if you want the untouched signal. And the
tail keeps ringing after the source stops, as `audioecho.FeedbackDelay`'s,
`audioverb.Tank`'s and `audioconvolve.Convolver`'s do, because a drum whose
decay ended the instant the stick left would be the one thing this node exists
not to be.

See [upstream-diff](../how-it-works/upstream-diff.md) for why this is a node
rather than a chain of `audiobiquad.Biquad`, measured rather than argued.
## `audiograph`


A whole patch rendered by one C call per block. You build a graph of nodes
over the kernels above (the oscillator, the ladder, the biquad, the feedback
delay, the all-pass, the sample-and-hold) and a few small ones a voice needs
(an ADSR, an LFO of five shapes, noise, a mixer with ramped gains, a VCA, a
glide), wire them through the graph's own block buffers,
put each voice in a group, and call `run()` once a block; nothing in Python
runs between the nodes. That is what lets a six-voice polysynth play in real
time on an ESP32-P4. The same graph renders the same samples on CPython,
MicroPython and CircuitPython, so a patch you build on the desktop sounds
there as it will on the board:
[the audiograph page](../how-it-works/audiograph.md) has the nodes, the wire and the
limits.
