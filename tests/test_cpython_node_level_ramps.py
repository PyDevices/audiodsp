# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""A node's level control walks to a new value over 20 ms instead of stepping.

The Waveshaper's `mix` and `post_gain`, the Biquad's and the AllPass's `mix`,
the Dynamics node's makeup and the FeedbackDelay's `mix` each scale a signal
the node has already worked out. Moved on a playing node, each walks there in
a straight line over 20 ms (`audiodsp_level_ramp.h`), so the move cannot click.
None of these controls feeds the node's own state, so the walked output is
exactly the blend of two nodes that never moved: one held at the old value,
one at the new. That is what this holds them to.

| what it checks | bar |
|---|---|
| a move walks in a straight line and lands | for each control, mono and stereo, at 48 and 22.05 kHz: up to the move, the bytes of a node that stayed at the old value; for 20 ms after it, that node's frame plus `(i + 1) / n` of the way to a node built at the new value, to within 2 LSB; from then on, that node's bytes exactly |
| a control set before the first pull is not walked | a node built at the old value and moved before anything pulled it renders the bytes of one built at the new value |
| a reset clears the walk | a node reset halfway through a walk renders, from the reset on, the bytes of a node built at the new value and reset at the same pull |

The C in `src/shared` is the authority; this target runs the same C, so the
native builds are held to the same bytes by
`tests/parity/node_level_ramp_probe.py`.

    python -m unittest tests.test_cpython_node_level_ramps
"""

import math
import unittest
from array import array

import audiobiquad
import audiocore
import audiodynamics
import audioecho
import audioshaper

MOVE_AT = 4
PULLS = 16


def clip_curve(threshold=0.25, points=1025):
    """A hard clip at a quarter of full scale, so the wet path is far from
    the dry one."""
    last = points - 1
    return array("h", [
        int(round(max(-1.0, min(1.0, (-1.0 + 2.0 * i / last) / threshold))
                  * 32767))
        for i in range(points)])


CLIP = clip_curve()


def tone(rate, channels):
    """A looped 200 Hz sine, the right channel quieter than the left."""
    period = int(rate / 200.0) * 10
    values = array("h")
    for i in range(period):
        v = 12000 * math.sin(2 * math.pi * 200.0 * i / rate)
        values.append(int(v))
        if channels == 2:
            values.append(int(0.7 * v))
    return audiocore.RawSample(values, sample_rate=rate,
                               channel_count=channels)


def shaper(rate, channels, **options):
    return audioshaper.Waveshaper(sample_rate=rate, curve=CLIP, oversample=1,
                                  channel_count=channels, **options)


def biquad(rate, channels, mix):
    return audiobiquad.Biquad(mode=audiobiquad.HIGH_PASS, frequency=400.0,
                              mix=mix, sample_rate=rate,
                              channel_count=channels)


def allpass(rate, channels, mix):
    return audiobiquad.AllPass(stages=4, frequency=300.0, mix=mix,
                               sample_rate=rate, channel_count=channels)


def dynamics(rate, channels, makeup_db):
    return audiodynamics.Dynamics(audiodynamics.DYN_COMPRESS,
                                  sample_rate=rate, channel_count=channels,
                                  threshold_db=-24, ratio=4,
                                  makeup_db=makeup_db)


def delay(rate, channels, mix):
    return audioecho.FeedbackDelay(sample_rate=rate, max_delay_ms=20,
                                   channel_count=channels, delay_ms=3.0,
                                   feedback=0.3, mix=mix)


def _set(name):
    def move(node, value):
        node.set(**{name: value})
    return move


def _attr(name):
    def move(node, value):
        setattr(node, name, value)
    return move


#: name, build(rate, channels, value), move(node, value), old, new
CASES = (
    ("shaper mix", lambda r, c, v: shaper(r, c, mix=v), _set("mix"),
     1.0, 0.2),
    ("shaper post_gain", lambda r, c, v: shaper(r, c, post_gain=v),
     _set("post_gain"), 0.9, 0.3),
    ("biquad mix", biquad, _attr("mix"), 1.0, 0.0),
    ("allpass mix", allpass, _attr("mix"), 0.5, 0.0),
    ("dynamics makeup", dynamics, _set("makeup_db"), 0.0, 9.0),
    ("delay mix", delay, _set("mix"), 0.0, 1.0),
)


def render(node, rate, channels, moves=None, reset_at=None):
    """PULLS blocks; `moves` is (pull index, callable). Returns the frames,
    interleaved, and how many frames came before each pull."""
    node.play(tone(rate, channels), loop=True)
    out = array("h")
    starts = []
    for index in range(PULLS):
        if moves is not None and index == moves[0]:
            moves[1](node)
        if index == reset_at:
            audiocore.reset_buffer(node)
        starts.append(len(out) // channels)
        out.extend(array("h", bytes(audiocore.get_buffer(node)[1])))
    node.deinit()
    return out, starts


class NodeLevelRamps(unittest.TestCase):

    def _cases(self):
        for name, build, move, old, new in CASES:
            for rate in (48000, 22050):
                for channels in (1, 2):
                    yield name, build, move, old, new, rate, channels

    def test_a_move_walks_and_lands(self):
        for name, build, move, old, new, rate, channels in self._cases():
            with self.subTest(name=name, rate=rate, channels=channels):
                held, _ = render(build(rate, channels, old), rate, channels)
                there, _ = render(build(rate, channels, new), rate, channels)
                self.assertNotEqual(held, there)
                walked, starts = render(
                    build(rate, channels, old), rate, channels,
                    (MOVE_AT, lambda node: move(node, new)))
                n = (rate * 20 + 500) // 1000
                at = starts[MOVE_AT]
                self.assertGreater(len(walked), (at + n) * channels)
                self.assertEqual(walked[:at * channels],
                                 held[:at * channels])
                worst = 0
                for i in range(n):
                    t = (i + 1) / n
                    for c in range(channels):
                        k = (at + i) * channels + c
                        want = held[k] + (there[k] - held[k]) * t
                        worst = max(worst, abs(walked[k] - want))
                self.assertLessEqual(worst, 2)
                self.assertEqual(walked[(at + n) * channels:],
                                 there[(at + n) * channels:])

    def test_a_move_before_the_first_pull_lands(self):
        for name, build, move, old, new, rate, channels in self._cases():
            with self.subTest(name=name, rate=rate, channels=channels):
                there, _ = render(build(rate, channels, new), rate, channels)
                moved, _ = render(build(rate, channels, old), rate, channels,
                                  (0, lambda node: move(node, new)))
                self.assertEqual(moved, there)

    def test_a_reset_clears_the_walk(self):
        for name, build, move, old, new, rate, channels in self._cases():
            with self.subTest(name=name, rate=rate, channels=channels):
                there, starts = render(build(rate, channels, new), rate,
                                       channels, reset_at=MOVE_AT + 1)
                walked, _ = render(build(rate, channels, old), rate,
                                   channels,
                                   (MOVE_AT, lambda node: move(node, new)),
                                   reset_at=MOVE_AT + 1)
                at = starts[MOVE_AT + 1] * channels
                self.assertNotEqual(walked[:at], there[:at])
                self.assertEqual(walked[at:], there[at:])


if __name__ == "__main__":
    unittest.main()
