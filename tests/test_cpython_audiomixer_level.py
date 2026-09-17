"""A voice is scaled in float32, because that is what the kernel does.

`src/audiomixer/Mixer.c`'s `mult16signed` is the authority::

    float mod_mul = (float)(i ? himul : lomul) / (float)((1 << 15) - 1);
    int16_t ai = (val >> (sizeof(uint16_t) * 8 * i));
    int32_t intermediate = (int32_t)(ai * mod_mul);

Single-precision quotient, single-precision product, truncation toward zero.
The CPython twin formed the same product in `float64` until audioif#84, and the
two precisions truncate to different integers exactly where the true product
sits within a float32 rounding of one -- a whole LSB apart, not a rounding.

| what it checks | bar |
|---|---|
| the twin's multiplier is the float32 one | `_mod_mul(level)` is bit-equal to `(float)level / 32767.0f` for every Q15 level 0..32768 |
| the flipping values are enumerated, not sampled | at level 100/127 exactly 56 of the 65536 int16 values flip -- 28 on the left scale (Q15 25800), 28 on the right (25801), sharing none |
| the twin renders the float32 answer | all 56, both channels, through a real `Mixer` block |
| the old float64 twin would fail this | the float64 formula differs on every one of the 56 |
| the level gate is not what is being measured | the leading zero pair opens it on the first word, so nothing here renders at level zero |

The other half of the evidence is not here, because CPython alone cannot
supply it: `tests/parity/mixer_level_precision_probe.py` renders this same
material under desktop MicroPython and under the pinned CircuitPython oracle
and `verify_dsp.py` requires all three to agree byte for byte. This file is
what CI can run, which has no interpreters.
"""

import array
import struct
import unittest

import audiocore
import audiomixer
from audiomixer import _f32, _mod_mul

SAMPLE_RATE = 48000
BUFFER_SIZE = 2048
LEVEL = 100 / 127

#: Q15 scales the mixer derives for level 100/127 at panning 0.0. The left
#: channel runs one below the right because `left_scale` is `32767 - panning`
#: where `right_scale` is `32768` -- the panning-zero asymmetry this port keeps
#: verbatim from CircuitPython.
LEFT_SCALE, RIGHT_SCALE = 25800, 25801

#: Every int16 whose truncated product differs between the two precisions at
#: that level. Enumerated by `_flips` below over the whole int16 range, and
#: pinned here so a change to the arithmetic has to move a written-down number.
LEFT_FLIPS = (
    -32466, -31135, -29503, -27871, -26239, -24908, -24607, -23276,
    -21644, -18681, -17049, -12454, -10822, -6227, 6227, 10822,
    12454, 17049, 18681, 21644, 23276, 24607, 24908, 26239,
    27871, 29503, 31135, 32466,
)
RIGHT_FLIPS = (
    -31544, -30321, -29098, -27875, -26652, -25429, -24206, -22983,
    -21760, -15772, -14549, -13326, -12103, -10880, 10880, 12103,
    13326, 14549, 15772, 21760, 22983, 24206, 25429, 26652,
    27875, 29098, 30321, 31544,
)


def _float64_scaled(value, scale):
    """What the twin did before audioif#84."""
    return max(-32768, min(32767, int(value * (scale / 32767.0))))


def _float32_scaled(value, scale):
    """What `mult16signed` does."""
    return max(-32768, min(32767, int(_f32(value * _mod_mul(scale)))))


def _flips(scale):
    return tuple(value for value in range(-32768, 32768)
                 if _float64_scaled(value, scale)
                 != _float32_scaled(value, scale))


class MixerLevelIsSinglePrecision(unittest.TestCase):

    def test_the_multiplier_is_the_float32_quotient(self):
        """Every Q15 level a voice can carry, not a sampled few."""
        for level in range(0, 32769):
            expected = struct.unpack(
                "f", struct.pack("f", level / 32767.0))[0]
            self.assertEqual(
                struct.pack("f", _mod_mul(level)),
                struct.pack("f", expected),
                "level %d" % level)

    def test_the_flipping_values_are_exactly_these(self):
        """The enumeration is the test: a sampled probe would miss them.

        56 of 65536 is 0.085% of the int16 range, and on real material about
        1% of samples -- 77 of 8192 on the render that found this.
        """
        self.assertEqual(_flips(LEFT_SCALE), LEFT_FLIPS)
        self.assertEqual(_flips(RIGHT_SCALE), RIGHT_FLIPS)
        self.assertEqual(len(LEFT_FLIPS) + len(RIGHT_FLIPS), 56)
        self.assertEqual(set(LEFT_FLIPS) & set(RIGHT_FLIPS), set())

    def test_the_old_twin_would_fail_on_all_of_them(self):
        """A planted check: if the two formulas ever agree here, this file has
        stopped being able to fail."""
        for value in LEFT_FLIPS:
            self.assertNotEqual(_float64_scaled(value, LEFT_SCALE),
                                _float32_scaled(value, LEFT_SCALE))
        for value in RIGHT_FLIPS:
            self.assertNotEqual(_float64_scaled(value, RIGHT_SCALE),
                                _float32_scaled(value, RIGHT_SCALE))

    def test_the_named_sample_from_the_issue(self):
        """10822 * 25800 / 32767 is 8520.999786: float32 rounds the product up
        to 8521.0 and truncates to 8521, float64 keeps the .9998 and truncates
        to 8520. One value, spelled out, so the mechanism is readable."""
        self.assertEqual(_float64_scaled(10822, LEFT_SCALE), 8520)
        self.assertEqual(_float32_scaled(10822, LEFT_SCALE), 8521)
        self.assertEqual(_float64_scaled(10822, RIGHT_SCALE), 8521)
        self.assertEqual(_float32_scaled(10822, RIGHT_SCALE), 8521)

    def test_a_rendered_block_carries_the_float32_answer(self):
        """Through a real Mixer, both channels, all 56.

        Each value appears twice in a row, so it lands once at an even index
        (scaled by the left multiplier) and once at an odd one (the right).
        """
        values = sorted(set(LEFT_FLIPS) | set(RIGHT_FLIPS))
        material = array.array("h", [0, 0])
        for value in values:
            material.append(value)
            material.append(value)

        mixer = audiomixer.Mixer(
            voice_count=1, buffer_size=BUFFER_SIZE, sample_rate=SAMPLE_RATE,
            channel_count=2, bits_per_sample=16, samples_signed=True)
        mixer.voice[0].level = LEVEL
        mixer.voice[0].panning = 0.0
        mixer.voice[0].play(audiocore.RawSample(
            material, sample_rate=SAMPLE_RATE, channel_count=2), loop=True)

        rendered = array.array("h")
        rendered.frombytes(bytes(audiocore.get_buffer(mixer)[1]))

        for index, value in enumerate(values):
            self.assertEqual(rendered[2 + index * 2],
                             _float32_scaled(value, LEFT_SCALE),
                             "left, sample %d" % value)
            self.assertEqual(rendered[3 + index * 2],
                             _float32_scaled(value, RIGHT_SCALE),
                             "right, sample %d" % value)

    def test_the_scales_are_the_ones_named(self):
        """The Q15 pair above is derived, not asserted: if the panning-zero
        asymmetry ever goes, this says so rather than quietly testing the
        wrong two numbers."""
        level = int(min(1.0, max(0.0, LEVEL)) * 32768)
        panning = 0
        left_scale = 32767 - panning
        right_scale = 32768
        self.assertEqual((left_scale * level) >> 15, LEFT_SCALE)
        self.assertEqual((right_scale * level) >> 15, RIGHT_SCALE)


if __name__ == "__main__":
    unittest.main()
