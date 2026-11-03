# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Streaming, looping, panning, live-property, and tail parity probes."""

from array import array

import audiocore
import audiodelays
import audiomixer


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def sample(channels=1, frames=300):
    values = array("h")
    for frame in range(frames):
        for channel in range(channels):
            values.append(((frame * (131 + channel * 26)) % 28001) - 14000)
    return audiocore.RawSample(
        values, sample_rate=8000, channel_count=channels)


for channels in (1, 2):
    for frequency_shift in (False, True):
        effect = audiodelays.Echo(
            max_delay_ms=80, delay_ms=40, decay=.47, mix=.64,
            buffer_size=512, sample_rate=8000, channel_count=channels,
            freq_shift=frequency_shift,
        )
        effect.play(sample(channels))
        for index in range(7):
            if index == 2:
                effect.decay = .29
                effect.mix = .81
            data = bytes(audiocore.get_buffer(effect)[1])
            print("echo", channels, int(frequency_shift), index,
                  len(data), sum(data), checksum(data))

for channels in (1, 2):
    mixer = audiomixer.Mixer(
        voice_count=2, buffer_size=512, sample_rate=8000,
        channel_count=channels,
    )
    mixer.voice[0].level = .73
    mixer.voice[0].panning = -.61
    mixer.voice[1].level = .42
    mixer.voice[1].panning = .37
    mixer.voice[0].play(sample(channels, 173), loop=True)
    mixer.voice[1].play(sample(channels, 389))
    for index in range(5):
        data = bytes(audiocore.get_buffer(mixer)[1])
        print("mixer", channels, index, len(data), sum(data), checksum(data))
