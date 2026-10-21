"""audioladder's cutoff input: the cutoff moved every sample, not every block.

`set(cutoff_hz=...)` moves the cutoff once a block, so a cutoff swept by an
audio-rate signal that way is a staircase, and the staircase is heard: images
of the modulation at multiples of the block rate, a buzz at
sample_rate / block_size. The `cutoff` input exists to remove that, and to
change nothing else:

| trait | bar |
| --- | --- |
| an input of zero | the same bytes as no input at all |
| a constant input | within 1 LSB of `set()` at the cutoff it names |
| audio-rate FM through the input, at a 64-frame block | the block-rate spur at 750 Hz more than 100 dB under the carrier |
| the same FM, stepped once a block with `set()` | the spur less than 60 dB under: the control, so the measurement is shown to see it |
| `cutoff_octaves`, and an input that runs dry | the octaves it names; dry counts as zero |
| the surface | `modulate()` connects and disconnects, and refuses what is not an input |

The FFT row's frequencies are chosen so the answer is unambiguous. The carrier
is 1900 Hz and the modulator 100 Hz, so everything a per-sample filter can
make from them sits on a multiple of 100 Hz. The block rate is 750 Hz, and
1900 - 4 * 100 - 750 = 750, so the stepped filter puts an image exactly on the
block rate, where the per-sample one has nothing at all to put.
"""

import math
import unittest
from array import array

import numpy as np

import audiocore
import audioladder

RATE = 48000
BLOCK = 64


def raw(values, channels=1):
    return audiocore.RawSample(array("h", [int(v) for v in values]),
                               sample_rate=RATE, channel_count=channels)


def sine(hz, level, frames):
    t = np.arange(frames)
    return np.round(level * np.sin(2 * np.pi * hz * t / RATE))


def render(node, frames, before_block=None):
    blocks = []
    got = 0
    index = 0
    while got < frames:
        if before_block is not None:
            before_block(index)
        data = bytes(audiocore.get_buffer(node)[1])
        block = np.frombuffer(data, dtype=np.int16)
        blocks.append(block)
        got += len(block) // node.channel_count
        index += 1
    return np.concatenate(blocks)[:frames * node.channel_count]


class Once:
    """An input that hands out one block and then nothing: one that has run
    dry. A RawSample would not do: pulled again, it starts over."""

    channel_count = 1

    def __init__(self, values):
        self._data = bytes(array("h", values))

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        data, self._data = self._data, b""
        return audiocore.GET_BUFFER_DONE, data

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        pass


def mono(**options):
    options.setdefault("buffer_size", BLOCK * 2)
    return audioladder.Ladder(sample_rate=RATE, channel_count=1, **options)


def line_db(spectrum, hz, reference):
    """The peak within three bins of `hz`, in dB against `reference`."""
    peak = spectrum[int(hz) - 3:int(hz) + 4].max()
    return 20.0 * math.log10(max(peak, 1e-300) / reference)


def blackman_spectrum(samples):
    x = np.asarray(samples, dtype=float)
    window = np.blackman(len(x))
    return np.abs(np.fft.rfft(x * window)) * 2.0 / window.sum()


class ParityTest(unittest.TestCase):
    """The input changes nothing it is not asked to."""

    FRAMES = 12000

    def source(self):
        return raw(sine(1900.0, 12000, self.FRAMES + 512))

    def test_an_input_of_zero_is_no_input(self):
        plain = mono(cutoff_hz=900.0, resonance=3.9, drive=2.0)
        plain.play(self.source())
        zero = mono(cutoff_hz=900.0, resonance=3.9, drive=2.0,
                    cutoff=raw([0] * (self.FRAMES + 512)))
        zero.play(self.source())
        self.assertEqual(render(plain, self.FRAMES).tobytes(),
                         render(zero, self.FRAMES).tobytes())

    def test_a_constant_input_is_set_at_the_cutoff_it_names(self):
        for value in (8192, -12345, 32767):
            with self.subTest(value=value):
                octaves = 3.0
                hz = 1000.0 * 2.0 ** (value / 32768.0 * octaves)
                stepped = mono(cutoff_hz=hz, resonance=3.0)
                stepped.play(self.source())
                driven = mono(cutoff_hz=1000.0, resonance=3.0,
                              cutoff_octaves=octaves,
                              cutoff=raw([value] * (self.FRAMES + 512)))
                driven.play(self.source())
                a = render(stepped, self.FRAMES).astype(int)
                b = render(driven, self.FRAMES).astype(int)
                self.assertLessEqual(int(np.abs(a - b).max()), 1)

    def test_the_default_depth_is_ten_octaves(self):
        # Full scale on a 20 Hz cutoff, ten octaves up, is 20480 Hz: the
        # filter wide open against one at 20 Hz.
        closed = mono(cutoff_hz=20.0)
        closed.play(self.source())
        opened = mono(cutoff_hz=20.0, cutoff=raw([32767] * 13000))
        opened.play(self.source())
        quiet = np.abs(render(closed, 6000)[2000:]).max()
        loud = np.abs(render(opened, 6000)[2000:]).max()
        self.assertGreater(loud, 10000)
        self.assertLess(quiet, 300)

    def test_an_input_that_runs_dry_counts_as_zero(self):
        short = mono(cutoff_hz=900.0, resonance=3.5,
                     cutoff=Once([20000] * 100))
        short.play(self.source())
        plain = mono(cutoff_hz=900.0, resonance=3.5)
        plain.play(self.source())
        a = render(short, 6000)
        b = render(plain, 6000)
        # The two differ while the input lasts and then the dry one is the
        # plain filter, carrying only the state the first 100 frames left.
        self.assertNotEqual(a[:100].tobytes(), b[:100].tobytes())
        tail = np.abs(a[4000:].astype(int) - b[4000:].astype(int)).max()
        self.assertLessEqual(int(tail), 1)


