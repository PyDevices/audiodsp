"""audiomath.SubOctave: that it is actually an octave down, and the surface.

What the node *renders* is pinned byte-for-byte across the three interpreters
by tests/parity/suboctave_probe.py through verify_dsp.py. A digest says the
builds agree; it does not say the thing is a divider. What is here is the
claim itself -- a tone in, its own frequency halved coming out -- plus the
errors and the source behaviours only a Python-defined audiosample produces.
"""

import math
import unittest
from array import array

import audiocore
import audiomath

SAMPLE_RATE = 8000


def tone(frequency, seconds=0.5, level=20000, channel_count=2):
    values = array("h")
    for frame in range(int(SAMPLE_RATE * seconds)):
        value = int(level * math.sin(2 * math.pi * frequency * frame
                                     / SAMPLE_RATE))
        for _ in range(channel_count):
            values.append(value)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channel_count)


def render(node, blocks, channel_count=2):
    values = array("h")
    for _ in range(blocks):
        values.frombytes(bytes(audiocore.get_buffer(node)[1]))
    return values[::channel_count]


def magnitude(samples, frequency):
    """Goertzel, so a tolerance-free comparison of one bin against another
    needs no numpy in the wheel's own test suite."""
    real = 0.0
    imaginary = 0.0
    for index, value in enumerate(samples):
        angle = 2.0 * math.pi * frequency * index / SAMPLE_RATE
        real += value * math.cos(angle)
        imaginary -= value * math.sin(angle)
    return math.hypot(real, imaginary) / len(samples)


class OctaveTest(unittest.TestCase):
    def test_order_one_halves_the_frequency(self):
        node = audiomath.SubOctave(tone(400.0), order=1, mix=1.0,
                                   sample_rate=SAMPLE_RATE)
        rendered = render(node, 8)[256:1856]
        self.assertGreater(magnitude(rendered, 200.0),
                           20 * magnitude(rendered, 400.0))

    def test_order_two_quarters_it(self):
        node = audiomath.SubOctave(tone(400.0), order=2, mix=1.0,
                                   sample_rate=SAMPLE_RATE)
        rendered = render(node, 8)[256:1856]
        self.assertGreater(magnitude(rendered, 100.0),
                           10 * magnitude(rendered, 400.0))
        self.assertGreater(magnitude(rendered, 100.0),
                           10 * magnitude(rendered, 200.0))

    def test_it_tracks_a_different_note(self):
        node = audiomath.SubOctave(tone(300.0), order=1, mix=1.0,
                                   sample_rate=SAMPLE_RATE)
        rendered = render(node, 8)[256:1856]
        self.assertGreater(magnitude(rendered, 150.0),
                           20 * magnitude(rendered, 300.0))

    def test_a_mix_of_zero_is_a_bypass(self):
        values = array("h")
        for frame in range(1024):
            value = int(20000 * math.sin(2 * math.pi * 400.0 * frame
                                         / SAMPLE_RATE))
            values.append(value)
            values.append(value)
        source = audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                                     channel_count=2)
        node = audiomath.SubOctave(source, order=1, mix=0.0,
                                   sample_rate=SAMPLE_RATE)
        rendered = array("h")
        for _ in range(4):
            rendered.frombytes(bytes(audiocore.get_buffer(node)[1]))
        self.assertEqual(list(rendered), list(values))

    def test_a_half_mix_cancels_on_the_inverted_half(self):
        # A signal blended 50/50 with its own inverse is exactly zero, which
        # is the arithmetic the Q15 blend has to get right rather than
        # approximately right.
        node = audiomath.SubOctave(tone(50.0), order=1, mix=0.5,
                                   sample_rate=SAMPLE_RATE)
        rendered = render(node, 8)[256:1856]
        self.assertIn(0, rendered)
        self.assertNotEqual(min(rendered), max(rendered))

    def test_it_is_zero_latency(self):
        # Every output sample is the input sample at the same index, give or
        # take its sign: nothing is buffered ahead and nothing is
        # interpolated, which is the whole difference from a granular
        # shifter. A -32768 input is the one exception, since negating it
        # clamps to 32767.
        values = array("h")
        for frame in range(1024):
            value = int(20000 * math.sin(2 * math.pi * 400.0 * frame
                                         / SAMPLE_RATE))
            values.append(value)
            values.append(value)
        source = audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                                     channel_count=2)
        node = audiomath.SubOctave(source, order=1, mix=1.0,
                                   sample_rate=SAMPLE_RATE)
        rendered = render(node, 4)
        original = values[::2]
        self.assertEqual([abs(x) for x in rendered],
                         [abs(x) for x in original])
        # and it really did invert somewhere, or the claim above is vacuous
        self.assertNotEqual(list(rendered), list(original))


