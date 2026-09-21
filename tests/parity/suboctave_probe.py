"""Deterministic SubOctave PCM.

    suboctave_probe.py audiomath

Like multiply_probe.py and feedback_delay_probe.py this one has no oracle:
`audiomath` is audiodsp's own module, with no ancestor in CircuitPython or in
micropython-vst3's engine. What the golden pins is that every interpreter
renders it identically -- the arithmetic is entirely inside
shared/audiodsp_suboctave.c, the same C all three link, so a disagreement
between two of them would itself be the finding.

The fixtures are built with integer arithmetic only, the way multiply_probe.py
builds its modulator: a divider is decided by *where* the signal crosses a
threshold, so a fixture that moved by one LSB because two interpreters rounded
sin() differently would move an edge and change every sample after it.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiomath"
# A built-in module under MicroPython, which does not record those in
# sys.modules - take what __import__ hands back.
math_module = __import__(MODULE)

SAMPLE_RATE = 48000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def wave(period, frame, level):
    """One triangle sample: `period` frames to the cycle, `level` at the
    peaks. Integer throughout - see the module docstring."""
    position = (frame * 4) % (period * 4)
    if position < period * 2:
        step = position - period
    else:
        step = period * 3 - position
    return step * level // period


def tone(period, frames=1200, level=20000, channel_count=2, skew=0):
    """A triangle at `period` frames to the cycle. `skew` detunes the right
    channel's period, which is what proves one divider drives both: two
    dividers would count differently the moment the channels differ."""
    values = array("h")
    for frame in range(frames):
        for channel in range(channel_count):
            values.append(wave(period + channel * skew, frame, level))
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channel_count)


def two_tone(period, frames=1200, low=13000, high=11000):
    """A fundamental with a second harmonic large enough to cross zero twice
    more per cycle. This is the waveform the threshold and the hold gate
    exist for: a divider that counts the harmonic's crossings drops an octave
    too far, and both options are ways of not counting them."""
    values = array("h")
    for frame in range(frames):
        value = wave(period, frame, low) + wave(period // 2, frame, high)
        if value > 32767:
            value = 32767
        elif value < -32768:
            value = -32768
        values.append(value)
        values.append(value)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=2)


def pulses(spacing, frames=1400, level=15000):
    """One frame up, one frame down, then nothing, every `spacing` frames.
    Edges land exactly where they are asked for, which is what pins the
    default lockout: the interval either fits inside it or does not, with no
    waveform shape in the way."""
    values = array("h")
    for frame in range(frames):
        phase = frame % spacing
        value = level if phase == 0 else (-level if phase == 1 else 0)
        values.append(value)
        values.append(value)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=2)


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        print("sub", tag, index, len(data), sum(data), checksum(data))


# Both orders at both mix rails and a fraction that exercises the blend, over
# a period either side of the 256-frame block so the count has to survive a
# block boundary. 218 frames is 220.2 Hz at 48 kHz; 109 is 440.4 Hz.
for period in (109, 218):
    for order in (1, 2):
        for mix in (0.0, 0.35, 1.0):
            node = math_module.SubOctave(tone(period), order=order, mix=mix)
            emit("%d/%d/%.2f" % (period, order, mix), node, 5)

# The threshold, from a bare zero-crossing detector up to a rail the signal
# barely reaches, on the waveform that has something to reject.
for threshold in (0.0, 0.01, 0.25, 0.9):
    node = math_module.SubOctave(two_tone(218), order=1, mix=1.0,
                                 threshold=threshold, hold_ms=0.0)
    emit("thr/%.2f" % threshold, node, 5)

# The hold gate, on the same waveform: 0 accepts every crossing, 2.0 ms is
# shorter than the harmonic's half period (2.27 ms at 218 frames) and still
# accepts them, 6.0 ms rejects them, 1000.0 is clamped to a tenth of a second.
for hold in (0.0, 2.0, 6.0, 1000.0):
    node = math_module.SubOctave(two_tone(218), order=1, mix=1.0,
                                 threshold=0.0, hold_ms=hold)
    emit("hold/%.1f" % hold, node, 5)

# The two defaults, pinned. Nothing above reaches either: every threshold and
# hold case names its own value, and the triangles elsewhere step by hundreds
# of LSB a frame, so moving the default threshold by one would not move an
# edge. These two are built so that it does.
#
# A creep: 1600 frames to the cycle at a peak of 400, so consecutive samples
# differ by exactly 1 and the default threshold (328, about -40 dBFS) decides
# which frame the first edge lands on.
node = math_module.SubOctave(tone(1600, frames=4000, level=400), order=1,
                             mix=1.0)
emit("creep", node, 6)

# Edges 45 and 50 frames apart, either side of the default lockout of 48
# (1 ms at 48 kHz), which pins it from both directions: at 45 they are
# rejected and only every other one counts, at 50 they are all accepted.
for spacing in (45, 50):
    node = math_module.SubOctave(pulses(spacing), order=1, mix=1.0)
    emit("lockout/%d" % spacing, node, 5)

# Mono: one channel, so the divider is clocked by the sample rather than by a
# mean of two.
node = math_module.SubOctave(tone(218, channel_count=1), order=1, mix=1.0,
                             channel_count=1)
emit("mono", node, 4)

# Channels that disagree. One divider drives both, clocked by their mean, so
# the pair cannot come apart and cancel in mono.
node = math_module.SubOctave(tone(218, skew=37), order=2, mix=1.0)
emit("skewed", node, 4)

# Rails: a full-scale square, both channels together so the comparator
# actually sees it. Negating -32768 gives +32768, the one product that lands
# outside int16, and it has to clamp rather than wrap.
rails = array("h")
for frame in range(1200):
    value = 32767 if (frame // 109) % 2 else -32768
    rails.append(value)
    rails.append(value)
node = math_module.SubOctave(
    audiocore.RawSample(rails, sample_rate=SAMPLE_RATE, channel_count=2),
    order=1, mix=1.0)
emit("rails", node, 4)

# Silence in, silence out: no crossing, no edge, no toggle, and nothing to
# invert either way.
node = math_module.SubOctave(
    audiocore.RawSample(array("h", [0] * 2400), sample_rate=SAMPLE_RATE,
                        channel_count=2), order=2, mix=1.0)
emit("silence", node, 4)

# A signal that never reaches the threshold: the same, but with something to
# pass through.
node = math_module.SubOctave(tone(218, level=200), order=1, mix=1.0,
                             threshold=0.25)
emit("quiet", node, 4)

# Set mid-stream. The count keeps running across all of it - only clear()
# restarts it - which is the whole reason these are options rather than
# constructor-only.
node = math_module.SubOctave(tone(218, frames=4000), order=1, mix=1.0)
emit("late-a", node, 2)
node.set(order=2)
emit("late-b", node, 2)
node.set(mix=0.5, threshold=0.2, hold_ms=4.0)
emit("late-c", node, 2)
node.clear()
emit("late-d", node, 2)

# No source: silence, never a short block, never finished.
node = math_module.SubOctave()
emit("starved", node, 2)
