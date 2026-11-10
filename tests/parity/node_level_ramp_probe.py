# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Node level ramps and `Port.play(after=)`, the same bytes on every target.

    node_level_ramp_probe.py audioshaper

    RAMP  <case>  <checksum of each pull>
    PORT  <case>  <first value of each pull>

The Waveshaper's `mix` and `post_gain`, the Biquad's and the AllPass's `mix`,
the Dynamics node's makeup and the FeedbackDelay's `mix` walk a move over
20 ms (`src/shared/audiodsp_level_ramp.h`). Each is moved on a playing node,
moved before the first pull, and moved and then reset halfway through the
walk, mono and stereo, at 48 and 22.05 kHz. A Port told to switch `after`
some frames switches at the start of the next pull once they have played,
and at once when nothing has pulled it since it was pointed at what it plays;
pointing it at that same source again is not a new start. The nodes and the
Port are this package's own, and every target runs the same C for them.
"""

from array import array

import audiobiquad
import audiocore
import audiodynamics
import audioecho
import audioroute
import audioshaper

PULLS = 12
MOVE_AT = 3


def checksum(data):
    value = 2166136261
    for byte in bytes(data):
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def clip_curve(points=1025):
    last = points - 1
    out = array("h")
    for i in range(points):
        x = (-1.0 + 2.0 * i / last) * 4.0
        out.append(int(round(max(-1.0, min(1.0, x)) * 32767)))
    return out


CLIP = clip_curve()


def tone(rate, channels):
    """A 200 Hz triangle in whole cycles, worked in integers so a
    single-precision build plays the same input, and longer than PULLS
    pulls of 256 frames: a node's play() takes no loop on MicroPython."""
    cycle = rate // 200
    values = array("h")
    for i in range(cycle * 30):
        t = 48000 * (i % cycle) // cycle
        v = t if t < 12000 else (24000 - t if t < 36000 else t - 48000)
        values.append(v)
        if channels == 2:
            values.append(v * 7 // 10)
    return audiocore.RawSample(values, sample_rate=rate,
                               channel_count=channels)


def shaper(rate, channels, name, value):
    options = {name: value}
    return audioshaper.Waveshaper(sample_rate=rate, curve=CLIP, oversample=1,
                                  channel_count=channels, **options)


def biquad(rate, channels, name, value):
    return audiobiquad.Biquad(mode=audiobiquad.HIGH_PASS, frequency=400.0,
                              mix=value, sample_rate=rate,
                              channel_count=channels)


def allpass(rate, channels, name, value):
    return audiobiquad.AllPass(stages=4, frequency=300.0, mix=value,
                               sample_rate=rate, channel_count=channels)


def dynamics(rate, channels, name, value):
    return audiodynamics.Dynamics(audiodynamics.DYN_COMPRESS,
                                  sample_rate=rate, channel_count=channels,
                                  threshold_db=-24, ratio=4, makeup_db=value)


def delay(rate, channels, name, value):
    return audioecho.FeedbackDelay(sample_rate=rate, max_delay_ms=20,
                                   channel_count=channels, delay_ms=3.0,
                                   feedback=0.3, mix=value)


def by_set(node, name, value):
    options = {name: value}
    node.set(**options)


def by_attr(node, name, value):
    setattr(node, name, value)


CASES = (
    ("shaper-mix", shaper, by_set, "mix", 1.0, 0.2),
    ("shaper-post", shaper, by_set, "post_gain", 0.9, 0.3),
    ("biquad-mix", biquad, by_attr, "mix", 1.0, 0.0),
    ("allpass-mix", allpass, by_attr, "mix", 0.5, 0.0),
    ("dynamics-makeup", dynamics, by_set, "makeup_db", 0.0, 9.0),
    ("delay-mix", delay, by_set, "mix", 0.0, 1.0),
)


def render(node, rate, channels, move_at, move, reset_at=None):
    node.play(tone(rate, channels))
    sums = []
    for index in range(PULLS):
        if index == move_at:
            move(node)
        if index == reset_at:
            audiocore.reset_buffer(node)
        sums.append(checksum(audiocore.get_buffer(node)[1]))
    node.deinit()
    return sums


for name, build, mover, control, old, new in CASES:
    for rate in (48000, 22050):
        for channels in (1, 2):
            def move(node):
                mover(node, control, new)
            for tag, move_at, reset_at in (("move", MOVE_AT, None),
                                           ("first", 0, None),
                                           ("reset", MOVE_AT, MOVE_AT + 1)):
                print("RAMP", "%s-%s-%d-%dch" % (name, tag, rate, channels),
                      render(build(rate, channels, control, old), rate,
                             channels, move_at, move, reset_at))


def constant(value, frames=256):
    return audiocore.RawSample(array("h", [value] * frames),
                               sample_rate=48000, channel_count=1)


def firsts(port, pulls):
    out = []
    for _ in range(pulls):
        out.append(array("h", bytes(audiocore.get_buffer(port)[1]))[0])
    return out


A, B, C = constant(1000), constant(2000), constant(3000)

port = audioroute.Port(A)
seen = firsts(port, 1)
port.play(B, after=300)
print("PORT", "after-pulled", seen + firsts(port, 4))

port = audioroute.Port(A)
port.play(B, after=300)
print("PORT", "after-unpulled", firsts(port, 3))

port = audioroute.Port(A)
seen = firsts(port, 1)
port.play(B, after=300)
seen += firsts(port, 3)
port.play(C, after=300)
print("PORT", "after-switched", seen + firsts(port, 4))

port = audioroute.Port(A)
seen = firsts(port, 1)
port.play(B, after=600)
seen += firsts(port, 1)
port.play(C, after=300)
print("PORT", "after-replaced", seen + firsts(port, 4))

port = audioroute.Port(A)
seen = firsts(port, 1)
port.play(B, after=256)
print("PORT", "after-exact", seen + firsts(port, 3))

port = audioroute.Port(A)
seen = firsts(port, 1)
port.play(A)
port.play(B, after=300)
print("PORT", "after-same-again", seen + firsts(port, 4))

try:
    audioroute.Port(A).play(B, after=-1)
    print("PORT", "after-negative", "accepted")
except ValueError:
    print("PORT", "after-negative", "ValueError")

print("done node-level-ramp")
