"""A block where nothing sounds is silence, and costs next to nothing.

The CPython synthio twin renders in Python, and under Pyodide that is the
cost that matters: a synthesiser kept running between notes still renders a
block every 256 frames. `_render` used to build a zeroed accumulator and mix
it down even when no voice contributed. It now skips both and hands back
silence directly.

The shortcut is only allowed if it changes nothing a listener or a golden can
see, so these tests pin both halves. The bytes must equal what mixing down
zeros produces (the old path, computed here with the same native call), and
the mix-down must not run on a silent block while it still runs on a sounding
one. Without that second check a regression that mixed down every block
would pass the first. audiodsp#278.
"""

import array
import unittest
from unittest import mock

import audiocore
import synthio

_ENVELOPE = synthio.Envelope(
    attack_time=0.0, decay_time=0.0, release_time=0.0,
    attack_level=1.0, sustain_level=1.0)


def _block(synth, **kwargs):
    return bytes(synth._render(256, **kwargs))


def _mixed_down_zeros(synth, channels):
    """What the old path returned for a silent block."""
    zeros = array.array("i", bytes(4 * 256 * channels))
    return synthio._audiodsp.mixdown_i32(zeros, synth.max_polyphony)


class SilentRenderTest(unittest.TestCase):
    def _count_mixdowns(self):
        real = synthio._audiodsp.mixdown_i32
        spy = mock.Mock(side_effect=real)
        return mock.patch.object(synthio._audiodsp, "mixdown_i32", spy), spy

    def test_idle_synth_is_silence_without_a_mixdown(self):
        for channels in (1, 2):
            synth = synthio.Synthesizer(sample_rate=48000, channel_count=channels)
            patch, spy = self._count_mixdowns()
            with patch:
                blocks = [_block(synth) for _ in range(8)]
            expected = _mixed_down_zeros(synth, channels)
            self.assertEqual(len(expected), 256 * channels * 2)
            for block in blocks:
                self.assertEqual(block, expected)
            self.assertEqual(spy.call_count, 0, "silent block took the slow path")

    def test_single_channel_output_matches(self):
        synth = synthio.Synthesizer(sample_rate=48000, channel_count=2)
        self.assertEqual(_block(synth, single_channel_output=True),
                         _mixed_down_zeros(synth, 1))

    def test_finished_note_falls_back_to_the_fast_path(self):
        synth = synthio.Synthesizer(
            sample_rate=48000, channel_count=1, envelope=_ENVELOPE)
        synth.press(synthio.Note(
            frequency=110.0, waveform=array.array("h", [32767] * 64)))
        patch, spy = self._count_mixdowns()
        with patch:
            # A voice's first block is gated until its waveform crosses zero,
            # and a constant waveform never does, so listen to the second.
            _block(synth)
            sounding = _block(synth)
        self.assertTrue(any(sounding), "setup: the note should sound")
        self.assertEqual(spy.call_count, 2, "a sounding block must still mix down")

        synth.release_all()
        # The release is instant, so the envelope reaches 0 within a block or
        # two. Run until the voice is collected, then insist on silence.
        for _ in range(4):
            _block(synth)
        self.assertEqual(synth._notes, [])
        patch, spy = self._count_mixdowns()
        with patch:
            after = _block(synth)
        self.assertEqual(after, _mixed_down_zeros(synth, 1))
        self.assertEqual(spy.call_count, 0)

    def test_blocks_still_advance_on_a_silent_block(self):
        """The shortcut skips voices, not the clock that LFOs read."""
        synth = synthio.Synthesizer(sample_rate=48000)
        before = synthio._CLOCK.tick
        _block(synth)
        self.assertEqual(synthio._CLOCK.tick, before + 1)


if __name__ == "__main__":
    unittest.main()
