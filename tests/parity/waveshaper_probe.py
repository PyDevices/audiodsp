"""Deterministic Waveshaper PCM.

    waveshaper_probe.py audioshaper

Like multiply_probe.py and feedback_delay_probe.py, this has no oracle:
`audioshaper` is audiodsp's own module, with no ancestor in CircuitPython or in
micropython-vst3's engine. What the golden pins is that every interpreter
renders it identically.

The curves are built out of integer arithmetic on purpose. A curve computed
with `math.exp` would be a different table under CircuitPython, whose floats
are single-precision, than under CPython and MicroPython, whose are double --
and the probe would then be measuring the interpreters' libm rather than this
node. Everything here is `//` on ints, so the same table reaches all three.

The half-band filters are recursive and run in `float` at up to eight times
the sample rate, so this is a sensitive fixture in the same way the feedback
delay's is: a one-ulp disagreement between two builds is fed back through the
all-pass sections rather than staying one ulp.

Two of the lines are verdicts rather than PCM. `hysteresis=0` has to render
the same bytes as a node that was never given the option at all -- that is
what makes it an additive option rather than a change -- and `hysteresis=0.7`
has to render different bytes, or the option is not wired to anything. Both
are printed, so the golden pins the answers and not just the samples.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audioshaper"
# A built-in module under MicroPython, which does not record those in
# sys.modules - take what __import__ hands back.
shaper = __import__(MODULE)

SAMPLE_RATE = 8000
POINTS = 1024
Q15 = 32768


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def clamp15(value):
    if value > 32767:
        return 32767
    if value < -32768:
        return -32768
    return value


def curve_positions():
    """Q15 input positions the table spans, -32768 to +32768."""
    last = POINTS - 1
    return [(2 * index - last) * Q15 // last for index in range(POINTS)]


def linear_curve():
    """The identity. Pins the half-bands with nothing else in the way."""
    return array("h", [clamp15(x) for x in curve_positions()])


def cubic_curve():
    """y = 1.5x - 0.5x^3: odd-symmetric, so third harmonic and no second."""
    return array("h", [clamp15((3 * x * Q15 * Q15 - x * x * x) //
                               (2 * Q15 * Q15)) for x in curve_positions()])


def asymmetric_curve():
    """Clips at +0.55 and -0.95 - a germanium pair's two forward drops. The
    one thing none of audiofilters.Distortion's odd-symmetric curves can do:
    make an even harmonic."""
    high = 55 * Q15 // 100
    low = -95 * Q15 // 100
    return array("h", [clamp15(min(max(x, low), high))
                       for x in curve_positions()])


LINEAR = linear_curve()
CUBIC = cubic_curve()
ASYMMETRIC = asymmetric_curve()


def source(frames=1200, level=11000):
    """Sustained, not a burst: a shaper is judged on what it does to a signal
    that keeps arriving, and the two channels differ so a channel swap shows."""
    values = array("h")
    for frame in range(frames):
        for channel in range(2):
            shape = ((frame * (53 + channel * 11)) % 401) - 200
            values.append(shape * level // 200)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=2)


def render(blocks, **options):
    node = shaper.Waveshaper(sample_rate=SAMPLE_RATE, **options)
    node.play(source())
    return [bytes(audiocore.get_buffer(node)[1]) for _ in range(blocks)]


def emit(tag, blocks):
    for index in range(len(blocks)):
        data = blocks[index]
        print("shp", tag, index, len(data), sum(data), checksum(data))


# The identity table at every factor: what the half-bands alone do. Then the
# cubic and the asymmetric curve, then each control in turn, so a failure says
# which part of the path moved.
CASES = (
    ("wire", {"curve": LINEAR, "oversample": 4, "mix": 0.0}),
    ("linear-x1", {"curve": LINEAR, "oversample": 1}),
    ("linear-x2", {"curve": LINEAR, "oversample": 2}),
    ("linear-x4", {"curve": LINEAR, "oversample": 4}),
    ("linear-x8", {"curve": LINEAR, "oversample": 8}),
    ("cubic-x1", {"curve": CUBIC, "oversample": 1, "pre_gain": 4.0,
                  "post_gain": 0.5}),
    ("cubic-x2", {"curve": CUBIC, "oversample": 2, "pre_gain": 4.0,
                  "post_gain": 0.5}),
    ("cubic-x4", {"curve": CUBIC, "oversample": 4, "pre_gain": 4.0,
                  "post_gain": 0.5}),
    ("cubic-x8", {"curve": CUBIC, "oversample": 8, "pre_gain": 4.0,
                  "post_gain": 0.5}),
    ("asymmetric", {"curve": ASYMMETRIC, "oversample": 4, "pre_gain": 3.0,
                    "post_gain": 0.6}),
    ("biased", {"curve": ASYMMETRIC, "oversample": 4, "pre_gain": 3.0,
                "bias": 0.25, "post_gain": 0.6}),
    ("blend", {"curve": CUBIC, "oversample": 2, "pre_gain": 6.0,
               "post_gain": 0.4, "mix": 0.37}),
    ("hysteresis", {"curve": CUBIC, "oversample": 4, "pre_gain": 4.0,
                    "post_gain": 0.5, "hysteresis": 0.7,
                    "hysteresis_width": 0.08}),
    ("hysteresis-skewed", {"curve": CUBIC, "oversample": 4, "pre_gain": 4.0,
                           "post_gain": 0.5, "hysteresis": 0.7,
                           "hysteresis_width": 0.08,
                           "hysteresis_bias": -0.6}),
    ("mono", {"curve": CUBIC, "oversample": 4, "pre_gain": 4.0,
              "post_gain": 0.5, "channel_count": 1}),
    ("hot", {"curve": CUBIC, "oversample": 4, "pre_gain": 200.0}),
)

for name, options in CASES:
    emit(name, render(6, **options))

# Set mid-stream: the half-band memories and the play position keep their
# contents and only what the node does to them changes, which is a different
# fixture from building it that way.
node = shaper.Waveshaper(sample_rate=SAMPLE_RATE, curve=CUBIC, oversample=4,
                         pre_gain=2.0, post_gain=0.6)
node.play(source())
emit("late-a", [bytes(audiocore.get_buffer(node)[1]) for _ in range(3)])
node.set(pre_gain=9.0, hysteresis=0.5, hysteresis_width=0.1)
emit("late-b", [bytes(audiocore.get_buffer(node)[1]) for _ in range(3)])
node.set(curve=ASYMMETRIC)
emit("late-c", [bytes(audiocore.get_buffer(node)[1]) for _ in range(3)])
node.clear()
emit("late-d", [bytes(audiocore.get_buffer(node)[1]) for _ in range(3)])

# No source: silence, never a short block, never finished.
node = shaper.Waveshaper(sample_rate=SAMPLE_RATE, curve=CUBIC)
emit("starved", [bytes(audiocore.get_buffer(node)[1]) for _ in range(2)])

# The option is additive: off, the node is the static table it was before the
# option existed, sample for sample.
STATIC = {"curve": CUBIC, "oversample": 4, "pre_gain": 4.0, "post_gain": 0.5}
plain = render(4, **STATIC)
off = render(4, hysteresis=0.0, hysteresis_width=0.08,
             hysteresis_bias=0.4, **STATIC)
on = render(4, hysteresis=0.7, hysteresis_width=0.08, **STATIC)
print("shp hysteresis-off %s" %
      ("identical" if off == plain else "DIFFERENT"))
print("shp hysteresis-on %s" %
      ("changed" if on != plain else "UNCHANGED"))

# mix=0 is the wire, exactly: the dry path is the untouched input.
straight = render(2, curve=CUBIC, oversample=8, pre_gain=40.0, mix=0.0)
values = source()
expected = bytes(audiocore.get_buffer(values)[1])[:len(straight[0])]
print("shp mix-zero %s" % ("wire" if straight[0] == expected else "SHAPED"))
