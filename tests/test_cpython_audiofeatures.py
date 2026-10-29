"""audiofeatures: the frontend's bytes, the resampler, and the surface.

The bytes. tests/data/audiofeatures/signal_16k.u16 and .s8 are the features
TensorFlow Lite Micro's own microfrontend makes from ``signal()`` at 16 kHz:
the library at tflite-micro commit 230c9d9531bd9b08d445b4f2ed0760756ffd5f78,
built from its own C++ FFT files (fft.cc, fft_util.cc, kiss_fft_int16.cc)
rather than this repository's C, set up with ESPHome's micro_wake_word
settings, and quantized with ESPHome's integer scaling. They were written by
that build, not by this module, so a match is a match with the reference, not
with our own last answer. 16 kHz input skips the resampler, so the match must
be exact, in every block size.

The resampler. Audio at another rate goes through a low-pass to 16 kHz first,
so its features can't be byte-identical; the bar is that a band-limited
signal fed at 48 kHz (and 44.1 kHz, the interpolated path) makes nearly the
features the same signal makes at 16 kHz, and that a tone above 8.5 kHz, which
the resampler must remove, adds nothing.
"""

import os
import shutil
import subprocess
import unittest
from array import array

import numpy as np

import _audiodsp
import audiofeatures

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data", "audiofeatures")
SIZE = audiofeatures.FEATURE_SIZE


def signal(seconds=3):
    """Integer-only 16 kHz test audio: a noise floor, then tone and noise bursts.

    Integers only, so it is the same audio on every platform. Changing it
    invalidates the goldens.
    """
    out = array("h")
    s = 2024
    seg = 16000 // 4
    # (Minsky circle step, 2*pi*f/16000 in Q16; tone level; noise level)
    plan = [(0, 0, 300), (0, 0, 300), (7721, 9000, 300), (15442, 4000, 300),
            (3861, 12000, 2000), (0, 0, 9000), (25736, 6000, 600), (0, 0, 300),
            (11581, 15000, 300), (38604, 3000, 4000), (0, 0, 150), (5148, 8000, 1200)]
    for i in range(seconds * 4):
        step, level, noise = plan[i % len(plan)]
        x, y = level << 8, 0
        for n in range(seg):
            env = n * 4 if n < seg // 4 else seg - (n - seg // 4) * 4 // 3
            if env > seg:
                env = seg
            if env < 0:
                env = 0
            x -= (y * step) >> 16
            y += (x * step) >> 16
            s = (s * 1103515245 + 12345) & 0x7FFFFFFF
            r = ((s >> 15) & 0xFFFF) - 32768
            v = ((x >> 8) * env) // seg + r * noise // 32768
            out.append(max(-32768, min(32767, v)))
    return out


def golden(ext):
    with open(os.path.join(DATA, "signal_16k." + ext), "rb") as f:
        return f.read()


def run(pcm, rate, block, channels=1, raw=True):
    """Every slice the frontend makes from pcm, fed `block` frames at a time."""
    fe = audiofeatures.Frontend(slices=64)
    width = 2 * SIZE if raw else SIZE
    buf = bytearray(width * 64)
    out = bytearray()
    mv = memoryview(pcm).cast("B")
    step = block * 2 * channels
    for at in range(0, len(mv), step):
        fe.feed(mv[at:at + step], rate, channels)
        n = fe.readinto_raw(buf) if raw else fe.readinto(buf)
        out += buf[:n * width]
    st = fe.stats()
    fe.deinit()
    assert st["dropped"] == 0, st
    return bytes(out)