class SurfaceTest(unittest.TestCase):
    def test_it_presents_itself_as_a_stereo_sample(self):
        node = audiomath.SubOctave(sample_rate=SAMPLE_RATE)
        self.assertEqual(node.sample_rate, SAMPLE_RATE)
        self.assertEqual(node.channel_count, 2)
        self.assertEqual(node.bits_per_sample, 16)
        self.assertTrue(node.samples_signed)

    def test_mono_is_accepted(self):
        node = audiomath.SubOctave(tone(400.0, channel_count=1), order=1,
                                   mix=1.0, channel_count=1,
                                   sample_rate=SAMPLE_RATE)
        rendered = render(node, 8, channel_count=1)[256:1856]
        self.assertGreater(magnitude(rendered, 200.0),
                           20 * magnitude(rendered, 400.0))

    def test_an_unknown_option_is_refused(self):
        node = audiomath.SubOctave(sample_rate=SAMPLE_RATE)
        with self.assertRaises(TypeError):
            node.set(depth=0.5)

    def test_a_bad_channel_count_is_refused(self):
        with self.assertRaises(ValueError):
            audiomath.SubOctave(channel_count=3)

    def test_silence_in_is_silence_out(self):
        source = audiocore.RawSample(array("h", [0] * 2048),
                                     sample_rate=SAMPLE_RATE,
                                     channel_count=2)
        node = audiomath.SubOctave(source, order=2, mix=1.0,
                                   sample_rate=SAMPLE_RATE)
        self.assertEqual(set(render(node, 4)), {0})

    def test_a_starved_node_yields_silence_not_an_end(self):
        node = audiomath.SubOctave()
        result, buffer = audiocore.get_buffer(node)
        self.assertEqual(result, 1)
        self.assertEqual(len(buffer), 256 * 2 * 2)
        self.assertEqual(set(bytes(buffer)), {0})

    def test_stop_and_play_are_symmetric(self):
        node = audiomath.SubOctave(tone(400.0), order=1, mix=1.0,
                                   sample_rate=SAMPLE_RATE)
        self.assertTrue(node.playing)
        node.stop()
        self.assertFalse(node.playing)
        node.play(tone(400.0))
        self.assertTrue(node.playing)

    def test_set_does_not_restart_the_count_but_clear_does(self):
        first = audiomath.SubOctave(tone(400.0), order=1, mix=1.0,
                                    sample_rate=SAMPLE_RATE)
        render(first, 4)
        before = list(render(first, 1))
        second = audiomath.SubOctave(tone(400.0), order=1, mix=1.0,
                                     sample_rate=SAMPLE_RATE)
        render(second, 4)
        second.set(threshold=0.02)
        self.assertEqual(list(render(second, 1)), before)
        third = audiomath.SubOctave(tone(400.0), order=1, mix=1.0,
                                    sample_rate=SAMPLE_RATE)
        render(third, 4)
        third.clear()
        self.assertNotEqual(list(render(third, 1)), before)


if __name__ == "__main__":
    unittest.main()


