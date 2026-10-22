"""The C scaling kernel renders the same bytes the Python loop does.

`Mixer._get_buffer` scales every voice that is not at unity by its per-channel
Q15 level -- `(sample * level) >> 15`, clamped to int16 -- and
lets a pending level move land at a zero crossing or at the end of a source
stretch. That loop ran in Python, a struct round trip per sample, until
`_audiodsp.scale_s16_levels` (audiodsp#273). The Python loop stays as the
fallback for a build without the kernel, so the two have to agree exactly.

| what it checks | bar |
|---|---|
| the kernel is in the build | `_audiodsp.scale_s16_levels` exists, and `audiomixer` picked it up |
| every int16, every level that matters | all 65536 values at a spread of Q15 levels (unity, near-unity, 100/127, quiet, zero) match the Python loop byte for byte |
| level moves | a pending move landing at a zero crossing, forced at a stretch end, never arriving, or arriving at the first pair, over seeded random blocks with rails, zeros and sign changes |
| through a real Mixer | the same graph -- several voices, mono and stereo, levels and pans changed between blocks, looping sources -- rendered with the kernel and with the Python loop swapped in is byte-identical, block for block |
"""

import array
import random
import unittest
from unittest import mock

import _audiodsp
import audiocore
import audiomixer

SAMPLE_RATE = 48000
BUFFER_SIZE = 2048

LEVELS = (0, 1, 100, 8192, 16384, 25800, 25801, 30000, 32766, 32767, 32768)


def _native():
    return _audiodsp.scale_s16_levels


class KernelTest(unittest.TestCase):
    def test_the_mixer_uses_the_kernel(self):
        self.assertTrue(hasattr(_audiodsp, "scale_s16_levels"))
        self.assertIs(audiomixer._scale_s16_levels, _native())

    def test_every_int16_at_every_level(self):
        every = array.array("h", range(-32768, 32768)).tobytes()
        # Each level on the left against a different one on the right.
        for left, right in zip(LEVELS, LEVELS[3:] + LEVELS[:3]):
            with self.subTest(left=left, right=right):
                args = (every, left, right, left, right, [])
                self.assertEqual(_native()(*args),
                                 audiomixer._scale_s16_levels_py(*args))

    def _random_block(self, rng, pairs):
        picks = (0, 1, -1, 32767, -32768, 32766, -32767)
        values = []
        for _ in range(pairs * 2):
            roll = rng.random()
            if roll < 0.1:
                values.append(rng.choice(picks))
            elif roll < 0.2:
                values.append(0)
            else:
                values.append(rng.randint(-32768, 32767))
        return array.array("h", values).tobytes()

    def test_level_moves_land_the_same_way(self):
        rng = random.Random(273)
        for case in range(400):
            pairs = rng.choice((1, 2, 3, 64, 256, 257, 512))
            data = self._random_block(rng, pairs)
            active = (rng.choice(LEVELS), rng.choice(LEVELS))
            pending = active if case % 5 == 0 else (
                rng.choice(LEVELS), rng.choice(LEVELS))
            forced = sorted({rng.randrange(0, pairs + 2) * 2
                             for _ in range(rng.choice((0, 1, 2, 4)))})
            with self.subTest(case=case, active=active, pending=pending,
                              forced=forced):
                args = (data, *active, *pending, forced)
                self.assertEqual(_native()(*args),
                                 audiomixer._scale_s16_levels_py(*args))

    def test_a_move_with_no_zero_crossing_waits(self):
        # All positive and non-zero: no crossing, so the old level holds all
        # block long, and a forced index is the only thing that moves it.
        data = array.array("h", [1000, 2000] * 8).tobytes()
        for forced in ([], [6]):
            args = (data, 16384, 16384, 32768, 32768, forced)
            with self.subTest(forced=forced):
                out = _native()(*args)
                self.assertEqual(out, audiomixer._scale_s16_levels_py(*args))
                got = array.array("h")
                got.frombytes(out)
                self.assertEqual(got[0], 1000 * 16384 >> 15)
                self.assertEqual(got[-1], 2000 if forced else 2000 * 16384 >> 15)

    def test_bad_arguments_raise(self):
        with self.assertRaises(ValueError):
            _native()(b"\x00\x00\x00", 1, 1, 1, 1, [])
        with self.assertRaises(TypeError):
            _native()(b"\x00\x00", 1, 1, 1, 1, 5)


class MixerTest(unittest.TestCase):
    def _render(self, channels, native, seed):
        rng = random.Random(seed)
        mixer = audiomixer.Mixer(
            voice_count=5, sample_rate=SAMPLE_RATE, channel_count=channels,
            bits_per_sample=16, samples_signed=True, buffer_size=BUFFER_SIZE)
        frames = mixer._render_size // (2 * channels)
        for voice in range(4):
            length = frames * rng.choice((1, 2, 3)) + rng.choice((0, 7, 40))
            values = array.array("h", [0] * channels * 4)
            for _ in range(length * channels):
                values.append(rng.randint(-32768, 32767))
            mixer.play(audiocore.RawSample(
                values, sample_rate=SAMPLE_RATE, channel_count=channels),
                voice=voice, loop=bool(voice % 2))
        # The fifth voice is idle, as the spare voices of a big Mixer are.
        blocks = []
        kernel = _native() if native else audiomixer._scale_s16_levels_py
        with mock.patch.object(audiomixer, "_scale_s16_levels", kernel):
            for block in range(12):
                for voice in range(4):
                    mixer.voice[voice].level = rng.choice(
                        (1.0, 0.5, 100 / 127, 0.01, 0.999, 0.0, 0.25))
                    if channels == 2:
                        mixer.voice[voice].panning = rng.choice(
                            (0.0, -1.0, 1.0, 0.3, -0.7))
                blocks.append(bytes(audiocore.get_buffer(mixer)[1]))
        return blocks

    def test_kernel_and_python_loop_render_the_same_graph(self):
        for channels in (1, 2):
            for seed in (1, 2, 3):
                with self.subTest(channels=channels, seed=seed):
                    native = self._render(channels, True, seed)
                    python = self._render(channels, False, seed)
                    self.assertEqual(native, python)
                    self.assertTrue(any(any(block) for block in native))


if __name__ == "__main__":
    unittest.main()
