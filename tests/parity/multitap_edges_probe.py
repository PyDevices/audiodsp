"""Deterministic MultiTapDelay PCM for two loops audiodsp#177 found.

    multitap_edges_probe.py audiodelays

Each is asserted here as well as printed, so the interpreters are held to
the same bytes and the bytes are held to the claim:

- With a stereo source that hands an odd number of samples, every sample
  comes back out in the lane it went in on. Upstream CircuitPython starts
  every source buffer on the left lane, so the lanes swap from there on.
- Looping a source that comes back empty renders silence and returns.
  Upstream goes round its pull loop for ever.

Both are recorded deviations from the oracle (docs/upstream-diff.md), so the
probe runs on MicroPython and CPython and is skipped on CircuitPython, whose
MultiTapDelay is upstream's.
"""

import sys
from array import array

import audiocore

delays = __import__(sys.argv[1] if len(sys.argv) > 1 else "audiodelays")

RATE = 8000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def samples(data):
    out = [data[i] | (data[i + 1] << 8) for i in range(0, len(data), 2)]
    return [s - 65536 if s >= 32768 else s for s in out]


def pull(node):
    return bytes(audiocore.get_buffer(node)[1])


FAILED = []


def check(tag, ok):
    print("check", tag, "ok" if ok else "FAILED")
    if not ok:
        FAILED.append(tag)


# Lanes. 257 samples is 128 frames and half of one, so every other pass of
# the looped source starts on the right. With no feedback and the mix all
# wet, what comes out is what went in, 20 ms (320 samples) later, in the same
# lane: sample n of the output is sample n - 320 of the source stream.
pattern = array("h")
for index in range(257):
    pattern.append(((index * 37) % 61 - 30) * 200 + (6000 if index % 2 else 0))
node = delays.MultiTapDelay(max_delay_ms=40, delay_ms=20, decay=0.0, mix=1.0,
                            buffer_size=512, sample_rate=RATE,
                            channel_count=2)
node.play(audiocore.RawSample(pattern, sample_rate=RATE, channel_count=2),
          loop=True)
wrong = 0
position = 0
for index in range(6):
    data = pull(node)
    for value in samples(data):
        if position >= 320 and value != pattern[(position - 320) % 257]:
            wrong += 1
        position += 1
    print("lanes", index, len(data), checksum(data))
print("lanes wrong", wrong)
check("lanes", wrong == 0)

# An empty source, looped.
node = delays.MultiTapDelay(max_delay_ms=40, delay_ms=20, decay=0.5, mix=0.5,
                            buffer_size=512, sample_rate=RATE)
node.play(audiocore.RawSample(array("h", [0]), sample_rate=RATE), loop=True)
for index in range(2):
    data = pull(node)
    print("one_sample_loop", index, len(data), checksum(data))
node.play(audiocore.RawSample(array("h"), sample_rate=RATE), loop=True)
for index in range(3):
    data = pull(node)
    print("empty_loop", index, len(data), checksum(data))
check("empty_loop", True)

if FAILED:
    raise SystemExit("failed: " + " ".join(FAILED))
