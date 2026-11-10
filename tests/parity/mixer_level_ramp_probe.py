# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""A Mixer voice's level move walks in a straight line with `level_ramp` set.

    mixer_level_ramp_probe.py audiomixer

`Mixer.level_ramp` is this port's, not CircuitPython's (docs/how-it-works/upstream-diff.md,
"A level ramp on Mixer voices"). The native mixer walks a move one packed
word at a time and the CPython twin one int16 pair at a time; this prints
what each renders so `verify_dsp.py` can hold them to the same bytes. The
cases: a level move and a pan move, mono and stereo, a move made during a
walk, a walk through blocks shorter than it, a two-voice crossfade, and
`level_ramp` 0 beside a Mixer that never set it. CircuitPython's own Mixer has
no `level_ramp`, so `verify_dsp.py` doesn't run this there.
"""

import sys
from array import array

import audiocore

mixers = __import__(sys.argv[1] if len(sys.argv) > 1 else "audiomixer")

RATE = 48000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


FAILED = []


def check(tag, ok):
    print("check", tag, "ok" if ok else "FAILED")
    if not ok:
        FAILED.append(tag)


def material(frames, channels, offset=0):
    """A triangle a little off zero, so some moves wait for a stretch end and
    some land on a crossing, and every frame differs from its neighbours."""
    values = array("h")
    for frame in range(frames):
        phase = (frame * 37 + offset) % 800
        value = (phase if phase < 400 else 800 - phase) * 60 - 9000
        for channel in range(channels):
            values.append(value + 700 * channel)
    return values


def mixer(channels, voices, block):
    return mixers.Mixer(voice_count=voices, sample_rate=RATE,
                        channel_count=channels, bits_per_sample=16,
                        samples_signed=True,
                        buffer_size=block * channels * 2 * 2)


def render(channels, ramp, block, moves, voices=1, touch=True):
    """Pull 12 blocks; `moves` maps a block index to (voice, attr, value)."""
    node = mixer(channels, voices, block)
    if touch:
        node.level_ramp = ramp
    for v in range(voices):
        node.voice[v].level = 1.0 if v == 0 else 0.0
        node.voice[v].play(audiocore.RawSample(
            material(2400, channels, 211 * v), sample_rate=RATE,
            channel_count=channels), loop=True)
    out = []
    for index in range(12):
        for voice, attr, value in moves.get(index, ()):
            setattr(node.voice[voice], attr, value)
        out.append(bytes(audiocore.get_buffer(node)[1]))
    node.deinit()
    return out


CASES = (
    ("level", {3: ((0, "level", 0.25),)}),
    ("level-up", {3: ((0, "level", 0.0),), 7: ((0, "level", 0.8),)}),
    ("during", {3: ((0, "level", 0.0),), 4: ((0, "level", 0.6),)}),
    ("pan", {3: ((0, "panning", -1.0),), 6: ((0, "panning", 0.5),)}),
)

for channels in (1, 2):
    for block in (256, 100):
        for ramp in (0, 960, 37):
            for name, moves in CASES:
                if name == "pan" and channels == 1:
                    continue
                tag = "%s-%dch-b%d-r%d" % (name, channels, block, ramp)
                print("mixer-ramp", tag,
                      [checksum(b) for b in render(channels, ramp, block,
                                                   moves)])
        tag = "crossfade-%dch-b%d" % (channels, block)
        fade = {3: ((0, "level", 0.0), (1, "level", 1.0))}
        print("mixer-ramp", tag,
              [checksum(b) for b in render(channels, 960, block, fade, 2)])
        # level_ramp 0 is CircuitPython's mixer: the same bytes as a Mixer
        # that never touched the property.
        moves = CASES[0][1]
        check("off-%dch-b%d" % (channels, block),
              render(channels, 0, block, moves)
              == render(channels, 0, block, moves, touch=False))

print("done mixer-ramp")
if FAILED:
    raise SystemExit("failed: " + " ".join(FAILED))
