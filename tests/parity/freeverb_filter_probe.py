# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Deterministic Freeverb pre_filter / post_filter PCM.

    freeverb_filter_probe.py audiofreeverb

The first case sets neither filter. Its bytes have to be the bytes Freeverb
produced before the properties existed: an empty chain returns the word
unchanged.
"""

import sys
from array import array

import audiocore
import synthio

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiofreeverb"
freeverb = __import__(MODULE)

SAMPLE_RATE = 8000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def source(frames=1600, level=14000, channels=1):
    values = array("h")
    for frame in range(frames):
        for channel in range(channels):
            if frame < 120:
                shape = ((frame * (97 + channel * 18)) % 2001) - 1000
                values.append(shape * level // 1000)
            else:
                values.append(0)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        print("fvf", tag, index, len(data), sum(data), checksum(data))


verb = freeverb.Freeverb(
    roomsize=0.5, damp=0.5, mix=0.7, sample_rate=SAMPLE_RATE,
    channel_count=1, buffer_size=512)
verb.play(source())
emit("bare", verb, 6)

pre = synthio.Biquad(synthio.FilterMode.HIGH_PASS, 200, 0.7)
verb = freeverb.Freeverb(
    roomsize=0.5, damp=0.5, mix=0.7, pre_filter=pre,
    sample_rate=SAMPLE_RATE, channel_count=1, buffer_size=512)
verb.play(source())
emit("pre", verb, 6)

post = synthio.Biquad(synthio.FilterMode.LOW_PASS, 2000, 0.7)
verb = freeverb.Freeverb(
    roomsize=0.6, damp=0.4, mix=0.8, post_filter=post,
    sample_rate=SAMPLE_RATE, channel_count=1, buffer_size=512)
verb.play(source())
emit("post", verb, 6)

verb = freeverb.Freeverb(
    roomsize=0.5, damp=0.5, mix=0.7,
    pre_filter=synthio.Biquad(synthio.FilterMode.HIGH_PASS, 180, 0.8),
    post_filter=synthio.Biquad(synthio.FilterMode.LOW_PASS, 1800, 0.8),
    sample_rate=SAMPLE_RATE, channel_count=1, buffer_size=512)
verb.play(source())
emit("both", verb, 6)

verb = freeverb.Freeverb(
    roomsize=0.5, damp=0.5, mix=0.7, sample_rate=SAMPLE_RATE,
    channel_count=1, buffer_size=512)
verb.play(source())
emit("late-a", verb, 2)
verb.pre_filter = synthio.Biquad(synthio.FilterMode.LOW_PASS, 900, 0.7)
emit("late-b", verb, 2)
