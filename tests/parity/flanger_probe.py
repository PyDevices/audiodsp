# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Deterministic Flanger PCM.

    flanger_probe.py audiodelays

CircuitPython 10.3.0 added the node. This fixture holds the CPython twin to
the same bytes the native object renders.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiodelays"
delays = __import__(MODULE)

SAMPLE_RATE = 8000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def source(frames=1600, level=14000):
    values = array("h")
    for frame in range(frames):
        for channel in range(2):
            if frame < 400:
                shape = ((frame * (97 + channel * 18)) % 2001) - 1000
                values.append(shape * level // 1000)
            else:
                values.append(0)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=2)


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        print("flg", tag, index, len(data), sum(data), checksum(data))


CASES = (
    ("plain", {"max_delay_ms": 10, "min_delay_ms": 1.0, "rate": 0.5,
               "depth": 0.5, "feedback": 0.5, "mix": 1.0}),
    ("still", {"max_delay_ms": 10, "min_delay_ms": 2.0, "rate": 0.0,
               "depth": 0.8, "feedback": 0.4, "mix": 1.0}),
    ("deep", {"max_delay_ms": 8, "min_delay_ms": 1.0, "rate": 1.7,
              "depth": 1.0, "feedback": 0.85, "mix": 0.6}),
    ("invert", {"max_delay_ms": 10, "min_delay_ms": 1.5, "rate": 0.3,
                "depth": 0.7, "feedback": -0.6, "mix": 1.0, "invert": True}),
    ("dry", {"max_delay_ms": 10, "min_delay_ms": 1.0, "rate": 2.0,
             "depth": 1.0, "feedback": 0.7, "mix": 0.0}),
)

for name, options in CASES:
    node = delays.Flanger(sample_rate=SAMPLE_RATE, channel_count=2,
                          buffer_size=512, **options)
    node.play(source())
    emit(name, node, 6)

node = delays.Flanger(sample_rate=SAMPLE_RATE, channel_count=2, buffer_size=512,
                      max_delay_ms=10, mix=1.0, rate=0.8, depth=0.6,
                      feedback=0.5)
node.play(source())
emit("late-a", node, 2)
node.rate = 4.0
node.invert = True
emit("late-b", node, 2)

node = delays.Flanger(sample_rate=SAMPLE_RATE, channel_count=2, buffer_size=512)
emit("starved", node, 2)


def rails(frames=2400, level=32767):
    """Full-scale alternating: adjacent frames at opposite rails.

    This is the case that caught a real divergence, and it is here so it cannot
    come back. The wet tap interpolates between two neighbouring line samples,
    `s0 + ((s1 - s0) * delay_frac >> 16)`, and with the line holding opposite
    rails `s1 - s0` reaches 65535 while `delay_frac` reaches 65535 too - a
    product of 4.29e9, past INT32_MAX. The CPython twin computes it in Python
    and does not overflow; the first MicroPython port did it in `int32_t` and
    diverged on every setting tried here, while every other fixture in this
    file matched to the byte. Ordinary musical material never reaches it, which
    is exactly why it needs its own case.
    """
    values = array("h")
    for frame in range(frames):
        values.append(level if frame % 2 else -level)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=1)


for name, options in (("rails", {"max_delay_ms": 10, "mix": 1.0}),
                      ("rails-deep", {"max_delay_ms": 10, "mix": 1.0,
                                      "rate": 8.0, "depth": 1.0,
                                      "feedback": 0.9}),
                      ("rails-fb", {"max_delay_ms": 10, "mix": 1.0,
                                    "rate": 3.0, "depth": 1.0,
                                    "feedback": 0.95})):
    node = delays.Flanger(sample_rate=SAMPLE_RATE, channel_count=1,
                          buffer_size=512, **options)
    node.play(rails())
    emit(name, node, 6)
