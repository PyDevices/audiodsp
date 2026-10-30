"""`audiopump.DriftResampler`: lock, hold and sound, against arithmetic.

A producer on one clock writes a Ring; the node plays it on another and
steers its ratio so the Ring holds its target fill. `drift_probe.py` proves
every target renders the node identically. These are the traits, measured
against the clocks the simulation sets rather than against a stored answer:

| Trait | Bar |
| --- | --- |
| Locks at +100 ppm and -100 ppm, 48-frame packets | no underrun or overrun; smoothed fill within 8 frames of target after 20 s; mean ratio within 15 ppm of the offset |
| Locks with 480-frame packets (a USB or network burst) | no underrun or overrun; within 8 frames after 45 s |
| Follows a clock wandering +/-100 ppm over a minute | no underrun or overrun; within 80 frames after 20 s |
| THD+N of a 0.9 FS sine through it at +100 ppm, 1 kHz and 10 kHz | below -92 dB (two 16-bit quantizations alone are about -94.5) |
| Constant input | comes out exactly, whatever the fractional position |
| Starts at the target | silence until the Ring reaches it, then the fill is the target |
| A stall | one underrun, counted on the node and the Ring, then plays again |
| Arguments | every bound refused with ValueError, a non-Ring with TypeError |

The THD+N bar is set by 16-bit audio, not by the filter: a sine quantized
once to 16 bits measures -97.5 dB here and quantized twice about -94.5, and
the resampler adds little to that (docs/how-it-works/drift-resampler.md).
"""

import math
import unittest

import numpy as np

import audiocore
import audiopump

RATE = 48000
BLOCK = 256


def blackman_harris(n):
    k = np.arange(n) * (2.0 * math.pi / (n - 1))
    return (0.35875 - 0.48829 * np.cos(k) + 0.14128 * np.cos(2 * k) -
            0.01168 * np.cos(3 * k))


def thd_n(y, notch_hz=10.0):
    """Everything from 20 Hz to 20 kHz but the fundamental, in dB."""
    spectrum = np.abs(np.fft.rfft(y * blackman_harris(len(y)))) ** 2
    freqs = np.fft.rfftfreq(len(y), 1.0 / RATE)
    band = (freqs >= 20) & (freqs <= 20000)
    peak = freqs[np.argmax(np.where(band, spectrum, 0))]
    fundamental = band & (np.abs(freqs - peak) <= notch_hz)
    return 10 * math.log10(spectrum[band & ~fundamental].sum() /
                           spectrum[fundamental].sum())


def sine_packet(freq, frames, channels=2, amp=0.9):
    """`frames` frames of a sine whose period divides them, so every packet
    is the same bytes and the stream is continuous."""
    n = np.arange(frames)
    s = np.round(amp * 32767 * np.sin(2 * math.pi * freq * n / RATE))
    s = s.astype("<i2")
    return np.repeat(s, channels).tobytes() if channels == 2 else s.tobytes()


class Run:
    """A producer whose clock runs `ppm(t)` fast against the output, writing
    `packet` frames at a time, and the node pulling BLOCK frames a block."""

    def __init__(self, ppm, seconds, packet=48, capacity=8, freq=1000.0,
                 keep_audio=False, **options):
        self.ring = audiopump.Ring(sample_rate=RATE, channel_count=2,
                                   frames=BLOCK, capacity=capacity)
        self.node = audiopump.DriftResampler(self.ring, **options)
        data = sine_packet(freq, packet)
        blocks = int(seconds * RATE / BLOCK)
        self.fill = np.zeros(blocks)
        self.ppm = np.zeros(blocks)
        self.underruns = np.zeros(blocks, dtype=int)
        self.overruns = np.zeros(blocks, dtype=int)
        audio = []
        due = 0.0           # output-frame time the next packet is complete
        for block in range(blocks):
            now = block * BLOCK
            while True:
                after = due + packet / (1.0 + ppm(due / RATE) * 1e-6)
                if after > now:
                    break
                due = after
                self.ring.write(data)
            _, out = audiocore.get_buffer(self.node)
            if keep_audio:
                audio.append(bytes(out))
            self.fill[block] = self.node.fill
            self.ppm[block] = self.node.ppm
            self.underruns[block] = self.node.underruns
            self.overruns[block] = self.ring.stats()[4]
        if keep_audio:
            self.audio = np.frombuffer(b"".join(audio), "<i2")[0::2].astype(float)

    def after(self, seconds):
        return slice(int(seconds * RATE / BLOCK), None)


class TestLock(unittest.TestCase):

    def check_locked(self, run, settle, within):
        late = run.after(settle)
        target = run.node.target
        self.assertEqual(run.underruns[-1], 0)
        self.assertEqual(run.overruns[-1], 0)
        error = np.abs(run.fill[late] - target).max()
        self.assertLess(error, within, "fill strays %.1f frames" % error)

    def test_steady_offsets(self):
        for offset in (100, -100):
            with self.subTest(ppm=offset):
                run = Run(lambda t, o=offset: o, 40)
                self.check_locked(run, 20, 8)
                mean = run.ppm[run.after(30)].mean()
                self.assertLess(abs(mean - offset), 15, "ratio %.1f ppm" % mean)

    def test_large_packets(self):
        run = Run(lambda t: 100, 70, packet=480)
        self.check_locked(run, 45, 8)

    def test_wandering_clock(self):
        run = Run(lambda t: 100 * math.sin(2 * math.pi * t / 60), 60)
        self.check_locked(run, 20, 80)


