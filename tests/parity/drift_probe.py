"""DriftResampler following a producer on another clock.

    drift_probe.py audiopump

A Ring is written on a schedule that runs fast or slow against the pulls, by
an integer accumulator, so every target sees the same writes. The node starts
at its target, steers to the producer's rate, rides out a stall (an
underrun) and a moved target, and converts 44.1 kHz up to 48 kHz. Each case
prints a checksum of everything played, the underrun counts and the ratio's
offset rounded to a whole ppm. The material is integer and the kernel is
shared C, so MicroPython, CircuitPython and CPython print the same lines.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiopump"
audiopump = __import__(MODULE)


def checksum(value, data):
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


class Source:
    """An integer saw and its octave, `channels` wide, read in chunks."""

    def __init__(self, channels, period=97):
        self.channels = channels
        self.period = period
        self.at = 0

    def take(self, frames):
        out = array("h", [0] * (frames * self.channels))
        for i in range(frames):
            step = (self.at + i) % self.period
            saw = step * 20000 // (self.period - 1) - 10000
            fast = (2 * step) % self.period * 8000 // (self.period - 1) - 4000
            out[i * self.channels] = saw + fast
            if self.channels == 2:
                out[i * self.channels + 1] = saw - fast
        self.at += frames
        return out


def run(name, channels, in_rate, out_rate, frames, capacity, pulls,
        ppm, chunk, stall=None, retarget=None, **options):
    """`ppm` fast (positive) or slow against the pulls, in `chunk`-frame
    writes. `stall` is a range of pulls with no writes; `retarget` a pull
    and the target to set there."""
    ring = audiopump.Ring(sample_rate=in_rate, channel_count=channels,
                          frames=frames, capacity=capacity)
    node = audiopump.DriftResampler(ring, sample_rate=out_rate, **options)
    source = Source(channels)
    # Input frames owed per pull, times out_rate * 10^6, so the accumulator
    # stays an integer at any ppm.
    scale = out_rate * 1000000
    per_pull = frames * in_rate * (1000000 + ppm)
    owed = 0
    digest = 2166136261
    for pull in range(pulls):
        if stall is None or not (stall[0] <= pull < stall[1]):
            owed += per_pull
            while owed >= chunk * scale:
                owed -= chunk * scale
                ring.write(source.take(chunk))
        if retarget is not None and pull == retarget[0]:
            node.target = retarget[1]
        digest = checksum(digest, bytes(audiocore.get_buffer(node)[1]))
        if pull % 750 == 749:
            print(name, pull + 1, "fill", int(node.fill + 0.5),
                  "ppm", int(node.ppm + (0.5 if node.ppm >= 0 else -0.5)))
    stats = ring.stats()
    print(name, "digest", digest, "underruns", node.underruns, stats[3],
          "overruns", stats[4], "target", node.target)


# The bandwidth is 1/16, exact in binary: a decimal one would rest on each
# interpreter's float parser, and CircuitPython's parses to single precision.
run("stereo_fast", 2, 48000, 48000, 64, 16, 3000, 3000, 48,
    max_ppm=5000, bandwidth=0.0625)
run("stereo_slow", 2, 48000, 48000, 64, 16, 3000, -3000, 48,
    max_ppm=5000, bandwidth=0.0625)
run("mono_stall", 1, 48000, 48000, 32, 32, 6000, 1000, 40,
    stall=(3000, 3060), max_ppm=2000, bandwidth=0.0625)
run("retarget", 2, 48000, 48000, 64, 16, 3000, 0, 64,
    retarget=(1500, 300), max_ppm=10000, bandwidth=0.0625)
run("up_44k1", 2, 44100, 48000, 64, 16, 3000, 500, 49,
    max_ppm=2000, bandwidth=0.0625)
