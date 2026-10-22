"""A voice is scaled in integers, because that is what the kernel does.

`src/audiomixer/Mixer.c`'s `mult16signed` is the authority, as CircuitPython
11.0.0-alpha.2 has it (adafruit/circuitpython#11617)::

    int16_t ai = (val >> (sizeof(uint16_t) * 8 * i));
    int32_t intermediate = (ai * (i ? himul : lomul)) >> 15;

The product is floored by the shift, not truncated toward zero, so a small
negative sample rounds down: -1 at level 0.5 stays -1. A level of 1.0 is Q15
32768 and a centred pan is `32768 - panning`, so a voice at level 1.0 and pan 0
multiplies by exactly 32768 on both channels and passes every sample through.

| what it checks | bar |
|---|---|
| every int16 at a spread of Q15 levels | the Python loop and the answer `(x * q) >> 15` agree for all 65536 values |
| the product is floored | -1 at level 0.5 is -1, and 1 is 0 |
| a level-1.0 voice is a wire at the rails | every int16 value renders back unchanged, mono and stereo |
| 32767 is not unity | `(x * 32767) >> 15` differs from `x` for some int16, so only 32768 may skip the scaling |
| a centred pan is exact | a stereo voice at pan 0 scales both channels by the level itself |
| a hard pan | pan 1.0 scales the left channel by 1/32768 of the level, pan -1.0 the right |
| a voice at unity is passed through, not scaled | after the block that opens the level gate, a unity voice's output is byte-identical to its input and the scaling kernel is not called -- audiodsp#273 |
| any other level still scales | the same material at 0.5 differs from its input, and matches `(x * q) >> 15` sample for sample |

The other half of the evidence is not here, because CPython alone cannot
supply it: `tests/parity/mixer_q15_level_probe.py` renders material chosen at
the rails and around zero under desktop MicroPython and under the pinned
CircuitPython oracle and `verify_dsp.py` requires all three to agree byte for
byte. This file is what CI can run, which has no interpreters.
"""

import array
import unittest
from unittest import mock

import _audiodsp
import audiocore
import audiomixer

SAMPLE_RATE = 48000
BUFFER_SIZE = 2048

LEVELS = (0, 1, 100, 8192, 16384, 25800, 25801, 32766, 32767, 32768)


def _scaled(value, scale):
    """What `mult16signed` does."""
    return max(-32768, min(32767, (value * scale) >> 15))


def _render(channels, level, panning, values, loop=False):
    """One block of a single voice at `level` and `panning`, as a list."""
    mixer = audiomixer.Mixer(
        voice_count=1, sample_rate=SAMPLE_RATE, channel_count=channels,
        bits_per_sample=16, samples_signed=True, buffer_size=BUFFER_SIZE)
    mixer.voice[0].level = level
    if channels == 2:
        mixer.voice[0].panning = panning
    mixer.play(audiocore.RawSample(
        array.array("h", values), sample_rate=SAMPLE_RATE,
        channel_count=channels), loop=loop)
    block = array.array("h")
    block.frombytes(bytes(audiocore.get_buffer(mixer)[1]))
    return list(block)


