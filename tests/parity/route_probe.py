"""Deterministic Splitter PCM, from whichever module provides the node.

    route_probe.py audioroute        the port

Only the positional `Splitter(source, taps)` form is used here; the keyword
form belongs in the unit tests. The oracle invocation this file used to
carry is gone with the oracle itself - see docs/how-it-works/correctness-standard.md.

The source is a double-buffered RawSample, which hands out 300 frames at a
time - enough to walk the ring past its 8192-frame wrap, and short enough that
the 256-frame chunk cap shows up in the block lengths. A source that runs *out*
needs something RawSample cannot do; see route_dry_probe.py.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audioroute"
# Both spellings are built-in modules under MicroPython, which does not record
# those in sys.modules - take what __import__ hands back.
route = __import__(MODULE)

SAMPLE_RATE = 48000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def source(frames=600):
    """A stream whose content keeps changing, so a skipped span is visible."""
    values = array("h")
    for frame in range(frames):
        for channel in range(2):
            values.append(((frame * (37 + channel * 11) + channel * 500)
                           % 24001) - 12000)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=2, single_buffer=False)


def emit(tag, tap, blocks, index=0):
    for block in range(blocks):
        data = bytes(audiocore.get_buffer(tap)[1])
        print("route", tag, index, block, len(data), sum(data),
              checksum(data))


# Every tap count, each tap read in turn: the first one to ask refills the
# ring, the rest read what it wrote.
for taps in (1, 2, 3, 4):
    splitter = route.Splitter(source(), taps)
    for index in range(taps):
        emit("taps%d" % taps, splitter.tap(index), 5, index)

# Interleaved, which is how a Mixer actually pulls them.
splitter = route.Splitter(source(), 3)
handles = [splitter.tap(index) for index in range(3)]
for block in range(6):
    for index, tap in enumerate(handles):
        data = bytes(audiocore.get_buffer(tap)[1])
        print("route interleaved", index, block, len(data), sum(data),
              checksum(data))

# tap(i) is the same object every time - the taps are built with the Splitter,
# not on demand, because the ring drops what an unclaimed tap never collects.
splitter = route.Splitter(source(), 2)
print("route identity", splitter.tap(1) is splitter.tap(1))

# A laggard tap: read one branch past the ring's depth (8192 frames) while the
# other sits idle, then read the idle one. Its cursor has been dragged forward
# and the span it never collected is gone.
splitter = route.Splitter(source(), 2)
lead = splitter.tap(0)
for _ in range(40):          # 40 reads of up to 256 frames, past 8192
    bytes(audiocore.get_buffer(lead)[1])
emit("laggard", splitter.tap(1), 4, 1)
emit("laggard-lead", lead, 3, 0)

# reset_buffer on a tap does nothing on purpose: the cursors belong to the
# Splitter and the other branches are still reading against them.
splitter = route.Splitter(source(), 2)
first = splitter.tap(0)
emit("before-reset", first, 3)
audiocore.reset_buffer(first)
emit("after-reset", first, 3)

# Out-of-range tap indices, including the negative one that used to arrive
# here as a very large unsigned number.
splitter = route.Splitter(source(), 2)
for index in (2, 4, -1):
    try:
        splitter.tap(index)
        print("route range", index, "no error")
    except ValueError as error:
        print("route range", index, error)

for count in (0, 5):
    try:
        route.Splitter(source(), count)
        print("route taps", count, "no error")
    except ValueError as error:
        print("route taps", count, error)

print("done route")
