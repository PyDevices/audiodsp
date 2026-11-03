# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Deterministic Resampler PCM.

    resampler_probe.py audiospeed

CircuitPython 10.3.0 added the node. Playing it into a destination at a
different sample rate is what binds the ratio; the CPython twin has to take
that same path.
"""

import sys
from array import array

import audiocore
import audiofilters

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiospeed"
speed = __import__(MODULE)

SOURCE_RATE = 16000
DEST_RATE = 8000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def source(frames=2000, level=14000, rate=SOURCE_RATE):
    values = array("h")
    for frame in range(frames):
        for channel in range(2):
            shape = ((frame * (97 + channel * 18)) % 2001) - 1000
            values.append(shape * level // 1000)
    return audiocore.RawSample(values, sample_rate=rate, channel_count=2)


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        print("rsm", tag, index, len(data), sum(data), checksum(data))


def play_through(tag, resampler, dest_rate, blocks):
    node = audiofilters.Filter(
        sample_rate=dest_rate, channel_count=2, buffer_size=512)
    node.play(resampler)
    emit(tag, node, blocks)


play_through("half", speed.Resampler(source()), DEST_RATE, 6)
play_through("same", speed.Resampler(source(rate=DEST_RATE)), DEST_RATE, 4)
play_through("double", speed.Resampler(source(rate=4000)), DEST_RATE, 4)

node = audiofilters.Filter(
    sample_rate=DEST_RATE, channel_count=2, buffer_size=512)
node.play(speed.Resampler(source()))
emit("late-a", node, 2)
node.stop()
node.play(speed.Resampler(source(level=8000)))
emit("late-b", node, 2)
