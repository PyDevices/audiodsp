"""Deterministic GranularPitchShift PCM.

    granular_pitch_shift_probe.py audiodelays

CircuitPython 10.3.0 added the node. This fixture holds the CPython twin to
the same bytes the native object renders, including the xorshift32 jitter.
"""

import sys
from array import array

import audiocore
from audiodsp_util import float32

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiodelays"
delays = __import__(MODULE)

SAMPLE_RATE = 8000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def source(frames=1600, level=14000):
    values = array("h")
    for frame in range(frames):
        for channel in range(2):
            shape = ((frame * (97 + channel * 18)) % 2001) - 1000
            values.append(shape * level // 1000)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=2)


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        print("gps", tag, index, len(data), sum(data), checksum(data))


CASES = (
    ("unity", {"semitones": 0, "mix": 1.0, "grain_size": 64, "density": 2,
               "spread": 0.0}),
    ("up", {"semitones": 5, "mix": 1.0, "grain_size": 64, "density": 2,
            "spread": 0.0}),
    ("down", {"semitones": -7, "mix": 1.0, "grain_size": 48, "density": 3,
              "spread": 0.0}),
    # `float32(0.35)` rather than 0.35: the node keeps `mix` as an
    # `mp_float_t`, so a plain literal is one number on a double build and
    # another on a single-precision one, and the blend renders different
    # bytes before the kernel is reached at all. That was one of the six
    # divergences in audiodsp#80; `spread` below goes through it for the same
    # reason even though it happened not to move. 0.0, 0.5 and 1.0 are exact
    # in both widths and need no such call.
    ("blend", {"semitones": 3, "mix": float32(0.35), "grain_size": 64,
               "density": 2, "spread": 0.0}),
    ("cloud", {"semitones": 2, "mix": 1.0, "grain_size": 80, "density": 4,
               "spread": float32(0.6)}),
)

for name, options in CASES:
    node = delays.GranularPitchShift(
        sample_rate=SAMPLE_RATE, channel_count=2, buffer_size=512, **options)
    node.play(source())
    emit(name, node, 6)

node = delays.GranularPitchShift(
    sample_rate=SAMPLE_RATE, channel_count=2, buffer_size=512,
    semitones=0, mix=1.0, grain_size=64, density=2, spread=0.0)
node.play(source())
emit("late-a", node, 2)
node.semitones = 4
node.mix = 0.5
emit("late-b", node, 2)

node = delays.GranularPitchShift(
    sample_rate=SAMPLE_RATE, channel_count=2, buffer_size=512, grain_size=64)
emit("starved", node, 2)
