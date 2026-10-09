"""PitchShift.freeze, from CircuitPython 11.0.0-alpha.1 (9bef7b7606).

    pitchshift_freeze_probe.py audiodelays           # PASS, or FAIL and exit 1
    pitchshift_freeze_probe.py audiodelays --fault   # never sets freeze: must FAIL

A frozen PitchShift stops writing its window and holds its write position,
while the read position keeps going round, so what is in the window sustains.
Setting `freeze` back to False lets the source in again, and a reset of the
node's buffer clears it.

The probe renders a PitchShift with `freeze` turned on and off between pulls,
mono and stereo, with and without an overlap, wet and half dry, and prints a
checksum of every block, so every interpreter can be held to the same bytes
(`verify_dsp.py`, where CircuitPython 11.0.0-alpha.1 is the third).

It also checks what freezing means, so a build that ignores `freeze` fails here
even where every interpreter ignores it together:

- at 0 semitones and fully wet, the read position goes round the window once
  per window's worth of frames, so with the buffer the size of the window every
  frozen block is the same bytes, where the unfrozen blocks before it are not;
- `freeze` reads False after `audiocore.reset_buffer()`, and takes any value
  by its truth.

`--fault` stores the flag somewhere the node never sees, which is what a build
that ignores it renders, and the checks must fail.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else "audiodelays"
delays = __import__(MODULE)
FAULT = "--fault" in sys.argv

SAMPLE_RATE = 8000
failures = []


class _Ignored:
    freeze = False


_IGNORED = _Ignored()


def set_freeze(node, value):
    (_IGNORED if FAULT else node).freeze = value


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def source(channels, frames=1601, level=14000):
    # A saw whose period drifts, looped over a length that shares no factor
    # with the window, so no two unfrozen blocks are alike.
    values = array("h")
    for frame in range(frames):
        for channel in range(channels):
            shape = ((frame * (97 + channel * 18 + frame // 200)) % 2001) - 1000
            values.append(shape * level // 1000)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def emit(tag, node, blocks):
    sums = []
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        value = checksum(data)
        sums.append(value)
        print("freeze", tag, index, node.freeze, len(data), sum(data), value)
    return sums


def node_for(channels, **options):
    node = delays.PitchShift(sample_rate=SAMPLE_RATE, channel_count=channels,
                             buffer_size=1024, **options)
    node.play(source(channels), loop=True)
    return node


# Frozen mid-stream and let go again, at a shift up and a shift down.
CASES = (
    ("mono_up", 1, {"semitones": 7, "mix": 1.0, "window": 1024,
                    "overlap": 128}),
    ("stereo_down", 2, {"semitones": -5, "mix": 1.0, "window": 1024,
                        "overlap": 128}),
    ("mono_no_overlap", 1, {"semitones": 7, "mix": 1.0, "window": 1024,
                            "overlap": 0}),
    ("stereo_half_dry", 2, {"semitones": 12, "mix": 0.5, "window": 2048,
                            "overlap": 256}),
)

for name, channels, options in CASES:
    node = node_for(channels, **options)
    emit(name + "_before", node, 2)
    set_freeze(node, True)
    emit(name + "_frozen", node, 3)
    set_freeze(node, False)
    emit(name + "_after", node, 2)

# The sustain: at 0 semitones the read position comes round once a window, and
# the buffer here is exactly one window, so every frozen block repeats.
for channels in (1, 2):
    tag = "sustain_%d" % channels
    node = node_for(channels, semitones=0, mix=1.0, window=1024, overlap=128)
    before = emit(tag + "_before", node, 3)
    set_freeze(node, True)
    frozen = emit(tag + "_frozen", node, 3)
    if len(set(before)) != len(before):
        failures.append(tag + ": unfrozen blocks repeat, so the check is blind")
    if len(set(frozen)) != 1:
        failures.append(tag + ": frozen blocks are not the same bytes")

# A reset of the node's buffer lets go of the freeze.
node = node_for(1, semitones=7, mix=1.0, window=1024, overlap=128)
emit("reset_before", node, 1)
set_freeze(node, True)
emit("reset_frozen", node, 1)
audiocore.reset_buffer(node)
print("freeze reset_buffer", node.freeze)
if node.freeze is not False:
    failures.append("reset_buffer left freeze set")
emit("reset_after", node, 2)

# Any value, by its truth.
node = delays.PitchShift(sample_rate=SAMPLE_RATE)
results = []
for value in (1, 0, "yes", "", [0], []):
    set_freeze(node, value)
    results.append(node.freeze)
print("freeze truth", " ".join(str(result) for result in results))
if results != [True, False, True, False, True, False]:
    failures.append("freeze does not take a value by its truth")

if failures:
    for failure in failures:
        print("FAIL", failure)
    print("PITCHSHIFT FREEZE FAIL")
    sys.exit(1)
print("PITCHSHIFT FREEZE PASS")
