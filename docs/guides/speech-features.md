# Speech features

`audiofeatures` turns audio into what a microWakeWord model listens to: every
10 ms, a slice of 40 values, one per mel band from 125 Hz to 7.5 kHz. Hand
the slices to a TensorFlow Lite Micro model and you have a wake word
detector; the model and the interpreter that runs it aren't part of audiodsp.

```python
fe = audiofeatures.Frontend()
fe.feed(pcm, 48000, 2)                 # interleaved s16, any rate from 16 kHz
buf = bytearray(audiofeatures.FEATURE_SIZE * 10)
n = fe.readinto(buf)                   # n whole slices of 40 int8
```

[`examples/audiofeatures_simpletest.py`](../../examples/audiofeatures_simpletest.py)
prints the slices of a WAV file, one row of characters each.

On a board it can listen without Python touching a sample.
`fe.attach(tap, 48000)` reads an `audiopump.Tap` in C each time you call
`readinto()`, so whatever the pump is playing or recording, an I2S
microphone or a USB sound card, becomes slices. A tap runs at the pump's
rate and channel count; give `attach()` the rate.

## What a slice is

The frontend is TensorFlow Lite Micro's audio microfrontend, carried in
[`third_party/microfrontend`](../../third_party/microfrontend/ORIGIN.txt),
with the settings ESPHome's micro_wake_word models were trained on: a 30 ms
window every 10 ms, 40 channels, noise reduction, PCAN gain control and a
log scale. `readinto()` gives each value as the int8 those models take,
`(v * 256 + 333) // 666 - 128`, and `audiofeatures.quantize()` is that sum.
`readinto_raw()` gives the frontend's own uint16 values instead, about 0 to
670.

At 16 kHz the samples go to the frontend untouched, and the bytes are the
ones TensorFlow Lite Micro's own C++ build makes, in any block size.
[`tests/test_cpython_audiofeatures.py`](../../tests/test_cpython_audiofeatures.py)
holds goldens that build wrote.

## Other rates

Audio at any other rate up to 384 kHz is resampled to 16 kHz first, through
a windowed-sinc low-pass that is flat to 7.5 kHz, where the top mel band
ends, and about 70 dB down from 8.5 kHz up. Stereo and wider audio is mixed
to mono before it. The features can't be byte-identical to the 16 kHz ones,
but they are close: on 74 spoken clips fed at 48 kHz, 95% of the int8 values
are the same as at 16 kHz and the mean difference is 0.27 of a step, and
three microWakeWord models detected and missed exactly the same clips, with
peak probabilities within 0.03.

## The queue

Slices wait in a queue of `slices` (100 by default, a second of audio) until
you read them. When it is full, the oldest slice goes and `stats()["dropped"]`
counts it, so a reader that falls behind loses old audio, never new.
`reset()` forgets the audio and the queue, for the start of a new utterance.

A frontend takes about 17 to 26 KB of heap with the default queue, depending
on the rate: 8 KB is the queue, the rest the frontend's state and the
resampler's filter.

CircuitPython doesn't get `audiofeatures`; see
[additions](../how-it-works/additions.md).
