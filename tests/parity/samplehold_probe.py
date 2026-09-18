"""Deterministic SampleHold PCM, and the counts PCM alone would not say.

    samplehold_probe.py audioshaper

Like `waveshaper_probe.py` beside it this has no oracle: `audioshaper` is
audioif's own module, with no ancestor in CircuitPython or in
micropython-vst3's engine. What the gate pins is that every interpreter
renders it identically.

A probe of its own rather than cases appended to `waveshaper_probe.py`, for
the reason `verify_dsp.py` gives: one comparison covers a probe's whole
output, so a case added there would move the very numbers that say the
Waveshaper's bytes did not change.

Every fixture is integer arithmetic and every source frame is distinct --
`(frame * 7) % 65536`, which repeats no value in 65 536 frames. Both are
deliberate. Integers because a table built with `math.sin` would be a
different table under CircuitPython, whose floats are single-precision, and
the probe would be measuring three libms. Distinct frames because it is what
makes the **refresh count** readable from the output: with no two source
frames alike, the held value changes on exactly the frames the accumulator
wrapped on, so counting transitions counts refreshes.

Three of the lines are verdicts rather than PCM, and they are the ones this
node exists for. `refresh` is the count over a whole number of periods, which
must equal `frames * den / num` exactly -- that is the claim a pair of
`audiospeed.SpeedChanger` nodes could not make (audioif#97), and no amount of
PCM says whether it still holds. `frames` is one frame out per frame in.
`wire` is 1/1 rendering the source's own bytes.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audioshaper"
# A built-in module under MicroPython, which does not record those in
# sys.modules - take what __import__ hands back.
hold = __import__(MODULE)

SAMPLE_RATE = 48000
#: `GET_BUFFER_DONE`. CircuitPython does not export the name from `audiocore`,
#: so the number is used rather than imported -- the deinit probe does the same
#: with `GET_BUFFER_ERROR`.
DONE = 0


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def source(frames, channels=2):
    """Every frame distinct, and the two channels differ so a channel swap
    shows. `* 7` is co-prime with 65536, so no value repeats."""
    values = array("h")
    for frame in range(frames):
        for channel in range(channels):
            values.append(((frame * 7 + channel * 30011) % 65536) - 32768)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def render(node, blocks):
    """Up to `blocks` blocks, stopping where the node says it is done."""
    out = []
    for _index in range(blocks):
        result, data = audiocore.get_buffer(node)
        out.append(bytes(data))
        if result == DONE:
            break
    return out


def emit(tag, blocks):
    for index in range(len(blocks)):
        data = blocks[index]
        print("hold", tag, index, len(data), sum(data), checksum(data))


def held_words(node, channels=2):
    """Every rendered frame's first channel, as signed ints."""
    words = []
    for data in render(node, 400):
        for position in range(0, len(data), 2 * channels):
            word = data[position] | (data[position + 1] << 8)
            words.append(word - 65536 if word >= 32768 else word)
    return words


#: (tag, num, den, channels, frames). 400/217 is the effects programme's
#: shipped hold -- 26 040 Hz at 48 kHz, reduced -- and 105/62 is the same hold
#: at 44.1 kHz. 1/1 is the wire, 2/1 and 3/1 are the integer ratios where a
#: staircase is easiest to read by eye, and 1024/3 is a ratio deep enough that
#: a fixed-point accumulator would have nothing left.
CASES = (
    ("wire", 1, 1, 2, 1200),
    ("half", 2, 1, 2, 1200),
    ("third", 3, 1, 2, 1200),
    ("standout", 400, 217, 2, 1200),
    ("standout-44k", 105, 62, 2, 1200),
    ("deep", 1024, 3, 2, 1200),
    ("mono", 400, 217, 1, 1200),
    ("unreduced", 48000, 26040, 2, 1200),
)

for name, num, den, channels, frames in CASES:
    node = hold.SampleHold(source(frames, channels), num=num, den=den)
    print("hold ratio", name, node.num, node.den, node.latency,
          node.sample_rate, node.channel_count, node.bits_per_sample)
    emit(name, render(node, 6))

# Set mid-stream: the ratio moves and the accumulator re-arms, which is a
# different fixture from building the node that way. Setting it to what it
# already is must change nothing at all, which is what `late-c` pins.
node = hold.SampleHold(source(4096), num=400, den=217)
emit("late-a", render(node, 3))
node.set(3, 1)
emit("late-b", render(node, 3))
node.set(3, 1)
emit("late-c", render(node, 3))
node.clear()
emit("late-d", render(node, 3))

# Re-sourced: `play` starts the staircase over on a new stream of the same
# format.
node = hold.SampleHold(source(4096), num=400, den=217)
emit("replay-a", render(node, 2))
node.play(source(4096))
emit("replay-b", render(node, 2))

# The count, over a whole number of periods, is exactly frames * den / num.
# This is the line the issue is about: two SpeedChangers could not hold this
# for any ratio that was not a power of two.
for name, num, den, frames in (("standout", 400, 217, 12000),
                               ("standout-44k", 105, 62, 12600),
                               ("deep", 1024, 3, 12288),
                               ("half", 2, 1, 12000),
                               ("wire", 1, 1, 12000)):
    words = held_words(hold.SampleHold(source(frames), num=num, den=den))
    refreshes = 1
    for index in range(1, len(words)):
        if words[index] != words[index - 1]:
            refreshes += 1
    print("hold refresh %s %d %d %d"
          % (name, refreshes, len(words), frames * den // num))
    print("hold frames %s %d %d" % (name, frames, len(words)))

# 1/1 is a wire, byte for byte, and not merely close.
values = source(1200)
straight = render(hold.SampleHold(values, num=1, den=1), 2)
expected = bytes(audiocore.get_buffer(source(1200))[1])[:len(straight[0])]
print("hold wire %s" % ("identical" if straight[0] == expected else "HELD"))