class TestSound(unittest.TestCase):

    def test_thd_n(self):
        for freq in (1000.0, 10000.0):
            with self.subTest(freq=freq):
                run = Run(lambda t: 100, 24, freq=freq, keep_audio=True)
                self.assertEqual(run.underruns[-1], 0)
                tail = run.audio[-2 * RATE:]
                level = thd_n(tail)
                self.assertLess(level, -92.0, "THD+N %.1f dB" % level)

    def test_constant_passes_exactly(self):
        # 44.1 kHz up to 48 kHz walks the read position through every
        # fractional row of the table.
        ring = audiopump.Ring(sample_rate=44100, channel_count=1, frames=64,
                              capacity=8)
        node = audiopump.DriftResampler(ring, sample_rate=RATE, target=256)
        packet = np.full(64, 12345, dtype="<i2").tobytes()
        steady = []
        for block in range(400):
            while ring.space() >= len(packet) and ring.level() < 300 * 2:
                ring.write(packet)
            data = np.frombuffer(bytes(audiocore.get_buffer(node)[1]), "<i2")
            # After the first played block, whose filter reads the silent
            # history before the first frame.
            if block > 10:
                steady.append(data)
        steady = np.concatenate(steady)
        self.assertEqual(set(steady.tolist()), {12345})
        self.assertEqual(node.underruns, 0)


class TestBehaviour(unittest.TestCase):

    def test_starts_at_target(self):
        ring = audiopump.Ring(sample_rate=RATE, channel_count=2, frames=64,
                              capacity=16)
        node = audiopump.DriftResampler(ring, target=400)
        packet = sine_packet(1000.0, 48)
        silent = bytes(256)
        while ring.level() // 4 < 300:
            ring.write(packet)
        self.assertEqual(bytes(audiocore.get_buffer(node)[1]), silent)
        while ring.level() // 4 < 700:
            ring.write(packet)
        audiocore.get_buffer(node)
        # It dropped what was beyond the target and played from there.
        self.assertLess(abs(node.fill - 400), 2)
        self.assertLess(abs(ring.level() // 4 - (400 - 64)), 20)
        self.assertEqual(node.underruns, 0)

    def test_stall(self):
        ring = audiopump.Ring(sample_rate=RATE, channel_count=2, frames=64,
                              capacity=16)
        node = audiopump.DriftResampler(ring)
        packet = sine_packet(1000.0, 64)
        for _ in range(20):
            ring.write(packet)
        for _ in range(16):
            audiocore.get_buffer(node)
        self.assertEqual(node.underruns, 1)
        self.assertEqual(ring.stats()[3], 1)
        for _ in range(12):
            ring.write(packet)
        data = bytes(audiocore.get_buffer(node)[1])
        self.assertNotEqual(data, bytes(len(data)))
        self.assertEqual(node.underruns, 1)

    def test_reset_waits_again(self):
        ring = audiopump.Ring(sample_rate=RATE, channel_count=2, frames=64,
                              capacity=16)
        node = audiopump.DriftResampler(ring)
        packet = sine_packet(1000.0, 64)
        for _ in range(12):
            ring.write(packet)
        self.assertNotEqual(bytes(audiocore.get_buffer(node)[1]), bytes(256))
        node.reset()
        self.assertEqual(bytes(audiocore.get_buffer(node)[1]), bytes(256))
        self.assertEqual(node.ppm, 0.0)


class TestArguments(unittest.TestCase):

    def ring(self, **options):
        settings = dict(sample_rate=RATE, channel_count=2, frames=256,
                        capacity=8)
        settings.update(options)
        return audiopump.Ring(**settings)

    def test_defaults(self):
        node = audiopump.DriftResampler(self.ring())
        self.assertEqual(node.target, 1024)
        self.assertEqual(node.sample_rate, RATE)
        self.assertEqual(node.channel_count, 2)
        self.assertEqual(node.underruns, 0)
        self.assertAlmostEqual(node.rate, 1.0, places=6)
        # Half a two-block Ring is less than a block plus the correction, so
        # the default is the least target that works rather than an error.
        small = audiopump.DriftResampler(self.ring(capacity=2))
        self.assertEqual(small.target, 258)

    def test_bounds(self):
        ring = self.ring()
        for options in (dict(sample_rate=44100), dict(sample_rate=96001),
                        dict(max_ppm=-1), dict(max_ppm=10001),
                        dict(bandwidth=0.0005), dict(bandwidth=0.2),
                        dict(target=100), dict(target=2033)):
            with self.subTest(**options):
                with self.assertRaises(ValueError):
                    audiopump.DriftResampler(ring, **options)
        for frames in (8, 2048):
            with self.subTest(frames=frames):
                with self.assertRaises(ValueError):
                    audiopump.DriftResampler(self.ring(frames=frames,
                                                       capacity=2))
        with self.assertRaises(TypeError):
            audiopump.DriftResampler(object())

    def test_up_to_twice(self):
        node = audiopump.DriftResampler(self.ring(sample_rate=24000),
                                        sample_rate=RATE)
        self.assertAlmostEqual(node.rate, 0.5, places=6)

    def test_target_setter(self):
        node = audiopump.DriftResampler(self.ring())
        # The ring also holds the filter's 16 frames of look-ahead.
        node.target = 2032
        self.assertEqual(node.target, 2032)
        for value in (100, 2033):
            with self.assertRaises(ValueError):
                node.target = value
        self.assertEqual(node.target, 2032)

    def test_deinit(self):
        node = audiopump.DriftResampler(self.ring())
        node.deinit()
        with self.assertRaises(ValueError):
            node.ppm


if __name__ == "__main__":
    unittest.main()
