"""Deterministic FeedbackDelay PCM.

    feedback_delay_probe.py audioecho

Like multiply_probe.py, this has no oracle: `audioecho` is audiodsp's own
module, with no ancestor in CircuitPython or in micropython-vst3's engine.
What the golden pins is that every interpreter renders it identically.

That matters more here than anywhere else in the suite. The loop is
recursive and runs in `float`, so a difference of one ulp anywhere in it
does not stay one ulp -- it is fed back in and amplified for as long as the
feedback holds. If the three builds are going to disagree about floating
point at all, this is where it will show.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audioecho"
# A built-in module under MicroPython, which does not record those in
# sys.modules - take what __import__ hands back.
echo = __import__(MODULE)

SAMPLE_RATE = 8000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def source(frames=1600, level=14000):
    """A burst and then a long tail, which is what a delay has to be driven
    with: the repeats have to arrive somewhere the input is not."""
    values = array("h")
    for frame in range(frames):
        for channel in range(2):
            if frame < 120:
                shape = ((frame * (97 + channel * 18)) % 2001) - 1000
                values.append(shape * level // 1000)
            else:
                values.append(0)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=2)


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        print("fbd", tag, index, len(data), sum(data), checksum(data))


# A plain loop, then each thing in it turned on one at a time, so a failure
# says which part of the path moved.
CASES = (
    ("plain", {"delay_ms": 40.0, "feedback": 0.7, "mix": 1.0}),
    ("damped", {"delay_ms": 40.0, "feedback": 0.7, "mix": 1.0,
                "damping_hz": 900.0}),
    ("cut", {"delay_ms": 40.0, "feedback": 0.7, "mix": 1.0,
             "cut_hz": 220.0}),
    ("driven", {"delay_ms": 40.0, "feedback": 0.85, "mix": 1.0,
                "loop_drive": 0.9}),
    ("wow", {"delay_ms": 40.0, "feedback": 0.7, "mix": 1.0, "wow_hz": 5.0,
             "wow_depth_ms": 3.0}),
    ("crossed", {"delay_ms": 40.0, "feedback": 0.8, "mix": 1.0,
                 "cross_feed": 1.0, "input_pan": -1.0}),
    ("blend", {"delay_ms": 33.0, "feedback": 0.5, "mix": 0.37}),
    ("everything", {"delay_ms": 47.0, "feedback": 0.82, "mix": 0.64,
                    "damping_hz": 1400.0, "cut_hz": 140.0, "wow_hz": 3.0,
                    "wow_depth_ms": 1.5, "cross_feed": 0.6,
                    "loop_drive": 0.5, "input_pan": -0.4}),
)

for name, options in CASES:
    node = echo.FeedbackDelay(sample_rate=SAMPLE_RATE, max_delay_ms=120.0,
                              **options)
    node.play(source())
    emit(name, node, 8)

# Set mid-stream: the line keeps its contents and only what the loop does to
# them changes, which is a different fixture from building it that way.
node = echo.FeedbackDelay(sample_rate=SAMPLE_RATE, max_delay_ms=120.0,
                          delay_ms=40.0, feedback=0.75, mix=1.0)
node.play(source())
emit("late-a", node, 3)
node.set(damping_hz=700.0, delay_ms=25.0)
emit("late-b", node, 3)
node.clear()
emit("late-c", node, 3)

# Clamping: a delay longer than the line, and a feedback over unity.
node = echo.FeedbackDelay(sample_rate=SAMPLE_RATE, max_delay_ms=40.0,
                          delay_ms=5000.0, feedback=4.0, mix=1.0)
node.play(source())
emit("clamped", node, 4)

# No source: silence, never a short block, never finished.
node = echo.FeedbackDelay(sample_rate=SAMPLE_RATE, max_delay_ms=40.0)
emit("starved", node, 2)


def mono_source(frames=100, level=14000):
    """Mono, and **shorter than one DSP chunk** (AUDIODSP_FEEDBACK_DELAY_FRAMES
    is 256), so `get_buffer`'s inner loop runs more than once and `produced`
    is non-zero on a later pass.

    Both conditions are load-bearing. Every case above is stereo and every
    one of them hands out 1600 frames, and on that material the MicroPython
    binding advanced its destination by `produced * 2` while the DSP wrote
    `channel_count` samples per frame - correct on stereo by coincidence, and
    correct on mono only while one chunk covers the whole pull. So the bug in
    audiodsp#54 rendered different bytes on MicroPython from the CPython
    extension and the patched CircuitPython build, and no fixture in this
    file could see it: measured before the fix, this case summed 176601 on
    MicroPython against 206555 on the CPython target.
    """
    values = array("h")
    for frame in range(frames):
        shape = ((frame * 97) % 2001) - 1000
        values.append(shape * level // 1000)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=1)


node = echo.FeedbackDelay(sample_rate=SAMPLE_RATE, max_delay_ms=10.0,
                          delay_ms=6.0, feedback=0.7, mix=1.0,
                          channel_count=1)
node.play(mono_source())
emit("mono-short", node, 4)
