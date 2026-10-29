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

## Filter, modulation envelope and LFOs

A voice can also do what a SoundFont region asks for besides its amplitude
envelope. Give it a resonant low-pass, a second envelope that sweeps the
cutoff and the pitch, a vibrato, and a slower LFO on pitch, cutoff and
volume:

```python
sampler.voice[0].play(brass_c4, rate=44100, loop_start=2000, loop_end=6000,
                      loop_mode=audiosampler.LOOP_UNTIL_RELEASE,
                      attack=0.02, release=0.2,
                      cutoff=800, resonance=6,             # Hz, dB
                      mod_attack=0.05, mod_decay=0.4, mod_sustain=0.3,
                      mod_to_cutoff=2400,                  # cents at full scale
                      vibrato_delay=0.3, vibrato_rate=5.5, vibrato_depth=15)
...
sampler.voice[0].vibrato_depth = 50                        # a mod wheel
sampler.voice[0].cutoff = 1200
```

`cutoff` is in Hz and `resonance` in dB: the filter's response at the cutoff
sits that far above its response at DC, and DC drops by half of it, as the
SoundFont specification puts it. Zero resonance is a Butterworth. A cutoff of
19.9 kHz or more, with no resonance and nothing moving it, is no filter at
all, and so is leaving it out.

The modulation envelope has the amplitude envelope's six stages, prefixed
`mod_`, and moves pitch by `mod_to_pitch` and the cutoff by `mod_to_cutoff`
cents at full scale. The vibrato LFO (`vibrato_delay`, `vibrato_rate` in Hz,
`vibrato_depth` in cents) moves pitch. The modulation LFO (`lfo_delay`,
`lfo_rate`) moves pitch by `lfo_to_pitch` cents, the cutoff by
`lfo_to_cutoff` cents and the level by `lfo_to_volume` dB at its peak. Both
LFOs are triangles that start at zero, rising, once their delay is over.
`cutoff` and `vibrato_depth` are live properties.

These are worked out every 32 frames, which at 48 kHz is about fourteen
times a cycle of the fastest LFO a SoundFont can ask for (110 Hz). A voice that uses none of them
renders exactly as before and costs the same. One that does costs about
1.7 times as much on a desktop; the numbers for a board are in
[what the filter and the LFOs cost](../how-it-works/upstream-diff.md#what-the-filter-and-the-lfos-cost).

## How it renders

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
