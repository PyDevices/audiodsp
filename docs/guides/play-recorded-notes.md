# Play recorded notes


Recorded notes, played at any pitch. `audiomixer.Mixer` plays sources at
their own rate and `synthio` plays single-cycle waveforms; neither can take a
recorded note, play it a fourth up, loop a region of it while the key is held
and let the rest of it out when the key lifts. `Sampler` does that, which is
what a SoundFont instrument and every drum machine need.

```python
sampler = audiosampler.Sampler(voice_count=8, sample_rate=48000)
audio_out.play(sampler)
sampler.voice[0].play(piano_c4, rate=44100, pitch=2 ** (5 / 12),
                      loop_start=11000, loop_end=19000,
                      loop_mode=audiosampler.LOOP_UNTIL_RELEASE,
                      attack=0.005, release=0.3)
...
sampler.voice[0].release()
```

A `Sampler` is an audio source like a `Mixer`: play it into an output, a
mixer voice or an effect. Notes start on its voices. A buffer is mono signed
16-bit — an `array('h')`, a `memoryview`, anything with a buffer — and `rate`
is the buffer's own sample rate (the sampler's when left out), so the step per
output frame is `pitch * rate / sample_rate` and a 44.1 kHz recording plays at
the right speed through a 48 kHz output without being told anything else.
The buffer is held for as long as the voice sounds and let go when it stops.

The loop modes are SoundFont's `sampleModes` under their own numbers, so a
reader writes the file's value straight in: `LOOP_NONE` (0) plays to the end
and stops, `LOOP_FOREVER` (1) loops `[loop_start, loop_end)` until
`release()` or `stop()`, and `LOOP_UNTIL_RELEASE` (3) loops while the key is
held, then plays on past the loop to the end of the buffer.

Each voice has an amplitude DAHDSR — `delay`, `attack`, `hold`, `decay` and
`release` in seconds, `sustain` a linear level — run per sample in C, so a
two-millisecond attack is a slope and not a step. `delay` postpones the start
of the sound itself, which is what a flam wants. `level`, `pan` and `pitch`
are live properties: a change lands at the next block, the first two as a
ramp across it so a fader move is not a click, and the third is how a pitch
bend is played. `release()` enters the release stage from wherever the
envelope is; `stop()` is silence from the next block; `playing` says whether
the voice still sounds.

Every voice is mixed in one C call per block, summed in float and quantised
once, through a 4-point Hermite interpolator, so a loop at pitch 1.0 is a
wire. For why the position is fixed point and how the pitch is verified by
measurement, see [`upstream-diff`](../how-it-works/upstream-diff.md).

`SampleBuffer(size)` is memory for the samples, outside the collected heap:
`size` zeroed bytes, with a buffer, so you can read into it and play views of
it. A bank of samples in bytearrays makes every `gc.collect()` read all of
them looking for pointers, which on a board with megabytes of samples is
seconds; a `SampleBuffer`'s bytes are never read. A view of one doesn't keep
it alive, so hold the buffer itself for as long as a voice plays from it.
`deinit()`, or the buffer being collected, frees the bytes. On a board whose
audio driver says where (PSRAM on the ESP32 ports) they come from there; on
CPython it is a bytearray.
