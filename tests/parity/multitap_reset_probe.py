"""MultiTapDelay: a reset puts the write head back at the top.

    multitap_reset_probe.py audiodelays

A node played for a while, reset with `audiocore.reset_buffer`, and then
given a shorter Time, against a fresh node given the same source and the
same move. Up to v0.6.6rc3 the reset kept the write head where the pulls had
taken it, so the move kept another stretch of the line and the two parted;
now they render the same bytes. Each case prints every block after the move
and whether the two nodes agree on it.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiodelays"
delays = __import__(MODULE)


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def material(rate, channels, frames):
    values = array("h")
    state = 12345
    for _frame in range(frames):
        for _channel in range(channels):
            state = (state * 1103515245 + 12345) & 0x7fffffff
            values.append(((state >> 8) % 20001) - 10000)
    return audiocore.RawSample(values, sample_rate=rate,
                               channel_count=channels)


def node(rate, channels):
    # Every level is exact in binary. A tap level of 0.7 is 0.699999988 on a
    # single-precision build, so a repeat whose product truncates to a whole
    # number there lands one step lower than on CPython, and the two parted
    # at the first block the taps reached, with reset and fresh agreeing on
    # both. This probe is about the reset, not about that rounding.
    return delays.MultiTapDelay(
        max_delay_ms=400, delay_ms=300, decay=0.0, mix=1.0,
        taps=((1.0, 1.0), (0.5, 0.75)), buffer_size=512 * channels,
        sample_rate=rate, channel_count=channels)


def run(tag, rate, channels, played):
    frames = rate // 2
    used = node(rate, channels)
    used.play(material(rate, channels, frames), loop=True)
    for _ in range(played):
        audiocore.get_buffer(used)
    audiocore.reset_buffer(used)
    used.play(material(rate, channels, frames), loop=True)
    fresh = node(rate, channels)
    fresh.play(material(rate, channels, frames), loop=True)
    for block in range(24):
        if block == 3:
            used.delay_ms = 20.0
            fresh.delay_ms = 20.0
        a = bytes(audiocore.get_buffer(used)[1])
        b = bytes(audiocore.get_buffer(fresh)[1])
        print("mtr", tag, block, len(a), checksum(a), a == b)
    used.deinit()
    fresh.deinit()


for rate in (22050, 48000):
    for channels in (1, 2):
        for played in (9, 37):
            run("%d-%dch-%d" % (rate, channels, played), rate, channels,
                played)
print("done multitap-reset")
