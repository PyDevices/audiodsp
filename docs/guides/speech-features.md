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

## Is anyone speaking?

Each slice also carries a voice activity flag: 1 when it holds speech, 0
when it holds silence or steady noise. Pass a second buffer, one byte a
slice, and `readinto()` fills it beside the features; `speaking()` is the
newest slice's flag on its own.

```python
voice = bytearray(10)
n = fe.readinto(buf, voice)            # voice[i] is slice i's flag
if fe.speaking():
    ...                                # someone is talking right now
```

A wake word detector can skip the model while every flag is 0, and an
intercom can hold the floor while `speaking()` is true.
[`examples/audiofeatures_voice_activity.py`](../../examples/audiofeatures_voice_activity.py)
prints where the speech is in a WAV file. `readinto_raw()` takes the same
second buffer. Attached to a tap or fed from Python, at any rate, the flags
come with their slices through the same queue.

### How it decides

The detector reads the frontend's 40 band levels before noise reduction,
in steps of a 64th of an octave, and keeps a noise floor for each band.
The floor follows the quiet parts quickly and rises slowly, so a fan or
road noise that stays on is learnt within a few seconds and stops counting.
A slice's score is how far its four loudest bands stand above their floors,
averaged over the last 50 ms; over about 10 dB is speech. A flag stays on
for 100 ms after the score drops, so the gaps between words don't break a
sentence into pieces. All of it is integer arithmetic, so CPython,
MicroPython and a board give the same flags for the same audio.

The first 160 ms after the frontend starts or `reset()` is spent learning
the floor and is never flagged, so reset it in quiet, not mid-word.
Digital silence (a stream of zeros) is left out of the floor, so it doesn't
make the noise that follows it look like speech.

### How well it works

We scored it against Silero VAD, a neural voice detector, on 97
recordings: 72 spoken phrases, 23 longer recordings and sounds, and two
of steady noise, with the phrases also mixed with loud noise and turned
down 30 dB. Each clip had two seconds of its own
background first, as an always-on device would hear. The fraction of
10 ms slices on which the two agree:

| Audio | Agreement | Speech found | Noise called speech |
| --- | --- | --- | --- |
| Clean spoken phrases | 92% | 99.7% | 14% |
| Phrases with noise 10 dB below the speech | 97% | 96% | 1.7% |
| Phrases with noise as loud as the speech | 90% | 78% | 0.3% |
| Phrases 30 dB quieter | 97% | 99% | 4% |
| Longer recordings | 92% | 97% | 28% |
| Short spoken replies and alert sounds | 80% | 100% | 28% |
| 40 seconds of steady noise | | | 0.3% |

Most of the "noise called speech" on clean audio is the 100 ms hold
running past where Silero stops. Speech starts within 30 ms of Silero's
start. Fed at 48 kHz instead, every figure in the table is the same to the
nearest 0.1%.

On the same clips WebRTC's voice detector at its most aggressive setting
agreed 96% on clean phrases, but called the noisy mixes speech throughout
(55% and 42%) and missed most of the quiet phrases (62%), which is why
this one is built on the frontend's own bands instead.

What it gets wrong: a tone that comes and goes, like an alarm or a
doorbell, looks like speech, because nothing here knows what a voice
sounds like. Speech buried under noise louder than itself is mostly
missed. If you need either, run a model on the slices it flags.

### What it costs

About 0.7 KB of flash on an ESP32-P4 on top of the frontend, 176 bytes of
state, and a byte for each slice in the queue. Nothing to set up and no
settings: if you want a longer hold, keep a flag on in Python for as many
slices as you like.

## The queue

Slices wait in a queue of `slices` (100 by default, a second of audio) until
you read them. When it is full, the oldest slice goes and `stats()["dropped"]`
counts it, so a reader that falls behind loses old audio, never new.
`reset()` forgets the audio and the queue, for the start of a new utterance.

A frontend takes about 17 to 26 KB of heap with the default queue, depending
on the rate: 8 KB is the queue and its voice flags, the rest the frontend's
state and the resampler's filter.

CircuitPython doesn't get `audiofeatures`; see
[additions](../how-it-works/additions.md).
