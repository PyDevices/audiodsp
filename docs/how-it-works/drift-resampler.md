# The drift resampler

`audiopump.DriftResampler` plays a `Ring` that something else fills on its
own clock: a USB microphone, a network stream, a second codec. You hand it
the Ring and play it instead of the Ring, and it keeps the Ring at a target
fill for as long as the two clocks run, without a gap or a skip:

```python
ring = audiopump.Ring(sample_rate=48000, channel_count=2, frames=256, capacity=8)
drift = audiopump.DriftResampler(ring)          # holds the Ring at 1024 frames
mixer.voice[0].play(drift)
```

This page is why it is built the way it is, and what it measured.

## Why a fixed ratio isn't enough

Two devices that both say 48 kHz differ by tens of parts per million, and
up to a few hundred for cheap crystals. At 100 ppm a Ring between them
gains or loses 4.8 frames a second, so a 1024-frame cushion is gone in
under four minutes. The fix is to read the Ring very slightly faster or
slower than nominal, by exactly the clocks' difference, which nobody knows
in advance and which wanders with temperature.

`audiospeed.SpeedChanger` holds its rate in 16.16 fixed point, a step of
about 15 ppm: the size of the error being corrected. Here the read position
and the ratio are 32.32 fixed point, a step of about 0.0002 ppm, so the
ratio can sit where the clocks are rather than hunting between two steps
either side of it.

## Where it lives

The node is in `audiopump`, beside `Ring`, rather than beside
`audiospeed.Resampler`. CircuitPython has its own `audiospeed`, and a module
of ours never shadows one of CircuitPython's
([the correctness standard](correctness-standard.md)). `audiopump` is ours
on all three runtimes, and the node only reads a Ring, so it belongs with
the Ring.

## Interpolation

The read position falls between input samples, so the node interpolates
with a 32-tap Kaiser-windowed sinc (beta 9, cutoff 0.45 of the input rate).
The filter is tabulated at 128 fractional positions, plus one more so a
neighbour always exists, and the node blends linearly between the two rows
either side of the read position. Each row is scaled to sum to exactly 1, so
a constant input comes out unchanged at every position. The table is
[`src/shared/audiodsp_drift_table.h`](../../src/shared/audiodsp_drift_table.h),
written by [`tools/design_drift_table.py`](../../tools/design_drift_table.py);
it is a checked-in constant rather than something each runtime computes,
because CircuitPython's floats are single precision and its libm is not
glibc's, and every target has to render the node identically.

The cutoff is set for the input rate, so the node converts up or across,
never down: an output rate from the Ring's own up to twice it. Converting
down would fold everything between the two Nyquist frequencies back into
the band.

**Cost.** Per output frame: 32 coefficient blends, then 32 multiply-adds a
channel, all single-precision float. That is about 96 floating-point
multiply-adds a stereo frame, 4.6 million a second at 48 kHz, or roughly 3%
of one ESP32-P4 core at 360 MHz. That figure is an estimate from the
operation count; it has not been timed on a board. The work buffer is the block plus 36
frames, as floats.

## Control

Once a block, the node measures the fill: the frames the Ring holds plus
those already taken into the filter beyond its look-ahead, less the
fractional read position. The ring's level jumps by a whole packet each
time the producer writes, so the measurement goes through two one-pole
smoothers before a PI loop. The loop's output is the ratio's offset from
nominal, clamped to `max_ppm` (1000 by default), and the integrator is
clamped to the same bound so it cannot wind up.

The loop is a second-order system with damping 0.8 and natural frequency
`bandwidth` (0.01 Hz by default, 0.001 to 0.1 allowed). It starts at eight
times that and narrows with a 15-second time constant, so it locks in
seconds and then holds still. The smoothers sit at four times the loop's
frequency. Above about 0.1 Hz the loop starts following the producer's
packets instead of its clock, and the ratio swings by hundreds of ppm from
one block to the next, so the bound stops there.

Playback starts when the fill first reaches the target, and anything beyond
it is dropped on that block, so the loop does not start by draining an
excess. If the Ring runs short of a block, the node plays a block of
silence, counts an underrun on itself and on the Ring, and waits for the
target again. It keeps the rate it had learned, since the clocks have not
changed.

## What it measured

On the desktop, through the CPython build, with a producer writing a Ring
of 8 blocks of 256 frames (target 1024) on a clock offset from the output's.
Two packet sizes: 48 frames (1 ms, a USB microphone) and 480 (10 ms, a
network or Bluetooth burst). 90 to 120 seconds each, default settings.

| Producer's clock | Packet | Underruns, overruns | Within 8 frames of target after | Ratio after lock |
| --- | --- | --- | --- | --- |
| +100 ppm | 48 | 0, 0 | 2.3 s | 100.00 ± 0.36 ppm |
| −100 ppm | 48 | 0, 0 | 6.2 s | −99.94 ± 0.36 ppm |
| +300 ppm | 48 | 0, 0 | 5.6 s | — |
| +100 ppm | 480 | 0, 0 | 32 s | 101.7 ± 6.4 ppm |
| −100 ppm | 480 | 0, 0 | 28 s | −98.1 ± 5.2 ppm |
| ±100 ppm over 2 minutes | 48 and 480 | 0, 0 | tracks within 54 frames | follows it |

The 480-frame cases take longer because the first measurement lands
somewhere in the packet's sawtooth, up to about 100 frames from where the
smoothed fill settles. The ±5 ppm left on the ratio there is the packets
showing through the smoothers, a fraction of a cent of pitch moving below
1 Hz.

**THD+N**, a 0.9 full-scale sine in, everything from 20 Hz to 20 kHz but the
fundamental out: −93.7 to −95.2 dB at 1 kHz and 10 kHz across every case
above. A sine quantized once to 16 bits measures −97.5 dB at 1 kHz on the
same analysis, and the node's input and its output are each 16-bit, so two
roundings alone come to about −94.5 dB. The interpolation adds little to
what 16-bit audio already has.

`tests/test_cpython_drift.py` holds these as traits, with bars a little
looser than the figures, and `tests/parity/drift_probe.py` checks that
MicroPython, CircuitPython and CPython render the node byte for byte.
