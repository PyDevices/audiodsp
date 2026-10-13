"""The pitch shifters return from a pull whose looping source has nothing.

    pitch_empty_source_probe.py audiodelays

A looping source that comes back empty straight after its reset used to spin
`PitchShift`'s and `GranularPitchShift`'s pull loop for ever on the native
nodes: each pass asked again, got 0 frames and advanced by 0, so the pull
never returned and whatever was pulling them (the pump, a mixer) stood still.
Upstream CircuitPython's loop has the same shape. Here, as on CPython all
along, the node takes the empty buffer that came with `GET_BUFFER_DONE` as its
source's end: it lets go of the source and the rest of the block is silence.

Each node below plays a short buffer through to its end and then an empty
looping one, mono and stereo. On a build that spins, this probe never
finishes; that is the failure. Every line prints the PCM, so `verify_dsp.py`
holds the interpreters to the same bytes too.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiodelays"
delays = __import__(MODULE)

RATE = 8000
FRAMES = 300


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def material(channels, frames=FRAMES):
    values = array("h")
    for frame in range(frames):
        for channel in range(channels):
            values.append((((frame * (97 + 34 * channel)) % 2001) - 1000) * 12)
    return values


def empty(channels):
    # MicroPython's RawSample refuses an empty array but takes an empty
    # 16-bit memoryview.
    return audiocore.RawSample(memoryview(array("h", [0, 0]))[0:0],
                               sample_rate=RATE, channel_count=channels)


NODES = (
    ("PitchShift", lambda c: delays.PitchShift(
        semitones=7, mix=0.5, window=1024, overlap=128, sample_rate=RATE,
        channel_count=c)),
    ("GranularPitchShift", lambda c: delays.GranularPitchShift(
        semitones=7, mix=0.5, grain_size=256, density=2, sample_rate=RATE,
        channel_count=c)),
)

failures = []

for name, build in NODES:
    for channels in (1, 2):
        node = build(channels)
        node.play(audiocore.RawSample(material(channels), sample_rate=RATE,
                                      channel_count=channels))
        played = b""
        for _ in range(2):
            played += bytes(audiocore.get_buffer(node)[1])
        node.play(empty(channels), loop=True)
        data = bytes(audiocore.get_buffer(node)[1])
        silent = not any(data)
        print("empty", name, channels, len(played), checksum(played),
              len(data), silent, node.playing)
        if len(data) == 0 or not silent or node.playing:
            failures.append("%s (%d ch)" % (name, channels))

if failures:
    print("FAIL", "; ".join(failures))
    sys.exit(1)
print("pitch empty source: every pull returned a block of silence")
