"""Deterministic Ladder PCM.

    ladder_probe.py audioladder

Like multiply_probe.py and feedback_delay_probe.py, this has no oracle:
`audioladder` is audiodsp's own module, with no ancestor in CircuitPython or
in micropython-vst3's engine. What the golden pins is that every interpreter
renders it identically.

It belongs in the same sensitive class as feedback_delay_probe.py, and for a
sharper reason. That loop is recursive and runs in `float`; this one is
recursive, runs in `float`, and is *solved* rather than stepped -- a fixed
four passes of a contraction seeded by extrapolating the last two samples.
A one-ulp disagreement between two builds therefore has two ways to grow:
round the feedback loop, as it does in the delay, and through the solver's
seed. Several of the fixtures below sit at or past the feedback where the
loop sustains a tone of its own, which is the state in which nothing damps a
difference at all.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audioladder"
# A built-in module under MicroPython, which does not record those in
# sys.modules - take what __import__ hands back.
ladder = __import__(MODULE)

SAMPLE_RATE = 8000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def impulse(frames=1600, level=30000, channels=2):
    """One sample and then silence, which is how a resonator is asked whether
    it rings, decays or sustains."""
    values = array("h", [0] * (frames * channels))
    for channel in range(channels):
        values[channel] = level
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def buzz(frames=1600, level=12000, period=37, channels=2):
    """A sawtooth with an integer period: harmonics everywhere for the filter
    to remove, and a shape computed without touching libm, so the material is
    the same bytes on every interpreter before the filter sees it."""
    values = array("h", [0] * (frames * channels))
    for frame in range(frames):
        step = frame % period
        shape = step * 2 * level // (period - 1) - level
        for channel in range(channels):
            values[frame * channels + channel] = \
                shape if channel == 0 else -shape
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        print("ladder", tag, index, len(data), sum(data), checksum(data))


# The resonance sweep, on an impulse, is the fixture that matters most: below
# 4 the ring decays, at 4 it sustains, and past 4 it grows into the saturator
# and stops there. A build that got the loop wrong shows up here first.
for name, resonance in (("quiet", 0.0), ("mid", 2.0), ("hot", 3.9),
                        ("edge", 4.0), ("over", 4.2)):
    node = ladder.Ladder(sample_rate=SAMPLE_RATE, cutoff_hz=400.0,
                         resonance=resonance)
    node.play(impulse())
    emit(name, node, 6)

# Everything else, one at a time, so a failure says which part moved.
CASES = (
    ("wire", {"mix": 0.0, "cutoff_hz": 300.0, "resonance": 4.0,
              "drive": 8.0}),
    ("blend", {"mix": 0.37, "cutoff_hz": 600.0, "resonance": 3.0}),
    ("driven", {"cutoff_hz": 700.0, "resonance": 3.5, "drive": 2.5}),
    ("slammed", {"cutoff_hz": 700.0, "resonance": 3.5, "drive": 16.0}),
    ("one-pole", {"cutoff_hz": 500.0, "resonance": 3.8, "poles": 1}),
    ("two-pole", {"cutoff_hz": 500.0, "resonance": 3.8, "poles": 2}),
    ("three-pole", {"cutoff_hz": 500.0, "resonance": 3.8, "poles": 3}),
    ("four-pole", {"cutoff_hz": 500.0, "resonance": 3.8, "poles": 4}),
    ("plain-rate", {"cutoff_hz": 900.0, "resonance": 3.9, "drive": 6.0,
                    "oversample": 1}),
    ("doubled", {"cutoff_hz": 900.0, "resonance": 3.9, "drive": 6.0,
                 "oversample": 2}),
    ("compensated", {"cutoff_hz": 450.0, "resonance": 3.9,
                     "passband_comp": 1.0}),
    ("low", {"cutoff_hz": 30.0, "resonance": 3.9, "drive": 4.0}),
    ("open", {"cutoff_hz": 3600.0, "resonance": 3.9, "drive": 4.0}),
    ("everything", {"cutoff_hz": 520.0, "resonance": 4.1, "drive": 3.0,
                    "poles": 3, "passband_comp": 0.5, "oversample": 2,
                    "mix": 0.8}),
)

for name, options in CASES:
    node = ladder.Ladder(sample_rate=SAMPLE_RATE, **options)
    node.play(buzz())
    emit(name, node, 6)

# Mono: one lane, half the frames per block, and the second channel's state
# never touched.
node = ladder.Ladder(sample_rate=SAMPLE_RATE, channel_count=1,
                     cutoff_hz=650.0, resonance=3.9, drive=2.0)
node.play(buzz(channels=1))
emit("mono", node, 4)

# Set mid-stream: the integrators keep their charge and only what the loop
# does to them changes, which is a different fixture from building it that
# way. This is also the call a macro rides on, once a block.
node = ladder.Ladder(sample_rate=SAMPLE_RATE, cutoff_hz=800.0,
                     resonance=3.6, drive=2.0)
node.play(buzz())
emit("late-a", node, 3)
node.set(cutoff_hz=250.0, resonance=4.1)
emit("late-b", node, 3)
node.clear()
emit("late-c", node, 3)

# Clamping: every option pushed past its end at once.
node = ladder.Ladder(sample_rate=SAMPLE_RATE, cutoff_hz=100000.0,
                     resonance=9.0, drive=1000.0, poles=9,
                     passband_comp=4.0, oversample=5, mix=5.0)
node.play(buzz())
emit("clamped", node, 4)

# No source: silence, never a short block, never finished.
node = ladder.Ladder(sample_rate=SAMPLE_RATE, cutoff_hz=400.0,
                     resonance=4.2)
emit("starved", node, 2)
