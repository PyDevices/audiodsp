"""`audiospeed.SpeedChanger`: the Q16 rate, and the phase across a boundary.

Two deliberate departures from CircuitPython 10.3.0, both of them upstream
bugs this port declines to reproduce. `docs/upstream-diff.md` carries the
measurements and `docs/upstream-reports/` the reports.

The parity gate cannot see either one. `resampler_probe.py` only ever asks
for 2.0, 1.0 and 0.5, which are exact in Q16 and land on a buffer boundary
with nothing left over, so every interpreter agrees on it whether the
arithmetic is right or not. What these tests measure is the arithmetic.

No numpy: this runs wherever the wheel is installed, and the wheel's numpy is
an extra.

The material is a **sample-and-hold built out of two SpeedChangers** —
decimate at N, restore at 1/N — because that is how the palette builds a
lo-fi rate reducer and it is where both defects show. The source hands out
256-frame buffers, which is the boundary the phase is thrown away at.
"""

import math
import unittest
from array import array

import audiocore
import audiofilters
import audiospeed

RATE = 48000
BLOCK = 256


def blocks(values, rate=RATE, channels=2, block=BLOCK):
    """A source that hands out `block` frames at a time, the way a real graph
    does. A `Filter` with nothing set is a wire; its `buffer_size` is bytes."""
    node = audiofilters.Filter(sample_rate=rate, channel_count=channels,
                               buffer_size=block * channels * 2)
    node.play(audiocore.RawSample(values, sample_rate=rate,
                                  channel_count=channels))
    return node


def hold(values, down, rate=RATE):
    return audiospeed.SpeedChanger(
        audiospeed.SpeedChanger(blocks(values, rate), down), 1.0 / down)


def render(node, frames, channels=2):
    """The first channel of `frames` frames, as plain ints."""
    out = array("h")
    while len(out) < frames * channels:
        result, data = audiocore.get_buffer(node)
        chunk = bytes(data)
        if not chunk:
            break
        out.frombytes(chunk)
        if result == audiocore.GET_BUFFER_DONE:
            break
    return list(out[0:frames * channels:channels])


def tone(frames, hz, level=16384, rate=RATE, channels=2):
    values = array("h", bytes(frames * channels * 2))
    for frame in range(frames):
        value = int(round(level * math.sin(2 * math.pi * hz * frame / rate)))
        for channel in range(channels):
            values[frame * channels + channel] = value
    return values


def ramp(frames, channels=2):
    """One code per frame, full scale. A hold passes it unchanged except for
    the step it holds, so anything left over is drift."""
    values = array("h", bytes(frames * channels * 2))
    for frame in range(frames):
        value = max(-32768, min(32767, frame - frames // 2))
        for channel in range(channels):
            values[frame * channels + channel] = value
    return values


def line_db(signal, hz, rate=RATE):
    """One Hann-windowed DFT bin, in dB. A line that has moved off its bin
    reads *low*, which is the point: a smeared image is a quiet one."""
    real = imaginary = 0.0
    count = len(signal)
    for index, value in enumerate(signal):
        window = 0.5 - 0.5 * math.cos(2 * math.pi * index / count)
        angle = 2 * math.pi * hz * index / rate
        real += value * window * math.cos(angle)
        imaginary -= value * window * math.sin(angle)
    return 20 * math.log10(max(math.hypot(real, imaginary) / count, 1e-12))


def sinc_db(hz, hold_hz):
    """What a zero-order hold does to a line at `hz`, in dB."""
    x = math.pi * hz / hold_hz
    return 20 * math.log10(abs(math.sin(x) / x)) if x else 0.0


class TheRateRoundsToTheNearestStep(unittest.TestCase):
    """audioif#92. Upstream's `rate_to_fp` is `(uint32_t)(rate * (1 << 16))`,
    a truncation, so a float landing a hair under its Q16 neighbour loses a
    whole LSB rather than arriving at it. Every number below was measured on
    this twin before the fix and is in `docs/upstream-diff.md`."""

    def test_a_rate_a_hair_under_unity_arrives_at_unity(self):
        """Truncating gave 65535/65536 = 0.9999847412109375."""
        node = audiospeed.SpeedChanger(blocks(tone(512, 1000.0)),
                                       1.0 / 1.0000000000000004)
        self.assertEqual(node.rate, 1.0)

    def test_a_rate_a_hair_under_a_quarter_arrives_at_a_quarter(self):
        """Truncating gave 16383/65536 = 0.2499847412109375."""
        node = audiospeed.SpeedChanger(blocks(tone(512, 1000.0)),
                                       1.0 / 4.0000000000000036)
        self.assertEqual(node.rate, 0.25)

    def test_a_rate_already_on_a_step_is_not_moved(self):
        """The fault the rounding could plant: nudging what was already
        right. Every one of these is exact in Q16, so both arithmetics must
        agree on them."""
        for asked in (0.0, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 1000.0):
            with self.subTest(rate=asked):
                node = audiospeed.SpeedChanger(blocks(tone(64, 1000.0)),
                                               asked)
                self.assertEqual(node.rate, asked)

    def test_the_rate_is_still_refused_outside_its_range(self):
        """Rounding must not widen the range by half an LSB either."""
        source = blocks(tone(64, 1000.0))
        with self.assertRaises(ValueError):
            audiospeed.SpeedChanger(source, 1000.5)
        with self.assertRaises(ValueError):
            audiospeed.SpeedChanger(source, -0.5)

    def test_a_pair_asked_for_the_running_rate_is_a_wire(self):
        """The one that bites. A class computing `fs / rate_hz` in float gets
        1.0000000000000004 off a log-mapped knob, and the pair built from it
        was not an identity at 44.1 kHz: max |wet - dry| read 27666 codes."""
        frames, rate = 4096, 44100
        down = 1.0000000000000004
        values = tone(frames, 441.0, rate=rate)
        wet = render(hold(values, down, rate), frames)
        dry = render(blocks(values, rate), frames)
        self.assertEqual(max(abs(a - b) for a, b in zip(wet, dry)), 0)

    def test_a_resamplers_bound_ratio_rounds_too(self):
        """`Resampler` reaches the same arithmetic by another road: upstream's
        `calculate_rate` truncates 48000/44100 from 71331.9 to 71331."""
        source = audiocore.RawSample(tone(512, 1000.0, rate=48000),
                                     sample_rate=48000, channel_count=2)
        node = audiospeed.Resampler(source)
        audiofilters.Filter(sample_rate=44100, channel_count=2,
                            buffer_size=512).play(node)
        self.assertEqual(round(node.rate * 65536), 71332)


if __name__ == "__main__":
    unittest.main()
