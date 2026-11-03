# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""`audiospeed.Resampler`: the ratio is bound by whoever plays it.

CircuitPython 10.3.0 added the node. The thing worth testing is not the
resampling arithmetic - `tests/parity/resampler_probe.py` pins that, on both
interpreters, against a golden - but the **binding**, because it is the one
piece of machinery in the palette that reaches back up the graph:
`audiosample_must_match` exempts a Resampler from the sample-rate check that
refuses every other node, and then hands it the destination's rate. One call
site, and every node's `play()` goes through it.

Both halves matter and each is a fault for the other. Without the exemption a
Resampler could never be played at all, which is the whole point of one.
Without the exemption being **narrow**, every genuine rate mismatch in the
palette would be silently accepted - so the first test here is that a plain
sample at the wrong rate is still refused.
"""

from array import array
import unittest

import audiocore
import audiofilters
import audiospeed

FRAMES = 512


def source(rate, level=0):
    return audiocore.RawSample(array("h", [level] * (FRAMES * 2)),
                               sample_rate=rate, channel_count=2)


def destination(rate):
    return audiofilters.Filter(sample_rate=rate, channel_count=2,
                               buffer_size=FRAMES)


class TheRateIsBoundByTheDestination(unittest.TestCase):

    def test_a_plain_sample_at_the_wrong_rate_is_still_refused(self):
        """The exemption's own planted fault: if it were not narrow, this
        would pass and every rate mismatch in the palette would be silent."""
        with self.assertRaises(ValueError):
            destination(8000).play(source(16000))

    def test_a_resampler_is_accepted_at_a_rate_nothing_else_would_be(self):
        node = audiospeed.Resampler(source(16000))
        destination(8000).play(node)          # must not raise
        self.assertEqual(node.rate, 2.0)

    def test_the_ratio_is_one_until_something_plays_it(self):
        self.assertEqual(audiospeed.Resampler(source(16000)).rate, 1.0)

    def test_source_over_destination_in_both_directions(self):
        for source_rate, dest_rate, wanted in ((16000, 8000, 2.0),
                                               (8000, 8000, 1.0),
                                               (4000, 8000, 0.5)):
            with self.subTest(source=source_rate, destination=dest_rate):
                node = audiospeed.Resampler(source(source_rate))
                destination(dest_rate).play(node)
                self.assertEqual(node.rate, wanted)

    def test_being_replayed_rebinds_the_ratio(self):
        """A node moved to another destination must take that one's rate, not
        keep the first one's."""
        node = audiospeed.Resampler(source(16000))
        destination(8000).play(node)
        self.assertEqual(node.rate, 2.0)
        destination(16000).play(node)
        self.assertEqual(node.rate, 1.0)

    def test_the_rate_is_read_only(self):
        """Unlike `SpeedChanger.rate`. The ratio is not the caller's to set:
        it is whatever the destination asked for, and a setter would be a
        second source of truth that the next `play()` silently overwrites."""
        node = audiospeed.Resampler(source(16000))
        with self.assertRaises(AttributeError):
            node.rate = 3.0

    def test_it_releases_like_every_other_node(self):
        node = audiospeed.Resampler(source(16000))
        node.deinit()
        node.deinit()                          # idempotent
        with self.assertRaises(ValueError):
            audiocore.get_buffer(node)

    def test_it_is_a_context_manager(self):
        with audiospeed.Resampler(source(16000)) as node:
            self.assertEqual(node.rate, 1.0)
        with self.assertRaises(ValueError):
            audiocore.get_buffer(node)


if __name__ == "__main__":
    unittest.main()
