"""The pitch shifters with a source that says it has more and hands nothing.

`PitchShift` and `GranularPitchShift` render the rest of the block from
silence, keep the source and ask it again on the next pull, as
`MultiTapDelay` does (docs/upstream-diff.md). Upstream CircuitPython's pull
loop goes round for ever on such a source. No native source in this
repository hands an empty buffer with more to come, so the case is held on
the CPython twin, where a source can be written in Python;
`tests/parity/pitch_empty_source_probe.py` holds the looped-empty case on
every interpreter.
"""

import threading
import unittest
from array import array

import audiocore
import audiodelays

RATE = 8000


class _Stalling(audiocore._AudioSample):
    """Hands one real buffer, then empty ones with more to come, then real
    ones again: a stream that stalls for a few pulls and recovers."""

    def __init__(self, values, stall_pulls):
        self.sample_rate = RATE
        self.channel_count = 1
        self.bits_per_sample = 16
        self.samples_signed = True
        self._data = bytes(values)
        self._pulls = 0
        self._stall = stall_pulls

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        pass

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._pulls += 1
        if 1 < self._pulls <= 1 + self._stall:
            return audiocore.GET_BUFFER_MORE_DATA, b""
        return audiocore.GET_BUFFER_MORE_DATA, self._data


def pull_with_timeout(node, seconds=5.0):
    result = {}

    def target():
        result["data"] = bytes(audiocore.get_buffer(node)[1])

    worker = threading.Thread(target=target, daemon=True)
    worker.start()
    worker.join(seconds)
    return result.get("data")


class EmptyBufferWithMoreToCome(unittest.TestCase):

    def check(self, node):
        values = array("h", [8000] * 256)
        node.play(_Stalling(values, stall_pulls=3))
        blocks = []
        for _ in range(5):
            data = pull_with_timeout(node)
            self.assertIsNotNone(data, "the pull never returned")
            blocks.append(array("h", data))
        # The first block: the one real buffer (dry, at mix 0), then the
        # stall, so silence.
        self.assertEqual(list(blocks[0][:256]), [8000] * 256)
        self.assertEqual(list(blocks[0][256:]), [0] * 256)
        # The source is kept, and plays again once it has something.
        self.assertTrue(node.playing)
        self.assertTrue(any(word == 8000 for block in blocks[1:]
                            for word in block))

    def test_pitch_shift(self):
        self.check(audiodelays.PitchShift(
            semitones=7, mix=0.0, buffer_size=1024, sample_rate=RATE))

    def test_granular_pitch_shift(self):
        self.check(audiodelays.GranularPitchShift(
            semitones=7, mix=0.0, grain_size=256, buffer_size=1024,
            sample_rate=RATE))


if __name__ == "__main__":
    unittest.main()
