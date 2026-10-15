"""A Dynamics node whose source hands out buffers shorter than its block.

The node fills its 256-frame block from as many source buffers as it takes.
The native builds wrote each stretch at `produced * 2` samples into the
block, the stereo stride, whatever the channel count. In mono every stretch
after the first landed twice as far in as it belonged, and the frames
between were whatever an earlier block had left there. So a mono Dynamics
on 100-frame source buffers handed out frames 100-199 of each block stale,
and once its source ended part-way through a block it replayed one stale
frame at the same place in every block for good: a buzz at the block rate
(audiocomponents' mono Compressor). The CPython twin
wrote at the right place, and every other dynamics probe feeds whole
256-frame buffers, which is why none saw it.

    CASE  <name>  <block>  <first non-zero sample index or -1>  <sum>  <checksum>

`chain` is the Compressor's mono shape: a Splitter tap into two Dynamics in
series. `end` points the source at an empty sample part-way through block 4,
so every block from 5 on must be exact silence.
"""

from array import array

import audiocore
import audiodynamics
import audiofilters
import audioroute

RATE = 48000


def checksum(values):
    value = 2166136261
    for word in values:
        value = ((value ^ (word & 0xffff)) * 16777619) & 0xffffffff
    return value


def material(frames, channels):
    values = array("h")
    for frame in range(frames):
        for channel in range(channels):
            values.append(((frame * (53 + channel * 11)) % 1201) * 23 - 13800)
    return values


def reblock(channels, frames):
    """A looped sample handed out `frames` at a time."""
    block = audiofilters.Filter(filter=None, mix=1.0,
                                buffer_size=frames * channels * 2,
                                sample_rate=RATE, channel_count=channels)
    block.play(audiocore.RawSample(material(2400, channels), sample_rate=RATE,
                                   channel_count=channels), loop=True)
    return block


def dynamics(channels):
    return audiodynamics.Dynamics(audiodynamics.DYN_COMPRESS,
                                  sample_rate=RATE, channel_count=channels,
                                  threshold_db=-12.0, ratio=4.0)


def report(name, block, data):
    words = array("h", bytes(data))
    first = -1
    for index in range(len(words)):
        if words[index]:
            first = index
            break
    print("CASE", name, block, first, sum(words), checksum(words))


def run(shape, channels, frames, blocks=10, end_at=None):
    port = audioroute.Port(reblock(channels, frames))
    empty = audiocore.RawSample(memoryview(array("h", [0, 0]))[0:0],
                                sample_rate=RATE, channel_count=channels)
    if shape == "chain":
        splitter = audioroute.Splitter(port, taps=1)
        first = dynamics(channels)
        first.play(splitter.tap(0))
        node = dynamics(channels)
        node.play(first)
    else:
        node = dynamics(channels)
        node.play(port)
    name = "%s-%dch-%d%s" % (shape, channels, frames,
                             "" if end_at is None else "-end")
    for block in range(blocks):
        if block == end_at:
            port.play(empty)
        report(name, block, audiocore.get_buffer(node)[1])


for channels in (1, 2):
    for frames in (100, 37, 3, 256, 1000):
        run("plain", channels, frames)
    for frames in (100, 37):
        run("chain", channels, frames)
        run("chain", channels, frames, end_at=4)
