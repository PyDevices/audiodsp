# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""What a Splitter does once its source runs out.

    route_dry_probe.py audioroute        the port

Separate from route_probe.py because it needs a source that finishes, and the
only one available to all of these interpreters is `audiospeed.SpeedChanger`
over a short RawSample - a RawSample on its own hands out its whole buffer
forever. CircuitPython 10.2.1 ships audiospeed but its unix `coverage` variant
does not compile it, so this probe is not part of that interpreter's gate - a
stated skip in `verify_dsp`'s table rather than a silent one. The same shared C
runs there either way, and enabling a module a build leaves out to suit a test
would be the wrong trade.
"""

import sys
from array import array

import audiocore
import audiospeed

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audioroute"
route = __import__(MODULE)

SAMPLE_RATE = 48000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def source(frames):
    values = array("h")
    for frame in range(frames):
        for channel in range(2):
            values.append(((frame * (37 + channel * 11) + channel * 500)
                           % 24001) - 12000)
    return audiospeed.SpeedChanger(
        audiocore.RawSample(values, sample_rate=SAMPLE_RATE, channel_count=2),
        1.0)


def emit(tag, tap, blocks, index=0):
    for block in range(blocks):
        data = bytes(audiocore.get_buffer(tap)[1])
        print("dry", tag, index, block, len(data), sum(data), checksum(data))


# The taps keep answering with silence rather than reporting themselves
# finished: the graph around them is still running. The trailing partial block
# before the silence is the part that is easy to get wrong.
splitter = route.Splitter(source(400), 2)
emit("first", splitter.tap(0), 7)
emit("second", splitter.tap(1), 7, 1)

# One tap alone, so every pull it makes is its own.
splitter = route.Splitter(source(300), 1)
emit("single", splitter.tap(0), 6)

# Dry from the very first pull.
splitter = route.Splitter(source(0), 2)
emit("empty", splitter.tap(0), 3)
emit("empty-other", splitter.tap(1), 3, 1)

print("done route-dry")