class MixerLevelIsInteger(unittest.TestCase):

    def test_both_loops_are_the_integer_product_for_every_int16(self):
        values = array.array("h", range(-32768, 32768))
        every = values.tobytes()
        kernels = [audiomixer._scale_s16_levels_py]
        if hasattr(_audiodsp, "scale_s16_levels"):
            kernels.append(_audiodsp.scale_s16_levels)
        for left, right in zip(LEVELS, LEVELS[3:] + LEVELS[:3]):
            for kernel in kernels:
                with self.subTest(left=left, right=right, kernel=kernel):
                    out = array.array("h")
                    out.frombytes(kernel(every, left, right, left, right, []))
                    want = [_scaled(v, left if i % 2 == 0 else right)
                            for i, v in enumerate(values)]
                    # Not assertEqual on the lists: a failure there spends
                    # minutes in difflib over 65536 items.
                    bad = next((i for i, (got, w) in enumerate(zip(out, want))
                                if got != w), None)
                    self.assertIsNone(
                        bad, "sample %s: %s, want %s" % (
                            bad, None if bad is None else out[bad],
                            None if bad is None else want[bad]))

    def test_the_product_is_floored_not_truncated(self):
        """Upstream's change in rounding, spelled out. Truncation toward zero
        sent -1 at level 0.5 to 0; the shift floors it to -1."""
        self.assertEqual(_scaled(-1, 16384), -1)
        self.assertEqual(_scaled(1, 16384), 0)
        self.assertEqual(_scaled(-3, 16384), -2)
        self.assertEqual(_scaled(3, 16384), 1)
        # Through the real kernel, not just the helper.
        out = _render(1, 0.5, 0.0, [0, -1, 1, -3, 3] * 3)
        self.assertEqual(out[1:5], [-1, 0, -2, 1])

    def test_32767_is_not_unity(self):
        """The CPython target skips the scaling at unity; it may do that only
        for 32768, the one multiplier that is the identity for every int16."""
        self.assertEqual(audiomixer._UNITY_Q15, 32768)
        self.assertTrue(all(_scaled(v, 32768) == v
                            for v in range(-32768, 32768)))
        self.assertTrue(any(_scaled(v, 32767) != v
                            for v in range(-32768, 32768)))

    def test_a_level_one_voice_is_a_wire_at_the_rails(self):
        """Every int16 value, once, through a level-1.0 voice, comes back as
        itself, mono and stereo.

        The leading block of silence opens the level gate -- without it the
        ramp renders at level 0 until its own zero crossing, and the
        comparison would be measuring the gate.
        """
        for channels in (1, 2):
            with self.subTest(channels=channels):
                values = array.array("h", [0] * (512 * channels))
                for value in range(-32768, 32768):
                    for _channel in range(channels):
                        values.append(value)
                mixer = audiomixer.Mixer(
                    voice_count=1, sample_rate=SAMPLE_RATE,
                    channel_count=channels, bits_per_sample=16,
                    samples_signed=True, buffer_size=BUFFER_SIZE)
                mixer.voice[0].level = 1.0
                mixer.play(audiocore.RawSample(
                    array.array("h", values), sample_rate=SAMPLE_RATE,
                    channel_count=channels))
                rendered = array.array("h")
                while len(rendered) < len(values):
                    block = array.array("h")
                    block.frombytes(bytes(audiocore.get_buffer(mixer)[1]))
                    rendered.extend(block)
                head = 512 * channels
                self.assertEqual(list(rendered[head:len(values)]),
                                 list(values[head:]))

    def test_a_centred_pan_scales_both_channels_by_the_level(self):
        """`32768 - panning` at pan 0: no channel sits one below the other.
        The material is a run of one value on both channels, so a difference
        between the channels would be a difference between the scales."""
        for level in (0.25, 0.5, 100 / 127, 0.999):
            with self.subTest(level=level):
                q = int(level * 32768)
                values = [0, 0] + [10822, 10822] * 8
                out = _render(2, level, 0.0, values, loop=True)
                self.assertEqual(out[2], _scaled(10822, q))
                self.assertEqual(out[2], out[3])

    def test_a_hard_pan_leaves_one_unit_on_the_far_channel(self):
        """Pan 1.0 is the clamped 32767/32768, so the left scale is
        `32768 - 32767`: 1 in Q15, scaled again by the level."""
        values = [0, 0] + [32767, 32767] * 8
        out = _render(2, 1.0, 1.0, values, loop=True)
        self.assertEqual(out[2], _scaled(32767, (1 * 32768) >> 15))
        self.assertEqual(out[3], 32767)
        out = _render(2, 1.0, -1.0, values, loop=True)
        self.assertEqual(out[2], 32767)
        self.assertEqual(out[3], _scaled(32767, (1 * 32768) >> 15))

    def _steady_state(self, channels, level, blocks=3):
        """Render `blocks` blocks of a looping ramp after one that opens the
        level gate, returning (input blocks, output blocks, kernel calls
        made while rendering them)."""
        mixer = audiomixer.Mixer(
            voice_count=1, sample_rate=SAMPLE_RATE, channel_count=channels,
            bits_per_sample=16, samples_signed=True, buffer_size=BUFFER_SIZE)
        mixer.voice[0].level = level
        frames_per_block = mixer._render_size // (2 * channels)
        values = array.array("h", [0] * (frames_per_block * channels))
        for index in range(frames_per_block * blocks * channels):
            values.append(((index * 37) % 20000) - 10000 or 1)
        self.assertEqual(len(values) * 2, mixer._render_size * (blocks + 1))
        mixer.play(audiocore.RawSample(
            values, sample_rate=SAMPLE_RATE, channel_count=channels),
            loop=True)
        audiocore.get_buffer(mixer)         # the block that opens the gate
        calls = []
        real = audiomixer._scale_s16_levels

        def counting(*args):
            calls.append(args)
            return real(*args)
        with mock.patch.object(audiomixer, "_scale_s16_levels", counting):
            out = [bytes(audiocore.get_buffer(mixer)[1])
                   for _ in range(blocks)]
        size = mixer._render_size
        src = values.tobytes()[size:]
        return [src[i * size:(i + 1) * size] for i in range(blocks)], out, calls

    def test_a_unity_voice_is_passed_through_unscaled(self):
        """audiodsp#273: at level 1.0 the multiplier is 32768, so the block is
        the source's bytes, and the scaling is skipped rather than computed to
        the same answer."""
        for channels in (1, 2):
            with self.subTest(channels=channels):
                src, out, calls = self._steady_state(channels, 1.0)
                self.assertEqual(out, src)
                self.assertEqual(calls, [])

    def test_a_non_unity_voice_is_still_scaled(self):
        for channels in (1, 2):
            with self.subTest(channels=channels):
                src, out, calls = self._steady_state(channels, 0.5)
                self.assertNotEqual(out, src)
                self.assertTrue(calls)
                scale = int(0.5 * 32768)
                for want, got in zip(src, out):
                    a, b = array.array("h"), array.array("h")
                    a.frombytes(want)
                    b.frombytes(got)
                    for x, y in zip(a, b):
                        self.assertEqual(y, _scaled(x, scale))


if __name__ == "__main__":
    unittest.main()
