"""A reset puts the pitch shifters back where a new node starts.

    pitch_reset_probe.py audiodelays

`audiocore.reset_buffer()` on `PitchShift` or `GranularPitchShift` clears what
the node holds of its own and keeps the source frames it has already taken
(audiodsp#181). What it holds of its own is more than its buffers:

- `PitchShift` reads its window at the shifted rate while it writes it at the
  source's, so the distance between the two positions is the splice phase,
  and it moves every frame the shift is not 0. A reset that zeroes the window
  and leaves the positions where they were splices the next audio at a phase
  no new node starts at, and the output never comes back to a new node's.
- `GranularPitchShift` jitters each grain's start from a random sequence when
  `spread` is above 0. A reset that leaves the sequence where it had got to
  starts the next grains at offsets no new node would draw.

Each case is fed one long buffer, pulled for several blocks, reset, and pulled
for three more. Those three must be exactly what a new node renders from the
source frames the first blocks had not used. Every line prints the PCM it
compared, so `verify_dsp.py` holds the interpreters to the same bytes too.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiodelays"
delays = __import__(MODULE)

RATE = 22050
FRAMES = 6000
BEFORE = 7          # blocks pulled before the reset
AFTER = 3           # blocks compared after it


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def material(channels, start):
    """A decorrelated signal whose every frame differs from its neighbours,
    so an offset of even one frame changes the bytes."""
    values = array("h")
    for frame in range(start, FRAMES):
        values.append((((frame * 97) % 2001) - 1000) * 12)
        if channels == 2:
            values.append((((frame * 131) % 1777) - 888) * 13)
    return audiocore.RawSample(values, sample_rate=RATE,
                               channel_count=channels)


def pull(node, blocks):
    out = b""
    for _ in range(blocks):
        out += bytes(audiocore.get_buffer(node)[1])
    return out


CASES = (
    ("PitchShift fifth wet", 1, lambda c: delays.PitchShift(
        semitones=7, mix=1.0, window=1024, overlap=128,
        sample_rate=RATE, channel_count=c)),
    ("PitchShift fifth half", 2, lambda c: delays.PitchShift(
        semitones=7, mix=0.5, window=1024, overlap=128,
        sample_rate=RATE, channel_count=c)),
    ("PitchShift down no overlap", 1, lambda c: delays.PitchShift(
        semitones=-5, mix=1.0, window=1024, overlap=0,
        sample_rate=RATE, channel_count=c)),
    ("PitchShift down", 2, lambda c: delays.PitchShift(
        semitones=-5, mix=1.0, window=2048, overlap=256,
        sample_rate=RATE, channel_count=c)),
    ("GranularPitchShift fifth", 1, lambda c: delays.GranularPitchShift(
        semitones=7, mix=1.0, grain_size=256, density=2,
        sample_rate=RATE, channel_count=c)),
    ("GranularPitchShift spread", 1, lambda c: delays.GranularPitchShift(
        semitones=-12, mix=1.0, grain_size=256, density=3, spread=0.75,
        sample_rate=RATE, channel_count=c)),
    ("GranularPitchShift spread", 2, lambda c: delays.GranularPitchShift(
        semitones=7, mix=0.5, grain_size=256, density=2, spread=0.5,
        sample_rate=RATE, channel_count=c)),
)

failures = []

for name, channels, build in CASES:
    node = build(channels)
    node.play(material(channels, 0))
    before = pull(node, BEFORE)
    taken = len(before) // (2 * channels)
    audiocore.reset_buffer(node)
    after = pull(node, AFTER)

    fresh = build(channels)
    fresh.play(material(channels, taken))
    expected = pull(fresh, AFTER)

    same = after == expected
    print("reset", name, channels, taken, len(after), checksum(before),
          checksum(after), same)
    if not same:
        failures.append("%s (%d ch)" % (name, channels))

if failures:
    print("FAIL", "; ".join(failures))
    sys.exit(1)
print("pitch reset: every shifter starts again where a new one does")
