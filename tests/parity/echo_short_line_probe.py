"""Deterministic Echo PCM for a delay line shorter than one audio buffer.

    echo_short_line_probe.py audiodelays

With freq_shift=False, Echo raises a delay shorter than its audio buffer to
the buffer's length. CircuitPython 10.3.0 did that after clamping to the
allocation rather than before, so a max_delay_ms shorter than one buffer gave
a line longer than the memory behind it, and the unix port segfaulted.
Upstream clamps last since 6dddbda87 (11.0.0-alpha.1), as this port and the
twin do, so the probe runs on all three interpreters.
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


def source(frames, channels):
    values = array("h")
    for frame in range(frames):
        for channel in range(channels):
            values.append((((frame * (131 + channel * 7)) % 2001) - 1000) * 9)
    return audiocore.RawSample(values, sample_rate=8000, channel_count=channels)


# (tag, channels, max_delay_ms, delay_ms, buffer frames)
CASES = (
    ("mono", 1, 20, 10, 256),
    ("stereo", 2, 20, 10, 256),
    ("tiny", 1, 1, 1, 512),
    ("edge", 2, 64, 30, 256),
)

for tag, channels, max_delay_ms, delay_ms, frames in CASES:
    echo = delays.Echo(
        max_delay_ms=max_delay_ms, delay_ms=delay_ms, decay=0.5, mix=0.5,
        freq_shift=False, sample_rate=8000, channel_count=channels,
        buffer_size=frames * channels * 2)
    echo.play(source(700, channels), loop=True)
    for index in range(12):
        data = bytes(audiocore.get_buffer(echo)[1])
        print("esl", tag, index, len(data), sum(data), checksum(data))
