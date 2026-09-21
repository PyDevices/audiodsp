"""Deterministic FeedbackDelay PCM for the four options the node grew later.

    feedback_delay_options_probe.py audioecho

`feedback_delay_probe.py` covers the node as it was: a sine on the delay, a
`delay_ms` that lands in one jump, no level modulation and a line read at
unity. `wow_shape`, `delay_slew`, `wow_am_depth` and `loop_semitones` are
additions, and they get their own fixture rather than joining that one -- the
same split `dynamics_extras_probe.py` makes, and for the same reason. That
file's hash is one hash over its whole output, so a case appended to it would
move the number that says the addition changed nothing.

Neither has an oracle: `audioecho` is audiodsp's own, with no ancestor in
CircuitPython or in micropython-vst3's engine. What the golden pins is that
every interpreter renders these identically, which matters more here than
almost anywhere else in the suite -- the loop is recursive and runs in
`float`, so a one-ulp disagreement is fed back in and amplified for as long
as the feedback holds.

Two cases here are the separability check rather than coverage. `off` sets
every new option to its default explicitly and has to render exactly what
`feedback_delay_probe.py`'s `plain` renders; `slew-still` turns the slew on
with nothing to glide to, and has to render that same PCM again -- a read
head primed at zero instead of on the delay it was told to hold would sweep
the whole line here and be obvious.
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
    with: the repeats have to arrive somewhere the input is not. The same
    fixture feedback_delay_probe.py uses, so `off` below can be compared with
    its `plain` line for line."""
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


def ramp_table(points=64):
    """One period of a linear ramp, Q15. A bucket brigade's clock ramped
    linearly is a hyperbola on the delay, so a class bakes the reciprocal in
    Python; what this pins is that C reads whatever table it is handed rather
    than a sine. Integer arithmetic on purpose -- a float literal rounded
    differently on one interpreter would be a false alarm about the DSP."""
    values = array("h")
    for index in range(points):
        values.append((index * 65534) // points - 32767)
    return values


def step_table(points=64, edge=8):
    """Two levels with a bounded edge between them: the modulation moves from
    one delay to another over `edge` points instead of jumping, which is the
    shape a step has to have for the delay to reach its new value without a
    discontinuity."""
    values = array("h")
    half = points // 2
    for index in range(points):
        if index < edge:
            level = -32767 + (2 * 32767 * index) // edge
        elif index < half:
            level = 32767
        elif index < half + edge:
            level = 32767 - (2 * 32767 * (index - half)) // edge
        else:
            level = -32767
        values.append(level)
    return values


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        print("fbdx", tag, index, len(data), sum(data), checksum(data))


PLAIN = {"delay_ms": 40.0, "feedback": 0.7, "mix": 1.0}
WOW = {"delay_ms": 40.0, "feedback": 0.7, "mix": 1.0, "wow_hz": 5.0,
       "wow_depth_ms": 3.0}

CASES = (
    # Every new option at its default, said out loud. Has to match `plain`.
    ("off", dict(PLAIN, wow_shape=None, delay_slew=0.0, wow_am_depth=0.0,
                 loop_semitones=0.0, loop_window_ms=25.0)),
    # A slew with nothing to glide to. Has to match `plain` as well.
    ("slew-still", dict(PLAIN, delay_slew=0.5)),

    # wow_shape. The two ends of the length range as well as a useful table,
    # so a build that gets the accumulator's shift or its mask wrong shows.
    ("wow-ramp", dict(WOW, wow_shape=ramp_table())),
    ("wow-step", dict(WOW, wow_shape=step_table())),
    ("wow-tiny", dict(WOW, wow_shape=ramp_table(2))),
    ("wow-long", dict(WOW, wow_shape=ramp_table(4096))),

    # wow_am_depth, with and without the delay moving under it.
    ("wow-am", {"delay_ms": 40.0, "feedback": 0.7, "mix": 1.0, "wow_hz": 5.0,
                "wow_am_depth": 0.8}),
    ("wow-am-full", dict(WOW, mix=0.64, wow_am_depth=1.0)),

    # loop_semitones, up and down, and the window it reads through.
    ("shift-up", dict(PLAIN, loop_semitones=12.0)),
    ("shift-down", {"delay_ms": 40.0, "feedback": 0.6, "mix": 1.0,
                    "loop_semitones": -7.0}),
    ("shift-window", dict(PLAIN, loop_semitones=12.0, loop_window_ms=60.0)),

    # All of it at once, on top of everything the node already had.
    ("everything-new", {"delay_ms": 47.0, "feedback": 0.82, "mix": 0.64,
                        "damping_hz": 1400.0, "cut_hz": 140.0, "wow_hz": 3.0,
                        "wow_depth_ms": 1.5, "cross_feed": 0.6,
                        "loop_drive": 0.5, "input_pan": -0.4,
                        "delay_slew": 0.25, "wow_am_depth": 0.4,
                        "loop_semitones": 5.0,
                        "wow_shape": step_table(128, 16)}),
)

for name, options in CASES:
    node = echo.FeedbackDelay(sample_rate=SAMPLE_RATE, max_delay_ms=120.0,
                              **options)
    node.play(source())
    emit(name, node, 8)

# The glide: `delay_ms` stepped mid-stream with a slew on, so the read head
# walks to the new time instead of jumping to it. 0.5 delay-seconds per second
# is a fifth up for as long as the move lasts, and back down on the return.
node = echo.FeedbackDelay(sample_rate=SAMPLE_RATE, max_delay_ms=120.0,
                          delay_ms=40.0, feedback=0.7, mix=1.0,
                          delay_slew=0.5)
node.play(source())
emit("glide-a", node, 3)
node.set(delay_ms=12.0)
emit("glide-b", node, 5)
node.set(delay_ms=40.0)
emit("glide-c", node, 5)

# A shape table put on and taken off mid-stream. `wow_shape=None` is the sine
# again, and the oscillator it goes back to has been turning the whole time.
node = echo.FeedbackDelay(sample_rate=SAMPLE_RATE, max_delay_ms=120.0,
                          delay_ms=40.0, feedback=0.7, mix=1.0, wow_hz=5.0,
                          wow_depth_ms=3.0)
node.play(source())
emit("shape-a", node, 3)
node.set(wow_shape=ramp_table())
emit("shape-b", node, 3)
node.set(wow_shape=None)
emit("shape-c", node, 3)

# The new options clamp too, at both ends and past them: a slew far longer
# than the line, a depth over unity, a shift past two octaves and a window
# shorter than the floor.
node = echo.FeedbackDelay(sample_rate=SAMPLE_RATE, max_delay_ms=40.0,
                          delay_ms=5000.0, feedback=4.0, mix=1.0,
                          delay_slew=9999.0, wow_am_depth=4.0,
                          loop_semitones=99.0, loop_window_ms=0.0)
node.play(source())
emit("clamped", node, 4)

# A bad table is refused rather than read: not a power of two, longer than the
# accumulator can index, shorter than a waveform, and an odd byte count.
for bad in (array("h", [0] * 63), array("h", [0] * 8192),
            array("h", [0]), b"\x01\x02\x03"):
    try:
        echo.FeedbackDelay(sample_rate=SAMPLE_RATE, max_delay_ms=40.0,
                           wow_shape=bad)
        print("fbdx refused", len(bad), "no")
    except ValueError:
        print("fbdx refused", len(bad), "yes")
