# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""audiofreeverb: the traits the renders cannot check.

`tests/parity/freeverb_filter_probe.py` pins what this module renders across
interpreters once a filter is set. What is left for here is the empty-chain
identity and that each property is not a no-op.

## The traits, with their bars

| ID | Trait | Bar |
|---|---|---|
| V1 | No pre/post filter is a bit-exact identity with `pre_filter=None, post_filter=None` | exact, 0 LSB |
| V2 | `pre_filter` changes the reverb input | at least 1 sample differs |
| V3 | `post_filter` changes the reverb output | at least 1 sample differs |
"""

from array import array
import unittest

import audiocore
import audiofreeverb
import synthio

SAMPLE_RATE = 8000


def burst(frames=1600, level=14000):
    values = array("h")
    for frame in range(frames):
        if frame < 120:
            shape = ((frame * 97) % 2001) - 1000
            values.append(shape * level // 1000)
        else:
            values.append(0)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=1)


def words(node, blocks):
    out = []
    for _block in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        for position in range(0, len(data), 2):
            word = data[position] | (data[position + 1] << 8)
            out.append(word - 65536 if word >= 32768 else word)
    return out


def verb(**extra):
    return audiofreeverb.Freeverb(roomsize=0.5, damp=0.5, mix=0.7,
                                  sample_rate=SAMPLE_RATE, channel_count=1,
                                  buffer_size=512, **extra)


class EmptyChainTest(unittest.TestCase):
    """V1, and the controls that give V2 and V3 teeth."""

    def test_omitted_filters_match_explicit_none(self):
        """V1."""
        implicit = verb()
        explicit = verb(pre_filter=None, post_filter=None)
        implicit.play(burst())
        explicit.play(burst())
        for _block in range(6):
            self.assertEqual(bytes(audiocore.get_buffer(implicit)[1]),
                             bytes(audiocore.get_buffer(explicit)[1]))

    def test_V1_discriminates_a_pre_filter(self):
        """V2's control, and V1's planted fault."""
        bare = verb()
        filtered = verb(pre_filter=synthio.Biquad(
            synthio.FilterMode.HIGH_PASS, 200, 0.7))
        bare.play(burst())
        filtered.play(burst())
        differed = any(bytes(audiocore.get_buffer(bare)[1])
                       != bytes(audiocore.get_buffer(filtered)[1])
                       for _block in range(6))
        self.assertTrue(differed, "a high-pass pre_filter already matches "
                        "an empty chain, so V1 cannot fail")

    def test_a_pre_filter_changes_the_render(self):
        """V2."""
        bare_node = verb()
        bare_node.play(burst())
        bare = words(bare_node, 6)
        node = verb(pre_filter=synthio.Biquad(
            synthio.FilterMode.HIGH_PASS, 200, 0.7))
        node.play(burst())
        self.assertNotEqual(words(node, 6), bare)

    def test_V1_discriminates_a_post_filter(self):
        """V3's control."""
        bare = verb()
        filtered = verb(post_filter=synthio.Biquad(
            synthio.FilterMode.LOW_PASS, 2000, 0.7))
        bare.play(burst())
        filtered.play(burst())
        differed = any(bytes(audiocore.get_buffer(bare)[1])
                       != bytes(audiocore.get_buffer(filtered)[1])
                       for _block in range(6))
        self.assertTrue(differed, "a low-pass post_filter already matches "
                        "an empty chain, so V3 cannot fail")

    def test_a_post_filter_changes_the_render(self):
        """V3."""
        node = verb()
        node.play(burst())
        bare = words(node, 6)
        node = verb(post_filter=synthio.Biquad(
            synthio.FilterMode.LOW_PASS, 2000, 0.7))
        node.play(burst())
        self.assertNotEqual(words(node, 6), bare)


if __name__ == "__main__":
    unittest.main()
