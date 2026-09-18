"""Deterministic Dynamics PCM for the two options the original never had.

    dynamics_extras_probe.py audiodynamics

`dynamics_probe.py` is held against `vstaudio_dsp.c` compiled unmodified, so
it may only use forms that original accepts. Lookahead and true-peak detection
are audioif's additions and have no oracle, so they get their own fixture,
captured from the port and holding the three interpreters to each other.

Everything here also proves the halves stay separable: the first case sets
neither option, and its numbers have to match what `dynamics_probe.py` gets
for the same settings.
"""

import sys
from array import array

import audiocore
from audioif_util import float32_bits

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiodynamics"
# A built-in module under MicroPython, which does not record those in
# sys.modules - take what __import__ hands back.
dynamics = __import__(MODULE)

SAMPLE_RATE = 48000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def transient(frames=2400):
    """Silence, then a hard square at full scale. Nothing exercises lookahead
    like an edge the detector would otherwise meet late."""
    values = array("h")
    for frame in range(frames):
        loud = 400 <= frame < 1600
        value = 0 if not loud else (32000 if frame % 2 else -32000)
        values.append(value)
        values.append(value)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=2)


def between_samples(frames=2400):
    """A quarter-rate sine offset by 45 degrees: every sample sits at -3 dBFS
    and every actual peak, halfway between two of them, at 0."""
    values = array("h")
    for frame in range(frames):
        # 45, 135, 225, 315 degrees, exactly, so no trigonometry is involved
        # and two interpreters cannot round it differently.
        value = 23170 if frame % 4 < 2 else -23170
        values.append(value)
        values.append(value)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=2)


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        # The gain reduction prints as its float32 bit pattern, not as
        # `"%.6f"`: the three targets hold the identical float32 and the
        # PCM beside it is byte-for-byte the same, but MicroPython's
        # single-precision formatter is not correctly rounded to seven
        # significant digits. audioif#80.
        print("dynx", tag, index, len(data), sum(data), checksum(data),
              float32_bits(node.gain_reduction_db()))


BASE = {"threshold_db": -12.0, "attack_ms": 0.05, "release_ms": 60.0,
        "sample_rate": SAMPLE_RATE}

CASES = (
    ("off", dict(BASE)),
    ("look-1", dict(BASE, lookahead_ms=1.0)),
    ("look-5", dict(BASE, lookahead_ms=5.0)),
    # Above the 50 ms cap, which has to clamp rather than allocate.
    ("look-clamped", dict(BASE, lookahead_ms=500.0)),
    ("truepeak", dict(BASE, true_peak=1)),
    ("both", dict(BASE, lookahead_ms=3.0, true_peak=1)),
)

for name, options in CASES:
    node = dynamics.Dynamics(dynamics.DYN_LIMIT, **options)
    node.play(between_samples() if "peak" in name else transient())
    emit(name, node, 6)

# Turned on mid-stream, which is when the buffer is allocated: the detector
# keeps its envelope, and the audio already in flight is not.
node = dynamics.Dynamics(dynamics.DYN_LIMIT, **BASE)
node.play(transient())
emit("late-a", node, 2)
node.set(lookahead_ms=4.0, true_peak=1)
emit("late-b", node, 4)

# Compression rather than limiting, so the gain computer's other branch sees
# the same detector.
node = dynamics.Dynamics(dynamics.DYN_COMPRESS, threshold_db=-24.0,
                         ratio=8.0, knee_db=6.0, attack_ms=2.0,
                         release_ms=90.0, lookahead_ms=2.5, true_peak=1,
                         sample_rate=SAMPLE_RATE)
node.play(transient())
emit("compress", node, 6)