class UniversalTraitTest(unittest.TestCase):
    """M11 - the identity trait, at the rails.

    There is no `clear()` trait here on purpose. `test_set_does_not_restart_the_count_but_clear_does`
    above already has it, and has it *better*: it clears mid-stream on a
    continuing tone, where the divider's flip-flop shows in the sub-octave's
    phase. A version that replays a source instead cannot fail - measured, a
    cleared and an uncleared SubOctave render identically that way - so adding
    one would have been a second, weaker test of something already covered.
    """

    RATE = 8000
    CHANNELS = 2

    def _alternating(self, frames=4096, level=32767):
        values = array("h")
        for frame in range(frames):
            for _channel in range(self.CHANNELS):
                values.append(level if frame % 2 else -level)
        return audiocore.RawSample(values, sample_rate=self.RATE,
                                   channel_count=self.CHANNELS)

    def _alternating_words(self, count, level=32767):
        return [level if (index // self.CHANNELS) % 2 else -level
                for index in range(count)]

    def _silence(self, frames=4096):
        return audiocore.RawSample(
            array("h", bytes(frames * self.CHANNELS * 2)),
            sample_rate=self.RATE, channel_count=self.CHANNELS)

    def _words(self, node, blocks):
        out = []
        for _block in range(blocks):
            data = bytes(audiocore.get_buffer(node)[1])
            for position in range(0, len(data), 2):
                word = data[position] | (data[position + 1] << 8)
                out.append(word - 65536 if word >= 32768 else word)
        return out

    def test_the_neutral_setting_is_exact_at_the_rails(self):
        """M11, this module's form of the identity trait. `mix=0` is already
        covered elsewhere in this file on ordinary material; what is new here is
        **at +/-32767**, which is where an arithmetic width error shows and a
        range check does not. Measured 0 LSB."""
        node = audiomath.SubOctave(sample_rate=self.RATE,
                                   channel_count=self.CHANNELS, mix=0.0)
        node.play(self._alternating())
        rendered = self._words(node, 6)
        self.assertEqual(rendered, self._alternating_words(len(rendered)))

    def test_the_identity_trait_discriminates(self):
        """Its control: the same node wet must not satisfy it."""
        node = audiomath.SubOctave(sample_rate=self.RATE,
                                  channel_count=self.CHANNELS, mix=1.0)
        node.play(self._alternating())
        rendered = self._words(node, 6)
        self.assertNotEqual(rendered, self._alternating_words(len(rendered)))


class RemixS16Test(unittest.TestCase):
    def test_stereo_to_mono_averages(self):
        src = bytes((100, 0, 50, 0, 10, 0, 0, 0))
        out = audiomath.remix_s16(src, 2, 1)
        self.assertEqual(bytes((75, 0, 5, 0)), bytes(out))

    def test_mono_to_stereo_duplicates(self):
        src = bytes((7, 0, 8, 0))
        out = audiomath.remix_s16(src, 1, 2)
        self.assertEqual(bytes((7, 0, 7, 0, 8, 0, 8, 0)), bytes(out))

    def test_same_channels_copy_into_dest(self):
        src = bytes((1, 0, 2, 0))
        dest = bytearray(4)
        returned = audiomath.remix_s16(src, 1, 1, dest)
        self.assertIs(returned, dest)
        self.assertEqual(src, bytes(dest))

    def test_mono_to_stereo_expands_in_place(self):
        # The push path's reason for `dest`: one scratch buffer, mono in the
        # front of it, stereo across the whole of it.
        buf = bytearray(16)
        buf[0:8] = bytes((1, 0, 2, 0, 3, 0, 4, 0))
        audiomath.remix_s16(memoryview(buf)[:8], 1, 2, buf)
        self.assertEqual(
            bytes((1, 0, 1, 0, 2, 0, 2, 0, 3, 0, 3, 0, 4, 0, 4, 0)), bytes(buf)
        )

    def test_stereo_to_mono_contracts_in_place(self):
        buf = bytearray(bytes((100, 0, 50, 0, 10, 0, 0, 0)))
        audiomath.remix_s16(memoryview(buf), 2, 1, buf)
        self.assertEqual(bytes((75, 0, 5, 0)), bytes(buf)[:4])

    def test_rejects_partial_frames(self):
        with self.assertRaises(ValueError):
            audiomath.remix_s16(bytes((1, 0, 2)), 2, 1)


if __name__ == "__main__":
    unittest.main()

