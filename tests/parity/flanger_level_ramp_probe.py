# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""The Flanger's `mix` walk over its `mix_ramp` frames, the same bytes on every target.

    flanger_level_ramp_probe.py audiodelays

    RAMP  <case>  <checksum of each pull>

`audiodelays.Flanger.mix_ramp` is this port's, not CircuitPython's
(`docs/dev/upstream-record.md`), so `verify_dsp.py` doesn't run this probe there.
The other nodes' walks, which every target has, are in
`node_level_ramp_probe.py`. The Flanger's `mix` is moved on a playing node,
moved before the first pull, and moved and then reset halfway through the
walk, mono and stereo, at 48 and 22.05 kHz. Its CPython twin is Python, so this
is where it meets the C.
"""

from array import array

import audiocore
import audiodelays

PULLS = 12
MOVE_AT = 3


def checksum(data):
    value = 2166136261
    for byte in bytes(data):
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def tone(rate, channels):
    """A 200 Hz triangle in whole cycles, worked in integers so a
    single-precision build plays the same input, and longer than PULLS
    pulls of 256 frames: a node's play() takes no loop on MicroPython."""
    cycle = rate // 200
    values = array("h")
    for i in range(cycle * 30):
        t = 48000 * (i % cycle) // cycle
        v = t if t < 12000 else (24000 - t if t < 36000 else t - 48000)
        values.append(v)
        if channels == 2:
            values.append(v * 7 // 10)
    return audiocore.RawSample(values, sample_rate=rate,
                               channel_count=channels)


def flanger(rate, channels, name, value):
    node = audiodelays.Flanger(max_delay_ms=10, min_delay_ms=2.0, rate=0.5,
                               depth=0.5, feedback=0.4, mix=value,
                               sample_rate=rate, channel_count=channels,
                               buffer_size=512 * channels)
    node.mix_ramp = (rate * 20 + 500) // 1000
    return node


def by_attr(node, name, value):
    setattr(node, name, value)


CASES = (
    ("flanger-mix", flanger, by_attr, "mix", 0.0, 1.0),
    ("flanger-mix-down", flanger, by_attr, "mix", 0.8, 0.0),
)


def render(node, rate, channels, move_at, move, reset_at=None):
    node.play(tone(rate, channels))
    sums = []
    for index in range(PULLS):
        if index == move_at:
            move(node)
        if index == reset_at:
            audiocore.reset_buffer(node)
        sums.append(checksum(audiocore.get_buffer(node)[1]))
    node.deinit()
    return sums


for name, build, mover, control, old, new in CASES:
    for rate in (48000, 22050):
        for channels in (1, 2):
            def move(node):
                mover(node, control, new)
            for tag, move_at, reset_at in (("move", MOVE_AT, None),
                                           ("first", 0, None),
                                           ("reset", MOVE_AT, MOVE_AT + 1)):
                print("RAMP", "%s-%s-%d-%dch" % (name, tag, rate, channels),
                      render(build(rate, channels, control, old), rate,
                             channels, move_at, move, reset_at))

print("done flanger-level-ramp")
