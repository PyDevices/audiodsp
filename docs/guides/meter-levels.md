# Meter levels


N log-spaced bands between two frequencies, plus peak and RMS, one byte each
in half-dB steps (0 is -100 dB or quieter, 200 a full-scale sine).

```python
meter = audiometer.Meter(32, low_hz=35, high_hz=20000)
meter.feed(pcm, 48000, 2)            # interleaved s16, from Python
seq, levels, peak, rms = meter.levels()
```

On a board it can listen without Python touching a sample.
`meter.attach(tap, 48000)` reads an `audiopump.Tap` in C each time you call
`levels()`, so any pump stream can be metered, whatever it came from: a file,
I2S, or a USB sound card playing through the pump. `seq` counts analyses, 60 a second of audio, so a
reader can tell fresh levels from a stopped stream.

Two FFTs, because one can't be both quick and sharp at the bottom of a log
scale: 1024 points for the bands above 400 Hz, and 512 points on a copy
low-passed and decimated by 16 for those below. A band's level is the
spectrum integrated over the band, so pink noise reads flat to within 3 dB
of the median band across 35 Hz to 20 kHz (the bound its test holds). The default floor is 35 Hz because below that
real music barely moves a bar. The window, the filter, the band edges and
the dB rounding avoid libm, so a desktop and a board read the same bytes for
the same samples; [`tests/parity/meter_probe.py`](../../tests/parity/meter_probe.py) checks that.