def upsample(x, factor_num, factor_den=1):
    """Band-limited resampling by FFT: the same audio at another rate."""
    x = np.asarray(x, dtype=np.float64)
    n = len(x)
    m = n * factor_num // factor_den
    spec = np.fft.rfft(x)
    out = np.zeros(m // 2 + 1, dtype=complex)
    k = min(len(spec), len(out))
    out[:k] = spec[:k]
    y = np.fft.irfft(out, m) * (m / n)
    return np.clip(np.round(y), -32768, 32767).astype("<i2")


class GoldenTest(unittest.TestCase):
    """16 kHz goes straight to the frontend, so the bytes must be the reference's."""

    def test_raw_matches_reference(self):
        pcm = signal()
        want = golden("u16")
        for block in (1, 37, 160, 480, 4096):
            with self.subTest(block=block):
                self.assertEqual(run(pcm, 16000, block), want)

    def test_int8_matches_reference(self):
        self.assertEqual(run(signal(), 16000, 160, raw=False), golden("s8"))

    def test_quantize_matches_reference(self):
        raw = np.frombuffer(golden("u16"), "<u2")
        q = bytes(audiofeatures.quantize(int(v)) & 0xFF for v in raw)
        self.assertEqual(q, golden("s8"))

    def test_stereo_of_one_channel_is_mono(self):
        mono = np.frombuffer(signal().tobytes(), "<i2")
        stereo = np.repeat(mono, 2).astype("<i2")
        self.assertEqual(run(stereo.tobytes(), 16000, 160, channels=2), golden("u16"))


class ResamplerTest(unittest.TestCase):

    def features(self, pcm, rate):
        return np.frombuffer(run(pcm, rate, 480, raw=False), np.int8).reshape(-1, SIZE).astype(int)

    def compare(self, rate, num, den=1):
        x = np.frombuffer(signal().tobytes(), "<i2")
        want = np.frombuffer(golden("s8"), np.int8).reshape(-1, SIZE).astype(int)
        got = self.features(upsample(x, num, den).tobytes(), rate)
        n = min(len(want), len(got))
        self.assertLessEqual(abs(len(want) - len(got)), 1)
        diff = np.abs(want[:n] - got[:n])
        return diff

    def test_48k_is_close(self):
        diff = self.compare(48000, 3)
        # int8 steps; the model's input is coarse, and these bars sit well
        # above what the resampler leaves (mean under a step).
        self.assertLess(diff.mean(), 1.0)
        self.assertLessEqual(np.percentile(diff, 99), 6)

    def test_44k1_is_close(self):
        diff = self.compare(44100, 441, 160)
        self.assertLess(diff.mean(), 1.0)
        self.assertLessEqual(np.percentile(diff, 99), 6)

    def with_and_without_tone(self, freq):
        """The test signal at 48 kHz, alone and under a loud tone at freq."""
        x = upsample(np.frombuffer(signal().tobytes(), "<i2"), 3).astype(np.float64)
        tone = np.sin(2 * np.pi * freq * np.arange(len(x)) / 48000) * 12000
        quiet = np.round(x).astype("<i2")
        loud = np.clip(np.round(x + tone), -32768, 32767).astype("<i2")
        return quiet, loud

    def test_out_of_band_tone_is_removed(self):
        # A loud tone at 9 or 12 kHz would alias to 7 or 4 kHz without the
        # filter. With it, what is left is a sample's rounding here and there,
        # which the frontend's gain control can still lift in a quiet channel:
        # measured 0.14 and 0.18 int8 steps on average.
        for freq in (9000, 12000):
            with self.subTest(freq=freq):
                quiet, loud = self.with_and_without_tone(freq)
                diff = np.abs(self.features(quiet.tobytes(), 48000) - self.features(loud.tobytes(), 48000))
                self.assertLess(diff.mean(), 0.5)

    def test_plain_decimation_would_fail_that(self):
        # The planted fault for the test above: every third sample, no filter
        # (measured 3.4 and 4.5 steps on average).
        for freq in (9000, 12000):
            with self.subTest(freq=freq):
                quiet, loud = self.with_and_without_tone(freq)
                a = self.features(quiet[::3].copy().tobytes(), 16000)
                b = self.features(loud[::3].copy().tobytes(), 16000)
                self.assertGreater(np.abs(a - b).mean(), 2.0)

    def test_rate_change_restarts_cleanly(self):
        fe = audiofeatures.Frontend()
        fe.feed(bytes(9600), 48000)
        self.assertTrue(fe.stats()["resampling"])
        fe.feed(bytes(3200), 16000)
        self.assertFalse(fe.stats()["resampling"])
        self.assertEqual(fe.stats()["rate"], 16000)


class SurfaceTest(unittest.TestCase):

    def test_constants(self):
        self.assertEqual(audiofeatures.FEATURE_SIZE, 40)
        self.assertEqual(audiofeatures.SAMPLE_RATE, 16000)
        self.assertEqual(audiofeatures.STEP_MS, 10)

    def test_quantize_ends(self):
        self.assertEqual(audiofeatures.quantize(0), -128)
        self.assertEqual(audiofeatures.quantize(333), 0)
        self.assertEqual(audiofeatures.quantize(666), 127)
        self.assertEqual(audiofeatures.quantize(65535), 127)
        with self.assertRaises(ValueError):
            audiofeatures.quantize(-1)

    def test_a_second_makes_a_hundred_slices(self):
        fe = audiofeatures.Frontend(slices=200)
        # The first slice needs a whole 30 ms window, then one every 10 ms.
        self.assertEqual(fe.feed(bytes(32000), 16000), 98)

    def test_readinto_takes_whole_slices(self):
        fe = audiofeatures.Frontend()
        fe.feed(bytes(32000), 16000)
        buf = bytearray(SIZE * 3 + 7)
        self.assertEqual(fe.readinto(buf), 3)
        raw = array("H", bytes(2 * SIZE * 2))
        self.assertEqual(fe.readinto_raw(raw), 2)
        self.assertEqual(fe.stats()["waiting"], 93)

    def test_a_full_queue_drops_the_oldest(self):
        fe = audiofeatures.Frontend(slices=10)
        fe.feed(bytes(32000), 16000)
        st = fe.stats()
        self.assertEqual((st["waiting"], st["dropped"], st["slices"]), (10, 88, 98))

    def test_reset(self):
        fe = audiofeatures.Frontend()
        fe.feed(bytes(32000), 16000)
        fe.reset()
        self.assertEqual(fe.stats()["waiting"], 0)
        self.assertEqual(fe.feed(bytes(32000), 16000), 98)

    def test_refusals(self):
        fe = audiofeatures.Frontend()
        with self.assertRaises(ValueError):
            fe.feed(bytes(320), 8000)
        with self.assertRaises(ValueError):
            fe.feed(bytes(3), 16000)
        with self.assertRaises(ValueError):
            fe.feed(bytes(6), 16000, 2)
        with self.assertRaises(ValueError):
            audiofeatures.Frontend(slices=2)
        with self.assertRaises(NotImplementedError):
            fe.attach(None, 48000)
        fe.deinit()
        with self.assertRaises(ValueError):
            fe.feed(bytes(320), 16000)

    def test_context_manager(self):
        with audiofeatures.Frontend() as fe:
            fe.feed(bytes(320), 16000)
        with self.assertRaises(ValueError):
            fe.stats()


@unittest.skipUnless(shutil.which("nm") and _audiodsp.__file__.endswith(".so"), "needs nm and an ELF build")
class SymbolTest(unittest.TestCase):
    """Every symbol of the microfrontend and kissfft is renamed, so another copy links."""

    def test_no_library_symbol_escapes(self):
        out = subprocess.run(["nm", "-D", "--defined-only", _audiodsp.__file__],
                             capture_output=True, text=True, check=True).stdout
        names = {line.split()[-1] for line in out.splitlines() if line.strip()}
        prefixes = ("Frontend", "Window", "Fft", "Filterbank", "NoiseReduction", "Pcan", "LogScale",
                    "WideDynamic", "kLogLut", "kiss_fft")
        escaped = sorted(n for n in names if n.startswith(prefixes))
        self.assertEqual(escaped, [])
        self.assertIn("audiodsp_mf_FrontendProcessSamples", names)


if __name__ == "__main__":
    unittest.main()
