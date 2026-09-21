"""Deterministic MidSide PCM.

    midside_probe.py audioroute

Like multiply_probe.py, this has no oracle. `audioroute.Splitter` came from
micropython-vst3's engine, but `MidSide` did not: it is audiodsp's own, with no
ancestor in CircuitPython or in that engine either. What the golden pins is
that every interpreter renders it identically -- the arithmetic is entirely
inside shared/audiodsp_midside.c, the same C all three link, so a disagreement
between two of them would itself be the finding.

Two things a checksum cannot say are asserted here as well, because they are
the node's contract rather than its output:

- at ``width=1`` the output bytes are the input bytes, for every int16 pair.
  A checksum would go on matching a golden captured from a node that had
  quietly started costing the chain an LSB; only the comparison catches it.
- the mono sum survives, to within one LSB per frame, at every width. That is
  what says the halving rounds both halves the same way rather than tilting
  the balance as the width moves.

Both print their result, so the golden covers them too.
"""

import struct
import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audioroute"
# A built-in module under MicroPython, which does not record those in
# sys.modules - take what __import__ hands back.
route = __import__(MODULE)

SAMPLE_RATE = 48000
# 0 and 2 are the rails, 1 is the identity, and the two halves either side of
# it are where the rounding has to hold. 0.35 is not representable in binary,
# so it also says the float -> Q14 conversion lands on the same integer under
# all three interpreters.
WIDTHS = (0.0, 0.35, 0.5, 1.0, 1.5, 2.0)


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def frames(count=1100, level=12000):
    """A decorrelated stereo ramp: the two channels walk at different rates,
    so the difference between them is never zero for long and the matrix has
    something to scale."""
    values = array("h")
    for frame in range(count):
        left = (((frame * 97) % 2001) - 1000) * level // 1000
        right = (((frame * 131) % 1777) - 888) * level // 888
        values.append(left)
        values.append(right)
    return values


def signal(count=1100, level=12000, channel_count=2):
    if channel_count == 1:
        values = array("h")
        for frame in range(count):
            values.append((((frame * 97) % 2001) - 1000) * level // 1000)
    else:
        values = frames(count, level)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channel_count)


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        print("ms", tag, index, len(data), sum(data), checksum(data))


# The width sweep. `level=12000` leaves the rails alone; the pass below
# reaches them.
for width in WIDTHS:
    node = route.MidSide(signal(), width=width)
    emit("w%.2f" % width, node, 4)

# Near full scale at width 2, which is the one setting whose numerator lands
# outside int16 and has to clamp rather than wrap.
for width in (1.5, 2.0):
    node = route.MidSide(signal(level=32000), width=width)
    emit("hot%.2f" % width, node, 4)

# Mono: there is no difference to scale, so every width is a passthrough.
for width in (0.0, 1.0, 2.0):
    node = route.MidSide(signal(channel_count=1), width=width,
                         channel_count=1)
    emit("mono%.2f" % width, node, 3)

# Set mid-stream, and a source attached late.
node = route.MidSide(width=1.5)
emit("late-a", node, 1)
node.play(signal())
emit("late-b", node, 2)
node.set(width=0.0)
emit("late-c", node, 2)

# Out of range both ways: clamped to the rails, not honoured.
node = route.MidSide(signal(), width=-3.0)
emit("under", node, 2)
node = route.MidSide(signal(), width=9.0)
emit("over", node, 2)

# No source at all: silence, never a short block, never finished.
node = route.MidSide()
emit("starved", node, 2)

# --- the two assertions a checksum cannot make -------------------------------

# 1. width=1 is the identity, byte for byte. Driven with the widest pair the
# format has as well as the ramp, because the rails are where a rounding that
# is one out shows first.
rails = array("h")
for frame in range(600):
    rails.append(32767 if frame % 2 else -32768)
    rails.append(-32768 if frame % 3 else 32767)
for name, values in (("ramp", frames()), ("rails", rails)):
    node = route.MidSide(
        audiocore.RawSample(values, sample_rate=SAMPLE_RATE, channel_count=2),
        width=1.0)
    source_bytes = bytes(values)  # array.tobytes() does not exist on MicroPython
    offset = 0
    same = True
    for _ in range(2):
        block = bytes(audiocore.get_buffer(node)[1])
        if block != source_bytes[offset:offset + len(block)]:
            same = False
        offset += len(block)
    print("ms identity", name, offset, same)
    assert same, "width=1 is not the identity for %s" % name

# 2. The mono sum survives to within one LSB per frame, at every width. Held
# at a level the clamp cannot reach: at width 2 an output is (3L - R) / 2, so
# anything under 16383 is safe, and past the clamp the bound is a property of
# the ceiling rather than of the matrix.
quiet = frames(level=15000)
worst = 0
for width in WIDTHS:
    node = route.MidSide(
        audiocore.RawSample(quiet, sample_rate=SAMPLE_RATE, channel_count=2),
        width=width)
    offset = 0
    for _ in range(2):
        data = bytes(audiocore.get_buffer(node)[1])
        # array.frombytes() does not exist on MicroPython; struct does on both.
        block = struct.unpack("<%dh" % (len(data) // 2), data)
        for index in range(0, len(block), 2):
            error = ((block[index] + block[index + 1]) -
                     (quiet[offset + index] + quiet[offset + index + 1]))
            if error < 0:
                error = -error
            if error > worst:
                worst = error
        offset += len(block)
print("ms monosum", worst)
assert worst <= 1, "the mono sum moved by %d" % worst
