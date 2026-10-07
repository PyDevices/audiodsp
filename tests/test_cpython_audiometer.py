"""audiometer: that its levels mean what they say, and the surface.

That every interpreter reads the same bytes is tests/parity/meter_probe.py's
job, through verify_dsp.py. A comparison can't see a meter that is wrong the
same way everywhere; these traits can, because each is measured against the
signal rather than against our own last answer. The bars, fixed before the
first run (media modules roadmap, Gate 6):

| trait                                   | bar                                        |
|-----------------------------------------|--------------------------------------------|
| a sine at a band's centre               | reads highest in that band (six bands, 40 Hz to 18 kHz) |
| pink noise, 48 bands 35 Hz..20 kHz      | every band within 3 dB of the median, mean of 10 s after 0.5 s, three seeds |
| white noise, the same measure           | fails it (spread over 10 dB): the planted fault |
| silence                                 | every band, peak and RMS read 0            |
| a -6 dB sine                            | peak 188 and RMS 182, within one step      |
| block size, and mono against stereo     | the same bytes                             |

The pink row was restated in its first run (2026-10-06). The bar said 2 s, and
the meter as it came from usbif failed it: bands one to three bins wide sat
3-4 dB low at every length and seed, because a band took whole bins only. With
the edge bins weighted by how much of them lies in the band, the error left is
under 1 dB at 40 s; but at 2 s one seed's own noise still put a band 3.3 dB
out, so 2 s was measuring the sample rather than the meter. Hence 10 s.
"""

import math
import unittest
from array import array

import numpy as np

import audiometer

RATE = 48000


def stereo(samples):
    """float samples in -1..1 to interleaved s16, both channels the same"""
    s = np.clip(np.round(np.asarray(samples) * 32767), -32768, 32767).astype("<i2")
    return np.repeat(s, 2).tobytes()


def sine(freq, seconds=1.0, amp=0.5):
    t = np.arange(int(RATE * seconds)) / RATE
    return amp * np.sin(2 * np.pi * freq * t)


def coloured(slope_db_per_octave, seconds, seed):
    """Gaussian noise shaped in the frequency domain: 0 is white, -3 pink."""
    n = int(RATE * seconds)
    rng = np.random.default_rng(seed)
    spec = rng.normal(size=n // 2 + 1) + 1j * rng.normal(size=n // 2 + 1)
    f = np.fft.rfftfreq(n, 1 / RATE)
    f[0] = f[1]
    spec *= f ** (slope_db_per_octave / (20 * math.log10(2)))
    x = np.fft.irfft(spec, n)
    return x / np.max(np.abs(x)) * 0.5


def feed_all(meter, pcm, block=480, channels=2):
    """Feed in blocks; every fresh analysis's levels, in order."""
    out = []
    frame = 2 * channels
    for at in range(0, len(pcm), block * frame):
        meter.feed(pcm[at:at + block * frame], RATE, channels)
        seq, lv, pk, rms = meter.levels()
        if seq and (not out or out[-1][0] != seq):
            out.append((seq, bytes(lv), pk, rms))
    return out


def centre(lo, hi, bands, i):
    r = (hi / lo) ** (1 / bands)
    return lo * r ** (i + 0.5)


def spread_db(seconds, slope, seed=7):
    """Each band's mean level after the first 0.5 s, in dB from the median band."""
    m = audiometer.Meter(48, low_hz=35, high_hz=20000)
    seen = feed_all(m, stereo(coloured(slope, seconds, seed=seed)))
    settle = int(0.5 * 60)
    lv = np.array([list(s[1]) for s in seen[settle:]], dtype=float).mean(axis=0) / 2
    return lv - np.median(lv)


class LevelsTest(unittest.TestCase):
    def test_a_sine_reads_highest_in_its_band(self):
        for band in (0, 6, 12, 18, 24, 31):
            f = centre(35, 20000, 32, band)
            m = audiometer.Meter(32, low_hz=35, high_hz=20000)
            seen = feed_all(m, stereo(sine(f)))
            levels = list(seen[-1][1])
            self.assertEqual(levels.index(max(levels)), band, "%.0f Hz: %r" % (f, levels))

    def test_pink_noise_reads_flat(self):
        for seed in (7, 8, 9):
            d = spread_db(10.5, -3.0, seed)
            self.assertLessEqual(np.max(np.abs(d)), 3.0, (seed, np.round(d, 1)))

    def test_white_noise_does_not(self):
        # the planted fault: the flatness bar has to be able to fail
        d = spread_db(10.5, 0.0)
        self.assertGreater(np.max(d) - np.min(d), 10.0, np.round(d, 1))

    def test_silence_reads_the_floor(self):
        m = audiometer.Meter(32)
        seen = feed_all(m, bytes(4 * RATE))
        self.assertTrue(seen)
        for seq, lv, pk, rms in seen:
            self.assertEqual((set(lv), pk, rms), ({0}, 0, 0))

    def test_peak_and_rms_of_a_half_scale_sine(self):
        m = audiometer.Meter(16)
        seq, lv, pk, rms = feed_all(m, stereo(sine(1000.0)))[-1]
        self.assertLessEqual(abs(pk - 188), 1)
        self.assertLessEqual(abs(rms - 182), 1)

    def test_block_size_and_channels_do_not_move_a_byte(self):
        pcm = stereo(coloured(-3.0, 1.0, seed=3))
        a = feed_all(audiometer.Meter(24), pcm, block=480)
        b = feed_all(audiometer.Meter(24), pcm, block=37)
        self.assertEqual([s[1:] for s in a[::5]], [s[1:] for s in b[::5]])
        mono = array("h", pcm)[::2].tobytes()
        c = feed_all(audiometer.Meter(24), mono, block=480, channels=1)
        self.assertEqual([s[1:] for s in a], [s[1:] for s in c])


class SurfaceTest(unittest.TestCase):
    def test_bad_arguments(self):
        with self.assertRaises(ValueError):
            audiometer.Meter(audiometer.MAX_BANDS + 1)
        with self.assertRaises(ValueError):
            audiometer.Meter(8, low_hz=1000, high_hz=500)
        m = audiometer.Meter(8)
        with self.assertRaises(ValueError):
            m.feed(b"\x00" * 6, RATE, 2)

    def test_off_stops_the_count(self):
        m = audiometer.Meter(8)
        feed_all(m, bytes(4 * RATE // 10))
        m.configure(0)
        before = m.levels()[0]
        feed_all(m, bytes(4 * RATE // 10))
        self.assertEqual(m.levels()[0], before)
        self.assertFalse(m.stats()["enabled"])

    def test_levels_into_a_buffer(self):
        m = audiometer.Meter(8)
        feed_all(m, stereo(sine(440.0, 0.2)))
        buf = bytearray(8)
        seq, lv, pk, rms = m.levels(buf)
        self.assertIs(lv, buf)
        self.assertEqual(bytes(buf), m.levels()[1])

    def test_attach_needs_a_board(self):
        with self.assertRaises(NotImplementedError):
            audiometer.Meter(8).attach(audiometer.UAC)

    def test_deinit(self):
        m = audiometer.Meter(8)
        m.deinit()
        with self.assertRaises(ValueError):
            m.levels()

    def test_db_byte(self):
        self.assertEqual(audiometer.db_byte(1.0), 200)
        self.assertEqual(audiometer.db_byte(0.5), 194)
        self.assertEqual(audiometer.db_byte(1e-11), 0)


if __name__ == "__main__":
    unittest.main()
