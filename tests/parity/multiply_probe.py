"""Deterministic Multiply PCM.

    multiply_probe.py audiomath

Unlike dynamics_probe.py and route_probe.py this one has no oracle to be
compared against: `audiomath` is audiodsp's own module, with no ancestor in
CircuitPython or in micropython-vst3's engine. What the golden pins is that
every interpreter renders it identically -- the arithmetic is entirely inside
shared/audiodsp_multiply.c, the same C all three link, so a disagreement
between two of them would itself be the finding.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiomath"
# A built-in module under MicroPython, which does not record those in
# sys.modules - take what __import__ hands back.
math_module = __import__(MODULE)

SAMPLE_RATE = 48000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def signal(frames=1100, level=12000):
    """A stereo ramp that walks the whole int16 range, so the product's
    saturation clamp is reached rather than assumed."""
    values = array("h")
    for frame in range(frames):
        for channel in range(2):
            shape = ((frame * (97 + channel * 18)) % 2001) - 1000
            values.append(shape * level // 1000)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=2)


def modulator(length, amplitude=32767):
    """A short table, deliberately not a multiple of the 256-frame output
    block: the loop back to its start has to fall inside a block, which is
    where a phase step would show up."""
    values = array("h")
    for frame in range(length):
        # A triangle rather than a sine: no floating point, so the fixture
        # cannot move because two interpreters round a transcendental
        # differently.
        position = (frame * 4) % (length * 4)
        if position < length * 2:
            level = position - length
        else:
            level = length * 3 - position
        values.append(level * amplitude // length)
        values.append(level * amplitude // length)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=2)


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        print("mul", tag, index, len(data), sum(data), checksum(data))


# Table lengths either side of the block size, and mixes at both rails plus a
# fraction that exercises the blend.
for length in (97, 256, 700):
    for mix in (0.0, 0.35, 1.0):
        node = math_module.Multiply(signal(), modulator(length), mix=mix)
        emit("%d/%.2f" % (length, mix), node, 5)

# Rails: both operands full scale, which is the one product that lands outside
# int16 and has to clamp rather than wrap.
rails = array("h")
for frame in range(600):
    rails.append(32767 if frame % 2 else -32768)
    rails.append(-32768 if frame % 2 else 32767)
node = math_module.Multiply(
    audiocore.RawSample(rails, sample_rate=SAMPLE_RATE, channel_count=2),
    audiocore.RawSample(rails, sample_rate=SAMPLE_RATE, channel_count=2),
    mix=1.0)
emit("rails", node, 4)

# No modulator at all: the signal has to pass through untouched, which is the
# one place "absent" and "zero" must not mean the same thing.
node = math_module.Multiply(signal(), mix=1.0)
emit("bare", node, 4)

# Set mid-stream, and a modulator attached late.
node = math_module.Multiply(signal(), mix=1.0)
emit("late-a", node, 2)
node.modulate(modulator(300))
emit("late-b", node, 2)
node.set(mix=0.5)
emit("late-c", node, 2)

# No source: silence, never a short block, never finished.
node = math_module.Multiply(modulator=modulator(128))
emit("starved", node, 2)