class AudioRateFmTest(unittest.TestCase):
    """The block-rate spur is the staircase's, and the input has none."""

    CARRIER = 1900.0
    MODULATOR = 100.0
    OCTAVES = 2.0       # the modulator at half scale: one octave either way
    SETTLE = RATE // 4
    FRAMES = RATE

    def setUp(self):
        total = self.SETTLE + self.FRAMES + 1024
        self.carrier = sine(self.CARRIER, 12000, total)
        self.modulator = sine(self.MODULATOR, 16384, total)
        self.options = dict(cutoff_hz=self.CARRIER, resonance=2.0)

    def per_sample(self):
        node = mono(cutoff_octaves=self.OCTAVES, **self.options)
        node.play(raw(self.carrier))
        node.modulate(cutoff=raw(self.modulator))
        return render(node, self.SETTLE + self.FRAMES)[self.SETTLE:]

    def stepped(self):
        # What a host without the input does: one set() a block, from the
        # modulator at the middle of the block.
        node = mono(**self.options)
        node.play(raw(self.carrier))

        def before(index):
            value = self.modulator[index * BLOCK + BLOCK // 2] / 32768.0
            node.set(cutoff_hz=self.CARRIER
                     * 2.0 ** (value * self.OCTAVES))
        return render(node, self.SETTLE + self.FRAMES, before)[self.SETTLE:]

    def spur_db(self, samples):
        spectrum = blackman_spectrum(samples)
        carrier = spectrum[int(self.CARRIER) - 3:int(self.CARRIER) + 4].max()
        return line_db(spectrum, RATE / BLOCK, carrier)

    def test_the_input_leaves_no_block_rate_spur(self):
        self.assertLess(self.spur_db(self.per_sample()), -100.0)

    def test_the_measurement_sees_the_staircase(self):
        self.assertGreater(self.spur_db(self.stepped()), -60.0)

    def test_the_input_does_modulate(self):
        # The sidebands at 1900 +- 100 Hz are the FM itself; a cutoff input
        # that did nothing would leave the carrier alone in its spectrum.
        spectrum = blackman_spectrum(self.per_sample())
        carrier = spectrum[int(self.CARRIER) - 3:int(self.CARRIER) + 4].max()
        for hz in (self.CARRIER - self.MODULATOR,
                   self.CARRIER + self.MODULATOR):
            self.assertGreater(line_db(spectrum, hz, carrier), -40.0, hz)


class SurfaceTest(unittest.TestCase):

    def test_modulate_connects_and_disconnects(self):
        source = raw(sine(1900.0, 12000, 4000))
        node = mono(cutoff_hz=400.0)
        node.play(source)
        node.modulate(cutoff=raw([30000] * 4000))
        opened = np.abs(render(node, 1000)[500:]).max()
        node.modulate(cutoff=None)
        closed = np.abs(render(node, 2000)[1500:]).max()
        self.assertGreater(opened, 4 * closed)

    def test_it_refuses_what_is_not_an_input(self):
        node = mono()
        with self.assertRaises(TypeError):
            node.modulate(cutof=raw([0] * 10))
        with self.assertRaises(TypeError):
            node.modulate(cutoff=600.0)
        with self.assertRaises(TypeError):
            audioladder.Ladder(sample_rate=RATE, cutoff=600.0)
        with self.assertRaises(TypeError):
            node.set(cutoff=raw([0] * 10))

    def test_a_stereo_input_is_read_from_its_first_channel(self):
        frames = 4000
        left = [20000] * frames
        stereo = [v for pair in zip(left, [-20000] * frames) for v in pair]
        source = sine(1900.0, 12000, frames)
        a = mono(cutoff_hz=600.0, cutoff=raw(left))
        a.play(raw(source))
        b = mono(cutoff_hz=600.0, cutoff=raw(stereo, channels=2))
        b.play(raw(source))
        self.assertEqual(render(a, 3000).tobytes(), render(b, 3000).tobytes())


if __name__ == "__main__":
    unittest.main()
