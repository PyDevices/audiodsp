"""A sample-and-hold built out of two SpeedChangers, over several block sizes.

    speedchanger_hold_probe.py audiospeed

`resampler_probe.py` next door pins the node's arithmetic at 2.0, 1.0 and 0.5.
Those are exact in Q16 *and* they divide a buffer exactly, so the accumulator is
back at zero every time a new source buffer arrives and nothing about how it
crosses that boundary can show. This probe asks for ratios that do neither.

What it renders is the shape the palette actually uses: decimate at N with one
`SpeedChanger`, restore at 1/N with another. The interesting property is that
**the same hold must render the same bytes whatever block size the source hands
out**, since the accumulator counts source frames and a buffer boundary is not
one. The same material is therefore pushed through at 64, 100 and 256 frames a
buffer and all three are in the printed output; upstream CircuitPython 10.3.0
restarts its phase at each buffer, so there they differ.

The three `rate` lines are Q16 integers rather than floats on purpose: a float
printed by three interpreters is a test of their `repr`, and the Q16 value is
what the node actually holds. All three are far enough from a half-step to
round the same way under a 32-bit and a 64-bit `mp_float_t`.

Both departures this probe sees are recorded in `docs/upstream-diff.md`
(audiodsp#91, audiodsp#92).
"""

import sys
from array import array

import audiocore
import audiofilters

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiospeed"
speed = __import__(MODULE)

SAMPLE_RATE = 48000
FRAMES = 1536


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def material(frames):
    values = array("h")
    for frame in range(frames):
        for channel in range(2):
            values.append(((frame * (53 + channel * 17) + channel * 300)
                           % 20001) - 10000)
    return values


def blocks(values, block):
    """A source that hands out `block` frames at a time. A Filter with nothing
    set is a wire, and its buffer_size is in bytes."""
    node = audiofilters.Filter(sample_rate=SAMPLE_RATE, channel_count=2,
                               buffer_size=block * 4)
    node.play(audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                                  channel_count=2))
    return node


def hold(values, down, block):
    return speed.SpeedChanger(
        speed.SpeedChanger(blocks(values, block), down), 1.0 / down)


def emit(tag, node, pulls):
    tail = []
    for index in range(pulls):
        data = bytes(audiocore.get_buffer(node)[1])
        print("spd hold", tag, index, len(data), sum(data), checksum(data))
        tail.extend(array("h", data))
    return tail


def runs(values):
    """Run lengths of the first channel. A hold at N puts every one of them in
    a two-value alphabet, unless the material repeats a value itself."""
    counts = {}
    length = 1
    for index in range(2, len(values), 2):
        if values[index] == values[index - 2]:
            length += 1
        else:
            counts[length] = counts.get(length, 0) + 1
            length = 1
    return counts


for asked, wanted in ((0.5 - 1e-6, 32768),
                      (1.0 / 1.0000000000000004, 65536),
                      (48000.0 / 44100.0, 71332)):
    node = speed.SpeedChanger(blocks(material(64), 64), asked)
    print("spd rate", wanted, int(round(node.rate * 65536)))

for down in (1.8433, 2.5, 6.0):
    for block in (64, 100, 256):
        tail = emit("%s-%d" % (down, block),
                    hold(material(FRAMES), down, block), 3)
        for length in sorted(runs(tail)):
            print("spd runs", down, block, length, runs(tail)[length])

# Upward, where the accumulator's carry is a fraction of a frame rather than
# whole frames, and where a source buffer is consumed over several pulls.
emit("0.3-100", speed.SpeedChanger(blocks(material(FRAMES), 100), 0.3), 4)

print("done speedchanger-hold")
