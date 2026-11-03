# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Small cross-runtime synthesis probes used while tightening PCM parity."""

from array import array

import audiocore
import audiomixer
import audiofilters
import audiodelays
import synthio


def pull(sample, count=4):
    output = bytearray()
    for _ in range(count):
        output.extend(audiocore.get_buffer(sample)[1])
    return len(output), sum(output)


def render(note, *, mixer_level=None):
    synth = synthio.Synthesizer(sample_rate=22050, channel_count=1)
    synth.press(note)
    if mixer_level is None:
        return pull(synth)
    mixer = audiomixer.Mixer(sample_rate=22050, channel_count=1,
                             buffer_size=1024, voice_count=1)
    mixer.voice[0].play(synth)
    mixer.voice[0].level = mixer_level
    return pull(mixer)


wave = array("h", (-32768, -16384, 0, 16384, 32767, 16384, 0, -16384))
print("default", render(synthio.Note(440)))
print("wave", render(synthio.Note(440, waveform=wave)))
print("envelope", render(synthio.Note(
    440, waveform=wave,
    envelope=synthio.Envelope(attack_time=.01, decay_time=.1,
                              sustain_level=.8, release_time=.4))))
print("filter", render(synthio.Note(
    440, waveform=wave,
    filter=synthio.Biquad(synthio.FilterMode.LOW_PASS, 800, 1.4))))
print("mixer", render(synthio.Note(440, waveform=wave), mixer_level=.5))

synth = synthio.Synthesizer(sample_rate=22050, channel_count=1)
synth.press((
    synthio.Note(220, waveform=wave, amplitude=.625,
                 filter=synthio.Biquad(synthio.FilterMode.LOW_PASS, 800, 1.4)),
    synthio.Note(220 * 1.004, waveform=wave, amplitude=.375,
                 filter=synthio.Biquad(synthio.FilterMode.LOW_PASS, 800, 1.4)),
))
print("two_voice", pull(synth))

for mode_index, mode in enumerate((audiofilters.DistortionMode.CLIP,
                                   audiofilters.DistortionMode.LOFI,
                                   audiofilters.DistortionMode.OVERDRIVE,
                                   audiofilters.DistortionMode.WAVESHAPE)):
    source = audiocore.RawSample(array("h", (wave[i % len(wave)] for i in range(256))),
                                 sample_rate=22050)
    effect = audiofilters.Distortion(
        drive=.3, pre_gain=2, post_gain=-1, mode=mode, mix=.4,
        sample_rate=22050, channel_count=1, buffer_size=512)
    effect.play(source)
    print("distortion", mode_index, pull(effect, 1))

source = audiocore.RawSample(array("h", (wave[i % len(wave)] for i in range(256))),
                             sample_rate=22050)
echo = audiodelays.Echo(max_delay_ms=10, delay_ms=5, decay=.35, mix=.3,
                        sample_rate=22050, channel_count=1, buffer_size=512)
echo.play(source)
for index in range(4):
    data = bytes(audiocore.get_buffer(echo)[1])
    print("echo", index, len(data), sum(data))

source = audiocore.RawSample(array("h", (wave[i % len(wave)] for i in range(256))),
                             sample_rate=22050)
transport_filter = audiofilters.Filter(
    filter=synthio.Biquad(synthio.FilterMode.LOW_PASS, 800, 1.4),
    mix=.75, sample_rate=22050, channel_count=1, buffer_size=512)
transport_filter.play(source)
print("transport_filter", pull(transport_filter, 2))

lfo = synthio.LFO(rate=5.5, scale=.006, offset=.02)
math_block = synthio.Math(synthio.MathOperation.SUM, lfo, .1, -.2)
print("blocks", tuple(round(value, 9) for value in synthio.lfo_tick(
    lfo, math_block, lfo, math_block)))
