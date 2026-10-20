"""Ladder at the caller's block size.

    ladder_frames_probe.py audioladder

`buffer_size` changes how many frames a pull hands out and nothing about the
filter, so a render at 64 frames, or at one, is the default render cut into
smaller pieces. This prints the checksum of each whole render beside whether
it matches the default's, and the block length each one handed out. Its own
file, so ladder_probe.py keeps rendering what it rendered.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audioladder"
ladder = __import__(MODULE)

RATE = 48000
FRAMES = 1024


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def buzz(channels, period=37, level=12000):
    values = array("h", [0] * (FRAMES * channels))
    for frame in range(FRAMES):
        step = frame % period
        shape = step * 2 * level // (period - 1) - level
        for channel in range(channels):
            values[frame * channels + channel] = \
                shape if channel == 0 else -shape
    return values


def render(channels, block, source_block):
    """`block` frames a pull, from a source handing out `source_block`."""
    material = buzz(channels)
    options = {}
    if block is not None:
        options["buffer_size"] = block * 2 * channels
    node = ladder.Ladder(sample_rate=RATE, channel_count=channels,
                         cutoff_hz=900.0, resonance=3.7, drive=2.0, **options)
    # One looping RawSample of `source_block` frames, refilled before each of
    # its pulls: the way a host feeds a node one block at a time.
    window = array("h", [0] * (source_block * channels))
    source = audiocore.RawSample(window, sample_rate=RATE,
                                 channel_count=channels)
    node.play(source)
    out = bytearray()
    lengths = []
    position = 0
    while len(out) < FRAMES * 2 * channels:
        chunk = material[position * channels:
                         (position + source_block) * channels]
        for index in range(len(window)):
            window[index] = chunk[index] if index < len(chunk) else 0
        position += source_block
        data = bytes(audiocore.get_buffer(node)[1])
        lengths.append(len(data))
        out += data
    return bytes(out[:FRAMES * 2 * channels]), lengths


for channels in (1, 2):
    reference, _ = render(channels, None, 256)
    print("ladder-frames", channels, "default", checksum(reference))
    for block in (64, 1, 200):
        data, lengths = render(channels, block, block)
        print("ladder-frames", channels, block, checksum(data),
              data == reference, min(lengths), max(lengths))
