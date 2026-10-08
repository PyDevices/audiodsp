"""A tail rings out after its source ends, and a node at rest is empty.

    tail_rest_probe.py

The lifecycle audiodsp#180 asks for, on the three nodes it names:
`audioecho.FeedbackDelay`, `audioverb.Tank` and `audioconvolve.Convolver`.

1. **A burst.** A short `RawSample` that is not a whole number of blocks, so
   it ends part-way through one. Every block the node hands on is full.
2. **A stop.** The `RawSample` hands its one buffer with `GET_BUFFER_DONE`.
   The node lets go of it after that buffer, rather than fetching it again on
   the next block and replaying it for ever.
3. **The ring to exact zero.** After the burst the node renders from silence.
   What it renders must be byte for byte what the same node renders when it
   is fed the burst followed by explicit silence, so the tail is the tail,
   not a frozen or a replayed one. It reaches exact zero within a bounded
   number of blocks and stays there; the block at which it does is printed,
   so `verify_dsp.py` holds the interpreters to the same answer.
4. **A second burst into a node at rest.** It must render exactly what a
   newly built node renders from the same burst: nothing of the first take
   is left anywhere in the node.

The nodes are built with their modulation off, because a resting node stops
its oscillators where they are, and a new node starts them from the top.
"""

import sys
from array import array

import audioconvolve
import audiocore
import audioecho
import audioverb

RATE = 48000
FRAMES = 256
BURST = 300           # ends 44 frames into the second block
LIMIT = 3000          # blocks a tail may take to reach exact zero
SETTLE = 64           # blocks pulled at rest to see it stays silent


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def burst(channels, seed):
    """A burst of decorrelated noise-like material."""
    values = array("h")
    state = seed
    for _ in range(BURST * channels):
        state = (state * 1103515245 + 12345) & 0x7fffffff
        values.append(((state >> 8) % 40001) - 20000)
    return values


def sample(values, channels):
    return audiocore.RawSample(values, sample_rate=RATE,
                               channel_count=channels)


def padded(values, channels):
    """The burst and then explicit silence to the end of its second block,
    so every later block of the reference is a block of zeros fed in."""
    out = array("h", values)
    for _ in range((2 * FRAMES - BURST) * channels):
        out.append(0)
    return sample(out, channels)


_IMPULSE = array("h", [24000] + [0] * 99 + [-12000] + [0] * 299 + [6000] +
                 [0] * 499 + [3000])


NODES = (
    ("audioecho.FeedbackDelay", lambda ch: audioecho.FeedbackDelay(
        sample_rate=RATE, max_delay_ms=40.0, delay_ms=23.0, feedback=0.7,
        mix=0.6, damping_hz=5000.0, cut_hz=120.0, channel_count=ch)),
    ("audioverb.Tank", lambda ch: audioverb.Tank(
        sample_rate=RATE, decay=0.45, damping_hz=6000.0, mod_depth_ms=0.0,
        channel_count=ch)),
    ("audioconvolve.Convolver", lambda ch: audioconvolve.Convolver(
        impulse=_IMPULSE, sample_rate=RATE, channel_count=ch)),
)


def block(node, width):
    data = bytes(audiocore.get_buffer(node)[1])
    if len(data) != FRAMES * width:
        raise AssertionError("a short block: %d bytes, not %d"
                             % (len(data), FRAMES * width))
    return data


def silent(data):
    for byte in data:
        if byte:
            return False
    return True


failures = []


def check(name, ok, what):
    if not ok:
        failures.append("%s: %s" % (name, what))


for name, build in NODES:
    for channels in (2, 1):
        tag = "%s/%d" % (name, channels)
        width = 2 * channels
        first_burst = burst(channels, 7)
        zeros = sample(array("h", [0] * (FRAMES * channels)), channels)

        # 1-3: the burst, the stop and the ring, block by block, beside the
        # same node fed the burst and then explicit silence -- one block of
        # zeros played into it before each pull.
        node = build(channels)
        node.play(sample(first_burst, channels))
        reference = build(channels)
        reference.play(padded(first_burst, channels))
        zero_at = None
        same = True
        whole = 2166136261
        index = 0
        while index < LIMIT:
            data = block(node, width)
            if index >= 2:
                reference.play(zeros)
            if block(reference, width) != data:
                same = False
            whole = checksum(bytes([whole & 0xff]) + data)
            if silent(data):
                if zero_at is None:
                    zero_at = index
            else:
                zero_at = None
            index += 1
            if zero_at is not None and index - zero_at > SETTLE:
                break
        check(tag, zero_at is not None and zero_at > 1,
              "the tail did not reach exact zero in %d blocks" % LIMIT)
        check(tag, same, "the ring-down is not what silence fed in renders")
        # At rest, and staying there.
        quiet = True
        for _ in range(SETTLE):
            if not silent(block(node, width)):
                quiet = False
        check(tag, quiet, "a node at rest rendered sound")

        # 4: a second burst into the node at rest, against a new node.
        second_burst = burst(channels, 11)
        node.play(sample(second_burst, channels))
        fresh = build(channels)
        fresh.play(sample(second_burst, channels))
        again = 2166136261
        alike = True
        for _ in range((zero_at or 0) + 4):
            data = block(node, width)
            if block(fresh, width) != data:
                alike = False
            again = checksum(bytes([again & 0xff]) + data)
        check(tag, alike,
              "a second burst into a node at rest is not a new node's")

        print("tail", tag, "zero_at", zero_at, "blocks", index, whole, again)

if failures:
    for line in failures:
        print("FAIL", line)
    sys.exit(1)
print("tails: every node rang out to exact zero and rested empty")
