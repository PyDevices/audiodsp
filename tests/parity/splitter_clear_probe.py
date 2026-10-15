"""`Splitter.clear()`: what the ring held and nobody read is let go.

A Splitter writes a whole source buffer into its ring and hands it out a
256-frame chunk at a time, so a source with longer buffers leaves frames in
the ring between pulls. A graph that stops being pulled (an effect's bypass,
while the host reads the source directly) came back on those frames: audio
from before the pause, and the stream late by as much. `clear()` moves every
tap to the write position and drops the remainder it could not fit, without
reading the source.

    CASE  <name>  <step>  <first frame index>  <frames>  <checksum>

The material carries each frame's index, so the first field after the step
says where in the source a tap is. `before` is a pull ahead of the clear,
`after` one behind it.
"""

from array import array

import audiocore
import audiofilters
import audioroute

RATE = 48000
TABLE = 3000


def checksum(values):
    value = 2166136261
    for word in values:
        value = ((value ^ (word & 0xffff)) * 16777619) & 0xffffffff
    return value


def ramp(channels):
    values = array("h")
    for frame in range(TABLE):
        values.extend([frame] * channels)
    return values


def feed(kind, channels):
    sample = audiocore.RawSample(ramp(channels), sample_rate=RATE,
                                 channel_count=channels)
    block = audiofilters.Filter(filter=None, mix=1.0,
                                buffer_size=kind * channels * 2,
                                sample_rate=RATE, channel_count=channels)
    block.play(sample, loop=True)
    return block


def report(name, step, channels, data):
    words = array("h", bytes(data))
    print("CASE", name, step, words[0] if words else -1,
          len(words) // channels, checksum(words))


def run(kind, channels):
    name = "%s-%dch" % (kind, channels)
    splitter = audioroute.Splitter(feed(kind, channels), taps=2)
    taps = (splitter.tap(0), splitter.tap(1))
    for step in range(2):
        for index in (0, 1):
            report(name, "before%d.%d" % (step, index), channels,
                   audiocore.get_buffer(taps[index])[1])
    splitter.clear()
    for step in range(4):
        for index in (0, 1):
            report(name, "after%d.%d" % (step, index), channels,
                   audiocore.get_buffer(taps[index])[1])


for channels in (1, 2):
    for kind in (1000, 100, 512, 256):
        run(kind, channels)
