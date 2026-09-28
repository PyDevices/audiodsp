"""Deterministic Tank PCM across the node's own state changes.

    tank_state_probe.py audioverb

`tank_probe.py` builds every network before anything plays and never moves
Tone through 0. This one does both, which is where the two 2026-09-28 fixes
live: the tilt's pole tracking while `tone_db` is 0 (audiodsp#168), and
`set(delays=..., taps=...)` re-cutting a playing node in place while it keeps
its source and the source frames it holds (audiodsp#169). Its own file, so
the probe above keeps rendering what it rendered.

No oracle: what `verify_dsp.py` checks is that every interpreter renders it
identically. Two lines are not checksums: `peak` is the largest sample after
Tone comes back out of exact silence (0 since #168), and `wire-exact` says a
re-cut at `mix=0` on a 1024-frame source left the output the source, byte
for byte (True since #169).
"""

import sys
from array import array

import audiocore
import audiofilters

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audioverb"
verb = __import__(MODULE)

SAMPLE_RATE = 8000
FRAMES = 256


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def noise(frames, channels, seed, level=11000):
    values = array("h")
    state = seed
    span = 2 * level + 1
    for _index in range(frames * channels):
        state = (state * 1103515245 + 12345) & 0x7fffffff
        values.append(((state >> 8) % span) - level)
    return values


def raw(values, channels):
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        print("tank-state", tag, index, len(data), sum(data), checksum(data))


OPTIONS = {"decay": 0.7, "diffusion": 0.7, "damping_hz": 2500.0,
           "bandwidth_hz": 3500.0, "mod_depth_ms": 0.3, "mod_rate_hz": 1.3,
           "mix": 0.5, "max_predelay_ms": 20.0, "predelay_ms": 5.0}
LINES = [16, 12, 40, 28, 70, 460, 190, 380, 95, 430, 275, 330]
TAPS = [0, 9, 27, 0.6, 0, 5, 200, -0.6, 0, 7, 100, 0.6,
        1, 5, 36, 0.6, 1, 11, 12, -0.6, 1, 10, 90, -0.6]


def scaled(factor):
    lines = [max(4, int(v * factor)) for v in LINES]
    taps = list(TAPS)
    for index in range(0, len(taps), 4):
        taps[index + 2] = min(int(taps[index + 2] * factor),
                              lines[int(taps[index + 1])] - 1)
    return lines, taps


def node(channels, **extra):
    options = dict(OPTIONS)
    options.update(extra)
    return verb.Tank(sample_rate=SAMPLE_RATE, channel_count=channels,
                     delays=LINES, taps=TAPS, **options)


# Tone out and back while the input plays (#168).
for channels in (2, 1):
    tank = node(channels, tone_db=12.0)
    tank.play(raw(noise(12 * FRAMES, channels, 17), channels))
    emit("tone-%d-in" % channels, tank, 2)
    tank.set(tone_db=0.0)
    emit("tone-%d-out" % channels, tank, 2)
    tank.set(tone_db=-6.0)
    emit("tone-%d-back" % channels, tank, 3)

# Tone back out of exact silence (#168): the peak after the move.
for channels in (2, 1):
    tank = node(channels, tone_db=12.0, decay=0.4)
    tank.play(raw(noise(4 * FRAMES, channels, 23), channels))
    for _block in range(3):
        audiocore.get_buffer(tank)
    tank.set(tone_db=0.0)
    audiocore.get_buffer(tank)
    tank.play(raw(array("h", bytes(2 * FRAMES * channels)), channels))
    quiet = 0
    for _block in range(400):
        quiet = quiet + 1 if not any(bytes(audiocore.get_buffer(tank)[1])) \
            else 0
        if quiet == 4:
            break
    tank.set(tone_db=12.0)
    top = 0
    for _block in range(2):
        for value in array("h", bytes(audiocore.get_buffer(tank)[1])):
            top = max(top, abs(value))
    print("tank-state peak", channels, quiet, top)

# Re-cut in place (#169): longer, shorter, the same size, and taps alone,
# with options in the same call. A RawSample handed whole, so the node holds
# the rest of it across every re-cut.
SAME = list(LINES)
SAME[5] -= 30
SAME[9] += 30
CUTS = (("longer", scaled(1.4)), ("shorter", scaled(0.3)),
        ("same", (SAME, list(TAPS))))
for channels in (2, 1):
    for name, (lines, taps) in CUTS:
        tank = node(channels, tone_db=3.0)
        tank.play(raw(noise(14 * FRAMES, channels, 29), channels))
        emit("recut-%d-%s-a" % (channels, name), tank, 2)
        tank.set(delays=lines, taps=taps, decay=0.5, mod_depth_ms=2.0)
        emit("recut-%d-%s-b" % (channels, name), tank, 4)
    tank = node(channels)
    tank.play(raw(noise(10 * FRAMES, channels, 31), channels))
    emit("recut-%d-taps-a" % channels, tank, 2)
    moved = list(TAPS)
    moved[2] = 5
    tank.set(taps=moved)
    emit("recut-%d-taps-b" % channels, tank, 3)

# The wire across a re-cut at mix 0 on a 1024-frame source (#169).
for channels in (2, 1):
    material = noise(10 * FRAMES, channels, 37)
    adapter = audiofilters.Filter(filter=None, mix=1,
                                  buffer_size=1024 * channels * 2,
                                  sample_rate=SAMPLE_RATE, bits_per_sample=16,
                                  samples_signed=True, channel_count=channels)
    adapter.play(raw(material, channels), loop=False)
    tank = node(channels, mix=0.0)
    tank.play(adapter)
    out = bytearray()
    for block in range(9):
        if block == 3:
            tank.set(delays=scaled(1.3)[0], taps=scaled(1.3)[1])
        out += bytes(audiocore.get_buffer(tank)[1])
    print("tank-state wire-exact", channels,
          bytes(out) == bytes(material)[:len(out)])
