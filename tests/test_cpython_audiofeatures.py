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
import sys
import unittest
from array import array

import numpy as np

import _audiodsp
import audiofeatures

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data", "audiofeatures")
SIZE = audiofeatures.FEATURE_SIZE


def signal(seconds=3, plan=None):
    """Integer-only 16 kHz test audio: a noise floor, then tone and noise bursts.

    Integers only, so it is the same audio on every platform. Changing it
    invalidates the goldens.
    """
    out = array("h")
    s = 2024
    seg = 16000 // 4
    # (Minsky circle step, 2*pi*f/16000 in Q16; tone level; noise level)
    quarters = seconds * 4
    if plan is None:
        plan = [(0, 0, 300), (0, 0, 300), (7721, 9000, 300), (15442, 4000, 300),
                (3861, 12000, 2000), (0, 0, 9000), (25736, 6000, 600), (0, 0, 300),
                (11581, 15000, 300), (38604, 3000, 4000), (0, 0, 150), (5148, 8000, 1200)]
    else:
        quarters = len(plan)
    for i in range(quarters):
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


QUIET = (0, 0, 300)
#: Quarter-second segments for voice activity: a quiet floor, tone bursts
#: (rising onto the floor and dying away) and a burst of loud noise.
BURSTS = [QUIET] * 4 + [(3861, 12000, 300), (7721, 6000, 300)] + [QUIET] * 6 + \
    [(5148, 8000, 600)] + [QUIET] * 2 + [(0, 0, 6000)] + [QUIET] * 4


def bursts():
    """signal()'s generator over BURSTS: 5 s, integers only."""
    return signal(plan=BURSTS)


def golden(ext, name="signal_16k"):
    with open(os.path.join(DATA, name + "." + ext), "rb") as f:
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


def voice(pcm, rate=16000, block=480, channels=1):
    """Every slice's voice flag, as bytes, fed `block` frames at a time."""
    fe = audiofeatures.Frontend(slices=64)
    buf = bytearray(SIZE * 64)
    flags = bytearray(64)
    out = bytearray()
    mv = memoryview(pcm).cast("B")
    step = block * 2 * channels
    for at in range(0, len(mv), step):
        fe.feed(mv[at:at + step], rate, channels)
        n = fe.readinto(buf, flags)
        out += flags[:n]
    fe.deinit()
    return bytes(out)


def noise(seconds, rms, seed=1):
    """Gaussian noise at `rms`, from numpy's seeded generator."""
    rng = np.random.default_rng(seed)
    return rng.normal(0, rms, int(16000 * seconds))


def pcm(x):
    return np.clip(np.round(x), -32768, 32767).astype("<i2")


def wrong_by(got, want):
    return sum(a != b for a, b in zip(got, want)) + abs(len(got) - len(want))


class VoiceTest(unittest.TestCase):

    def test_goldens(self):
        for name, x in (("signal_16k", signal()), ("bursts_16k", bursts())):
            with self.subTest(name=name):
                want = golden("voice", name)
                for block in (1, 160, 4096):
                    self.assertEqual(voice(x, block=block), want)

    def test_goldens_catch_a_slice_late_or_no_hold(self):
        # The planted faults for test_goldens: a detector one slice late, or
        # one without its hold, must not pass. Both differ in many slices.
        want = golden("voice", "bursts_16k")
        late = b"\x00" + want[:-1]
        self.assertGreater(wrong_by(late, want), 3)
        no_hold = bytearray(want)
        for i in range(len(want)):
            if want[i] and (i + 1 == len(want) or not want[i + 1]):
                no_hold[max(0, i - 9):i + 1] = bytes(i + 1 - max(0, i - 9))
        self.assertGreater(wrong_by(bytes(no_hold), want), 20)

    def test_bursts_are_voice_and_the_floor_between_is_not(self):
        flags = voice(bursts())
        # Slice k covers 10k..10k+30 ms. Each quarter is 25 slices.
        def quarter(q):
            return flags[q * 25 + 3:q * 25 + 22]
        for q, (step, level, noise_level) in enumerate(BURSTS):
            if q < 2:
                continue        # the floor is still being learnt
            loud = level > 0 or noise_level > 1000
            before = q > 0 and (BURSTS[q - 1][1] > 0 or BURSTS[q - 1][2] > 1000)
            if loud:
                self.assertGreater(sum(quarter(q)), 15, q)
            elif not before:
                self.assertEqual(sum(quarter(q)), 0, q)

    def test_steady_noise_is_never_voice(self):
        for rms in (30, 300, 3000):
            with self.subTest(rms=rms):
                flags = voice(pcm(noise(10, rms)))
                self.assertEqual(sum(flags), 0)

    def test_a_noise_that_steps_up_is_learnt(self):
        # 3 s of quiet noise, then 20 dB louder for 10 s: voice at first,
        # since that is what a sudden sound is, then never again.
        x = np.concatenate([noise(3, 100), noise(10, 1000, seed=2)])
        flags = voice(pcm(x))
        self.assertGreater(sum(flags[300:350]), 40)
        self.assertEqual(sum(flags[800:]), 0)   # measured: clear 3.5 s after the step

    def test_digital_silence_then_noise(self):
        # A stream that starts silent and then meets a microphone's floor.
        x = np.concatenate([np.zeros(32000), noise(10, 100)])
        flags = voice(pcm(x))
        self.assertEqual(sum(flags[:200]), 0)
        self.assertEqual(sum(flags[700:]), 0)

    def test_48k_flags_match_16k(self):
        x = np.frombuffer(bursts().tobytes(), "<i2")
        a = voice(x)
        b = voice(upsample(x, 3), rate=48000)
        n = min(len(a), len(b))
        self.assertLessEqual(wrong_by(a[:n], b[:n]), 4)

    def test_speaking_is_the_newest_flag(self):
        fe = audiofeatures.Frontend(slices=1000)
        x = bursts()
        mv = memoryview(x).cast("B")
        buf = bytearray(SIZE * 1000)
        flags = bytearray(1000)
        seen = []
        for at in range(0, len(mv), 320):
            fe.feed(mv[at:at + 320], 16000)
            seen.append(fe.speaking())
        n = fe.readinto(buf, flags)
        self.assertEqual(fe.speaking(), bool(flags[n - 1]))
        self.assertTrue(any(seen) and not all(seen))

    def test_voice_buffer_limits_the_read(self):
        fe = audiofeatures.Frontend()
        fe.feed(bytes(32000), 16000)
        flags = bytearray(5)
        self.assertEqual(fe.readinto(bytearray(SIZE * 10), flags), 5)
        self.assertEqual(fe.readinto_raw(array("H", bytes(4 * SIZE)), flags), 2)
        self.assertEqual(fe.readinto(bytearray(SIZE), None), 1)
        self.assertEqual(fe.stats()["waiting"], 90)

    def test_reset_forgets_the_floor(self):
        fe = audiofeatures.Frontend()
        fe.feed(pcm(noise(1, 3000)).tobytes(), 16000)
        fe.reset()
        self.assertFalse(fe.speaking())
        fe.feed(pcm(noise(1, 3000)).tobytes(), 16000)
        flags = bytearray(100)
        fe.readinto(bytearray(SIZE * 100), flags)
        self.assertEqual(sum(flags), 0)


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


@unittest.skipUnless(sys.platform.startswith("linux") and shutil.which("nm"), "needs GNU nm and an ELF build")
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
