"""Deterministic audiobiquad PCM, and the two invariants the module exists for.

    filter_f32_probe.py audiobiquad

Like multiply_probe.py and feedback_delay_probe.py, this has no oracle:
`audiobiquad` is audiodsp's own module, with no ancestor in CircuitPython or in
micropython-vst3's engine. What the golden pins is that every interpreter
renders it identically, and that nothing moves it by accident later.

It is deliberately **not** byte-gated against `audiofilters.Filter` or
`audiofilters.Phaser`. Those are the ported integer kernels this module was
added because of; agreeing with them bit for bit would mean it had failed.
What is checked against them is a level agreement, in
`tests/test_cpython_audiobiquad.py`, which needs arithmetic no probe should
be doing.

Two of the sections are invariants rather than PCM, because they are the
whole ask:

- **tail** prints the first output block that is all zero after the source
  goes silent. The ported kernels cannot answer this: `audiodsp_biquad.c`'s
  Q12 recursion has fixed points it can park on (measured, audiodsp#23:
  100 Hz at +/-1 LSB, 40 Hz at +/-4 LSB, held for 3000 blocks of silence),
  and `audiodsp_phaser.c`'s int16 all-pass memory has its own. A negative
  number here means the tail never reached zero, and that is a failure.
- **null** prints the settled peak of a tone at a frequency the four-stage
  cascade inverts. At `feedback=0.0` it is a true cancellation; the ported
  node clamps feedback to 0.1 (`audiofilters/Phaser.c:211`) and cannot get
  there.

Everything a probe prints has to be integer-exact on three interpreters, so
the test tone is a 64-entry sine table with integer entries, stepped by whole
samples -- never `math.sin()`, which is three functions that nearly agree.
Nothing here drives a parameter with a `synthio` block for the same reason:
the block layer is C on two targets and Python on the third, and a golden
that hashed it would be measuring that seam rather than this module's.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiobiquad"
# A built-in module under MicroPython, which does not record those in
# sys.modules - take what __import__ hands back.
biquad = __import__(MODULE)

SAMPLE_RATE = 8000

#: One period of a sine at 28000 peak, 64 entries, rounded on CPython once and
#: written down. Stepping it by 1/2/4/8/16 gives 125/250/500/1000/2000 Hz at
#: 8 kHz exactly, with no library call anywhere.
SINE64 = (
    0, 2744, 5463, 8128, 10715, 13199, 15556, 17763,
    19799, 21644, 23281, 24694, 25869, 26794, 27462, 27865,
    28000, 27865, 27462, 26794, 25869, 24694, 23281, 21644,
    19799, 17763, 15556, 13199, 10715, 8128, 5463, 2744,
    0, -2744, -5463, -8128, -10715, -13199, -15556, -17763,
    -19799, -21644, -23281, -24694, -25869, -26794, -27462, -27865,
    -28000, -27865, -27462, -26794, -25869, -24694, -23281, -21644,
    -19799, -17763, -15556, -13199, -10715, -8128, -5463, -2744,
)

#: The four-stage cascade's phase reaches -180 degrees where
#: tan(pi f / fs) = 0.414214 * tan(pi f_break / fs), so a break at 1139.6 Hz
#: inverts 500 Hz -- which is SINE64 stepped by 4.
NULL_BREAK_HZ = 1139.6


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def burst(frames=1600, level=14000, channels=2):
    """A short burst and then nothing, the shape a tail test needs."""
    values = array("h")
    for frame in range(frames):
        for channel in range(channels):
            if frame < 120:
                shape = ((frame * (97 + channel * 18)) % 2001) - 1000
                values.append(shape * level // 1000)
            else:
                values.append(0)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def silence(frames=1600, channels=2):
    values = array("h", bytes(frames * channels * 2))
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def tone(step, frames=2048, channels=1):
    values = array("h")
    index = 0
    for _frame in range(frames):
        for _channel in range(channels):
            values.append(SINE64[index % 64])
        index += step
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        print("f32", tag, index, len(data), sum(data), checksum(data))


def tail(tag, node, channels=2, blocks=400):
    """First all-zero block after the source goes quiet, or -1 for never."""
    node.play(burst(channels=channels))
    # One block only: the burst is 120 frames, so the decay this measures
    # starts right after it rather than a thousand samples further on.
    audiocore.get_buffer(node)
    node.play(silence(channels=channels))
    quiet = -1
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        if data != bytes(len(data)):
            quiet = -1
        elif quiet < 0:
            quiet = index
    print("tail", tag, quiet)


def peak(node, blocks=12, skip=6):
    top = 0
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        if index < skip:
            continue
        for position in range(0, len(data), 2):
            word = data[position] | (data[position + 1] << 8)
            if word >= 32768:
                word -= 65536
            if word < 0:
                word = -word
            if word > top:
                top = word
    return top


# --- the biquad, every mode ------------------------------------------------

BIQUAD_CASES = (
    ("lp", {"mode": biquad.LOW_PASS, "frequency": 900.0}),
    ("lp-low", {"mode": biquad.LOW_PASS, "frequency": 62.5, "Q": 2.0}),
    ("hp", {"mode": biquad.HIGH_PASS, "frequency": 300.0}),
    ("bp", {"mode": biquad.BAND_PASS, "frequency": 1000.0, "Q": 4.0}),
    ("notch", {"mode": biquad.NOTCH, "frequency": 500.0, "Q": 8.0}),
    ("peak-up", {"mode": biquad.PEAKING_EQ, "frequency": 1200.0, "Q": 1.2,
                 "gain_db": 9.0}),
    ("peak-down", {"mode": biquad.PEAKING_EQ, "frequency": 400.0, "Q": 0.9,
                   "gain_db": -12.0}),
    ("low-shelf", {"mode": biquad.LOW_SHELF, "frequency": 220.0,
                   "gain_db": 6.0}),
    ("high-shelf", {"mode": biquad.HIGH_SHELF, "frequency": 2400.0,
                    "gain_db": -6.0}),
    ("blend", {"mode": biquad.LOW_PASS, "frequency": 700.0, "mix": 0.37}),
    ("wire", {"mode": biquad.LOW_PASS, "frequency": 700.0, "mix": 0.0}),
)

for name, options in BIQUAD_CASES:
    for channels in (1, 2):
        node = biquad.Biquad(sample_rate=SAMPLE_RATE, channel_count=channels,
                             **options)
        node.play(burst(channels=channels))
        emit("bq-" + name + "-" + str(channels), node, 6)

# Clamping: past Nyquist, a negative Q, a mix over unity.
node = biquad.Biquad(sample_rate=SAMPLE_RATE, channel_count=2,
                     mode=biquad.LOW_PASS, frequency=99000.0, Q=-3.0, mix=4.0)
node.play(burst())
emit("bq-clamped", node, 4)

# Set mid-stream: the recursion keeps its memory and only the coefficients
# change, which is a different fixture from building it that way.
node = biquad.Biquad(sample_rate=SAMPLE_RATE, channel_count=2,
                     mode=biquad.LOW_PASS, frequency=1500.0)
node.play(burst())
emit("bq-late-a", node, 3)
node.frequency = 300.0
node.Q = 3.0
emit("bq-late-b", node, 3)
node.clear()
emit("bq-late-c", node, 3)

# No source: silence, never a short block, never finished.
node = biquad.Biquad(sample_rate=SAMPLE_RATE, channel_count=2)
emit("bq-starved", node, 2)

# --- the all-pass cascade --------------------------------------------------

ALLPASS_CASES = (
    ("plain", {"stages": 4, "frequency": 1139.6, "feedback": 0.0,
               "mix": 0.5}),
    ("one", {"stages": 1, "frequency": 800.0, "feedback": 0.0, "mix": 0.5}),
    ("six", {"stages": 6, "frequency": 1139.6, "feedback": 0.6, "mix": 0.5}),
    ("twelve", {"stages": 12, "frequency": 700.0, "feedback": 0.4,
                "mix": 0.5}),
    ("inverted", {"stages": 4, "frequency": 1139.6, "feedback": -0.7,
                  "mix": 0.5}),
    ("hot", {"stages": 4, "frequency": 1139.6, "feedback": 0.95, "mix": 0.5}),
    ("bare", {"stages": 4, "frequency": 1139.6, "feedback": 0.0, "mix": 1.0}),
    ("wire", {"stages": 4, "frequency": 1139.6, "feedback": 0.9, "mix": 0.0}),
)

for name, options in ALLPASS_CASES:
    for channels in (1, 2):
        node = biquad.AllPass(sample_rate=SAMPLE_RATE,
                              channel_count=channels, **options)
        node.play(burst(channels=channels))
        emit("ap-" + name + "-" + str(channels), node, 6)

# Clamping: feedback past unity in both directions, frequency past Nyquist.
node = biquad.AllPass(sample_rate=SAMPLE_RATE, channel_count=2, stages=4,
                      frequency=99000.0, feedback=4.0, mix=0.5)
node.play(burst())
emit("ap-clamped-high", node, 4)
node = biquad.AllPass(sample_rate=SAMPLE_RATE, channel_count=2, stages=4,
                      frequency=-50.0, feedback=-4.0, mix=0.5)
node.play(burst())
emit("ap-clamped-low", node, 4)

# The sweep: the coefficient walks to each block's value across the block
# rather than stepping to it at the boundary, so a moved frequency is a glide.
node = biquad.AllPass(sample_rate=SAMPLE_RATE, channel_count=2, stages=4,
                      frequency=400.0, feedback=0.5, mix=0.5)
node.play(tone(4, channels=2))
emit("ap-sweep-a", node, 2)
node.frequency = 2400.0
emit("ap-sweep-b", node, 2)
node.frequency = 400.0
emit("ap-sweep-c", node, 2)

node = biquad.AllPass(sample_rate=SAMPLE_RATE, channel_count=2)
emit("ap-starved", node, 2)

# --- Tier 1: a decaying tail reaches exact zero ----------------------------

for name, options in (
        ("lp-100", {"mode": biquad.LOW_PASS, "frequency": 100.0}),
        ("lp-40-q8", {"mode": biquad.LOW_PASS, "frequency": 40.0, "Q": 8.0}),
        ("lp-31", {"mode": biquad.LOW_PASS, "frequency": 31.25}),
        ("lp-62", {"mode": biquad.LOW_PASS, "frequency": 62.5}),
        ("hp-31", {"mode": biquad.HIGH_PASS, "frequency": 31.25}),
        ("peak-62", {"mode": biquad.PEAKING_EQ, "frequency": 62.5, "Q": 4.0,
                     "gain_db": 12.0}),
        ("shelf-62", {"mode": biquad.LOW_SHELF, "frequency": 62.5,
                      "gain_db": 12.0}),
):
    tail("bq-" + name, biquad.Biquad(sample_rate=SAMPLE_RATE,
                                     channel_count=2, **options))

for name, options in (
        ("ap-4-fb0", {"stages": 4, "frequency": 1139.6, "feedback": 0.0,
                      "mix": 0.5}),
        ("ap-4-fb95", {"stages": 4, "frequency": 1139.6, "feedback": 0.95,
                       "mix": 0.5}),
        ("ap-4-inv", {"stages": 4, "frequency": 1139.6, "feedback": -0.95,
                      "mix": 0.5}),
        ("ap-12-low", {"stages": 12, "frequency": 31.25, "feedback": 0.5,
                       "mix": 0.5}),
):
    tail(name, biquad.AllPass(sample_rate=SAMPLE_RATE, channel_count=2,
                              **options))

# --- P4: the null the 0.1 feedback clamp puts out of reach -----------------

wire = biquad.AllPass(sample_rate=SAMPLE_RATE, channel_count=1, stages=4,
                      frequency=NULL_BREAK_HZ, feedback=0.0, mix=0.0)
wire.play(tone(4))
print("null dry", peak(wire))

for feedback in (0.0, 0.05, 0.1, 0.3, -0.5, 0.7):
    node = biquad.AllPass(sample_rate=SAMPLE_RATE, channel_count=1, stages=4,
                          frequency=NULL_BREAK_HZ, feedback=feedback,
                          mix=0.5)
    node.play(tone(4))
    print("null", int(feedback * 100), peak(node))
