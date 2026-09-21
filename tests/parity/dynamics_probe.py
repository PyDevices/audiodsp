"""Deterministic Dynamics PCM, from whichever module provides the node.

    dynamics_probe.py audiodynamics

Every interpreter renders this and the outputs must be identical, so any
difference in the printed stats is a difference in the DSP - a width, a shift,
a compiler's choice. See docs/correctness-standard.md.

**A legacy worth knowing about.** This file uses only the argument forms
micropython-vst3's `vstaudio_dsp.c` accepted, because it was written to be run
against that engine compiled unmodified. That oracle is retired - the engine is
a consumer of this package now, not a grader of it - so the restriction no
longer has a reason. It is left in place because widening it is a change to
what this fixture covers rather than a tidy-up, and the port's own additions are
already covered by `dynamics_extras_probe.py`, `dynamics_options_probe.py` and
`tests/test_cpython_audiodynamics.py`.
"""

import sys
from array import array

import audiocore
from audiodsp_util import float32_bits

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiodynamics"
# Both spellings are built-in modules under MicroPython, which does not record
# those in sys.modules - take what __import__ hands back.
dynamics = __import__(MODULE)

SAMPLE_RATE = 48000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def source(frames=1200, quiet=700, loud=12000):
    """A stereo burst pattern: 40 loud frames, then 60 quiet ones.

    Both halves of every detector - attack and release, fast and slow - move
    within a single 256-frame block, which is what makes one short capture
    enough to pin the gain computer down.
    """
    values = array("h")
    for frame in range(frames):
        level = loud if (frame % 100) < 40 else quiet
        for channel in range(2):
            shape = ((frame * (97 + channel * 18)) % 2001) - 1000
            values.append(shape * level // 1000)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=2)


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        # The gain reduction prints as its float32 bit pattern, not as
        # `"%.6f"`. The three targets hold the identical float32 here -- the
        # PCM beside it is byte-for-byte the same on all of them -- but six
        # decimals is seven significant digits, and MicroPython's
        # single-precision formatter is not correctly rounded that far, so
        # that column used to disagree on a float build while the audio did
        # not. audiodsp#80; audiodsp_util.float32_bits says why.
        print("dyn", tag, index, len(data), sum(data), checksum(data),
              float32_bits(node.gain_reduction_db()))


# Every mode, at settings that actually engage it.
CASES = (
    ("compress", 0, {"threshold_db": -30.0, "ratio": 6.0, "knee_db": 9.0,
                     "makeup_db": 4.0, "attack_ms": 5.0, "release_ms": 80.0}),
    ("compress-hard", 0, {"threshold_db": -18.0, "ratio": 20.0,
                          "knee_db": 0.0}),
    ("limit", 1, {"threshold_db": -26.0, "attack_ms": 0.5,
                  "release_ms": 40.0}),
    ("expand", 2, {"threshold_db": -20.0, "ratio": 3.0}),
    ("gate", 3, {"threshold_db": -22.0, "attack_ms": 1.0,
                 "release_ms": 150.0}),
    ("transient", 4, {"attack_gain_db": 9.0, "sustain_gain_db": -7.0}),
    ("transient-soft", 4, {"attack_gain_db": -4.0, "sustain_gain_db": 3.0}),
    ("sidechain", 0, {"threshold_db": -28.0, "ratio": 8.0,
                      "sidechain_hz": 2500.0}),
    ("defaults", 0, {}),
)

for name, mode, options in CASES:
    node = dynamics.Dynamics(mode, sample_rate=SAMPLE_RATE, **options)
    node.play(source())
    emit(name, node, 5)

# Zero and negative option values take their own branches: ratio and knee are
# clamped, attack_ms <= 0 becomes an instant follower, sidechain_hz <= 0 turns
# the detector filter off entirely.
node = dynamics.Dynamics(0, sample_rate=SAMPLE_RATE, ratio=0.25, knee_db=-3.0,
                         attack_ms=0.0, release_ms=-5.0, sidechain_hz=-1.0)
node.play(source())
emit("clamped", node, 4)

# set() mid-stream: the detector keeps its memory, the gain computer changes
# underneath it.
node = dynamics.Dynamics(0, sample_rate=SAMPLE_RATE, threshold_db=-30.0,
                         ratio=4.0)
node.play(source())
emit("set-before", node, 2)
node.set(threshold_db=-45.0, ratio=12.0, makeup_db=6.0)
emit("set-after", node, 3)
node.set(sidechain_hz=800.0)
emit("set-sidechain", node, 2)

# reset_buffer drops the envelopes but keeps the sidechain filter's memory and
# the last reported gain reduction. Rendering the same source either side of it
# is what makes that visible.
node = dynamics.Dynamics(0, sample_rate=SAMPLE_RATE, threshold_db=-35.0,
                         ratio=10.0, sidechain_hz=1200.0)
node.play(source())
emit("reset-before", node, 3)
audiocore.reset_buffer(node)
print("dyn reset-gr", float32_bits(node.gain_reduction_db()))
node.play(source())
emit("reset-after", node, 3)

# A source shorter than one block, so the pending frames carry across output
# blocks and then run out. The node has to keep answering with silence rather
# than reporting itself finished.
node = dynamics.Dynamics(1, sample_rate=SAMPLE_RATE, threshold_db=-30.0)
node.play(source(frames=300))
emit("short", node, 4)

# Never fed at all: the same silence, from the other direction.
node = dynamics.Dynamics(0, sample_rate=SAMPLE_RATE)
emit("unplayed", node, 2)

# Sample rate is what the millisecond options are read against, so the same
# times at a different rate must give different coefficients.
for rate in (22050, 96000):
    node = dynamics.Dynamics(0, sample_rate=rate, threshold_db=-30.0,
                             ratio=5.0, attack_ms=3.0, release_ms=60.0)
    node.play(source())
    emit("rate-%d" % rate, node, 3)

print("done dynamics")
