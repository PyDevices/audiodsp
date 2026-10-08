"""MultiTapDelay: a shorter Time and back again reads silence, not old audio.

    multitap_time_probe.py audiodelays

Every write of `delay_ms` clears the line past the new length on the native
node, MicroPython's and CircuitPython's alike. The CPython twin did not, so a
shorter Time and then the old one played back repeats the native node had
already cleared: in one case 508 frames after the move back, CPython played a
peak of 2891 where both native builds played 0 (audiodsp#177). The cases move
the Time down and back with the repeats still in the line, at two rates and
both channel counts, and print every block.
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


def source(rate, channels, frames):
    """A burst at the top, then silence, so whatever plays after the burst
    is the line's."""
    values = array("h")
    for frame in range(frames):
        for channel in range(channels):
            if frame < 400:
                shape = ((frame * (89 + channel * 23)) % 2001) - 1000
                values.append(shape * 12)
            else:
                values.append(0)
    return audiocore.RawSample(values, sample_rate=rate,
                               channel_count=channels)


def peak(data):
    samples = array("h", data)
    return max(abs(sample) for sample in samples) if samples else 0


def run(tag, rate, channels):
    node = delays.MultiTapDelay(
        max_delay_ms=400, delay_ms=300, decay=0.0, mix=1.0,
        taps=((1.0, 1.0),), buffer_size=512, sample_rate=rate,
        channel_count=channels)
    node.play(source(rate, channels, rate * 2))
    block = 0

    def pull(count):
        nonlocal block
        for _ in range(count):
            data = bytes(audiocore.get_buffer(node)[1])
            print("mtt", tag, block, len(data), peak(data), checksum(data))
            block += 1

    pull(4)                  # the burst goes in at 300 ms
    node.delay_ms = 40       # shorter: the line past 40 ms is cleared
    pull(3)
    node.delay_ms = 300      # back: what was there must be silence now
    pull(30)
    node.deinit()


for rate in (22050, 48000):
    for channels in (1, 2):
        run("%d-%dch" % (rate, channels), rate, channels)
print("done multitap-time")
