"""audiooscillator: the traits the parity gate cannot see.

`verify_dsp.py` pins what this node renders, byte for byte, on every
interpreter. It would be just as green if the band-limiting had been left out
or the sync wired to nothing, so the traits the module exists for are asserted
here against arithmetic rather than against our own last answer:

| trait | bar |
| --- | --- |
| a saw's and a pulse's aliases below 10 kHz, at 4567 Hz | 45 dB under the harmonics (a naive saw is 14) |
| a triangle's, the same way | 60 dB under (a naive one is 36) |
| a 440 Hz saw's first eight harmonics | within 0.5 dB of 1/k |
| a saw synced to another Oscillator | within 0.003 of the analytic synced saw, away from its edges |
| linear FM, PWM and pitch | read every sample: the slope, the duty and the period follow the input inside one block |
| `buffer_size` | the blocks it asks for, and the same audio as the default |

Each band-limiting and sync row has a control beside it that must fail the
same bar, so the measurement is shown to tell the difference.
"""

import math
import unittest
from array import array

import numpy as np

import audiocore
import audiooscillator as ao

RATE = 48000


def render(node, frames):
    blocks = []
    got = 0
    while got < frames:
        data = bytes(audiocore.get_buffer(node)[1])
        block = np.frombuffer(data, dtype=np.int16)
        blocks.append(block)
        got += len(block) // node.channel_count
    return np.concatenate(blocks)[:frames * node.channel_count]


def table(values):
    return audiocore.RawSample(array("h", [int(v) for v in values]),
                               sample_rate=RATE, channel_count=1)


def alias_db(samples, hz, below=10000.0):
    """Energy outside the harmonics of `hz` and under `below`, against the
    energy in them, in dB. Blackman-windowed, so leakage sits far under the
    bars."""
    x = np.asarray(samples, dtype=float)
    spectrum = np.abs(np.fft.rfft(x * np.blackman(len(x)))) ** 2
    freqs = np.fft.rfftfreq(len(x), 1.0 / RATE)
    harmonic = np.zeros(len(freqs), dtype=bool)
    k = 1
    while k * hz < RATE / 2:
        harmonic |= np.abs(freqs - k * hz) < 60.0
        k += 1
    alias = spectrum[~harmonic & (freqs > 30.0) & (freqs < below)].sum()
    return 10.0 * math.log10(alias / spectrum[harmonic].sum())


def naive(shape, hz, frames):
    phase = (np.arange(frames) * hz / RATE) % 1.0
    if shape == "saw":
        return np.round((2.0 * phase - 1.0) * 16384.0)
    if shape == "pulse":
        return np.where(phase < 0.5, 16384.0, -16384.0)
    return np.round(np.where(phase < 0.5, 4 * phase - 1, 3 - 4 * phase)
                    * 16384.0)


class BandLimitingTest(unittest.TestCase):
    HZ = 4567.0
    FRAMES = 1 << 15

    def measure(self, waveform):
        node = ao.Oscillator(sample_rate=RATE, frequency=self.HZ,
                             waveform=waveform, level=0.5)
        return alias_db(render(node, self.FRAMES), self.HZ)

    def test_the_saw_and_the_pulse_are_band_limited(self):
        self.assertLess(self.measure(ao.SAW), -45.0)
        self.assertLess(self.measure(ao.PULSE), -45.0)

    def test_the_triangle_is_band_limited(self):
        self.assertLess(self.measure(ao.TRIANGLE), -60.0)

    def test_the_measurement_sees_aliasing(self):
        # The control: the same waveforms computed naively fail both bars.
        for shape, bar in (("saw", -45.0), ("pulse", -45.0),
                           ("triangle", -60.0)):
            self.assertGreater(
                alias_db(naive(shape, self.HZ, self.FRAMES), self.HZ), bar,
                shape)

    def test_a_saw_has_a_saws_harmonics(self):
        hz = 440.0
        node = ao.Oscillator(sample_rate=RATE, frequency=hz, waveform=ao.SAW,
                             level=0.5)
        x = render(node, RATE).astype(float)
        spectrum = np.abs(np.fft.rfft(x * np.hanning(len(x))))
        first = spectrum[int(hz)]
        for k in range(2, 9):
            ratio = 20.0 * math.log10(spectrum[int(k * hz)] / first)
            self.assertAlmostEqual(ratio, 20.0 * math.log10(1.0 / k),
                                   delta=0.5, msg="harmonic %d" % k)


class SyncTest(unittest.TestCase):
    MASTER = 110.3
    SLAVE = 377.7
    FRAMES = 8192

    def synced(self, sync=True):
        master = ao.Oscillator(sample_rate=RATE, frequency=self.MASTER,
                               waveform=ao.PULSE)
        slave = ao.Oscillator(sample_rate=RATE, frequency=self.SLAVE,
                              waveform=ao.SAW, level=0.5,
                              sync=master if sync else None)
        return render(slave, self.FRAMES) / 16384.0

    def error_away_from_edges(self, output):
        # The node hands each sample out one frame late (its band-limiting
        # corrects the sample before an edge), so frame n is time n - 1.
        time = np.arange(self.FRAMES) - 1.0
        period = RATE / self.MASTER
        edges = np.arange(0.0, self.FRAMES + period, period)
        last = edges[np.searchsorted(edges, time, side="right") - 1]
        ideal = 2.0 * (((time - last) * self.SLAVE / RATE) % 1.0) - 1.0
        events = list(edges)
        for edge in edges:
            wrap = edge + RATE / self.SLAVE
            while wrap < edge + period:
                events.append(wrap)
                wrap += RATE / self.SLAVE
        events = np.array(events)
        distance = np.min(np.abs(time[:, None] - events[None, :]), axis=1)
        # From the second master period: the first edge is the master
        # starting high, which the slave reads one frame early.
        keep = (distance > 2.0) & (time > period)
        return np.abs(output - ideal)[keep].max()

    def test_it_resets_at_the_masters_edges(self):
        self.assertLess(self.error_away_from_edges(self.synced()), 0.003)

    def test_the_measurement_sees_a_missing_sync(self):
        self.assertGreater(self.error_away_from_edges(self.synced(False)),
                           0.1)

    def test_a_sync_jump_is_band_limited(self):
        # A synced saw makes a step of a different height every period; the
        # aliasing bar is the free-running saw's.
        master = ao.Oscillator(sample_rate=RATE, frequency=1001.0,
                               waveform=ao.PULSE)
        slave = ao.Oscillator(sample_rate=RATE, frequency=2777.0,
                              waveform=ao.SAW, level=0.5, sync=master)
        x = render(slave, 1 << 15)
        self.assertLess(alias_db(x, 1001.0), -40.0)


class AudioRateInputTest(unittest.TestCase):
    def test_linear_fm_is_read_every_sample(self):
        frames, base, depth = 256, 2000.0, 1500.0
        modulator = [int(30000 * math.sin(2 * math.pi * n / 64))
                     for n in range(frames)]
        node = ao.Oscillator(sample_rate=RATE, frequency=base,
                             waveform=ao.SAW, level=0.5, fm_depth=depth,
                             fm=table(modulator))
        y = render(node, frames).astype(float) / 16384.0
        slope = np.diff(y)
        expected = 2.0 * (base + depth * np.array(modulator) / 32768.0) / RATE
        # Output frame n + 1 minus frame n is the step the input at frame n
        # set. A wrap is a big negative step, and the band-limiting bends the
        # steps either side of it, so those are left out.
        wraps = np.where(slope < 0)[0]
        rising = np.ones(len(slope), dtype=bool)
        for offset in range(-2, 3):
            rising[np.clip(wraps + offset, 0, len(slope) - 1)] = False
        error = np.abs(slope - expected[:-1])[rising]
        self.assertGreater(rising.sum(), 150)
        self.assertLess(error.max(), 2e-4)

    def test_the_pulse_width_follows_pwm_inside_a_block(self):
        frames, hz = 256, 1000.0
        pwm = [-16384] * 128 + [16384] * 128
        node = ao.Oscillator(sample_rate=RATE, frequency=hz,
                             waveform=ao.PULSE, pulse_width=0.5,
                             pwm_depth=0.6, pwm=table(pwm))
        y = render(node, frames)
        self.assertEqual(len(audiocore.get_buffer(node)[1]), 512)
        first = (y[1:97] > 0).mean()
        second = (y[145:241] > 0).mean()
        self.assertAlmostEqual(first, 0.2, delta=0.03)
        self.assertAlmostEqual(second, 0.8, delta=0.03)

    def test_pitch_moves_inside_a_block(self):
        frames, hz = 256, 1500.0
        pitch = [0] * 128 + [3277] * 128   # a tenth of full scale: an octave
        node = ao.Oscillator(sample_rate=RATE, frequency=hz, waveform=ao.SAW,
                             level=0.5, pitch=table(pitch))
        y = render(node, frames).astype(float) / 16384.0
        slope = np.diff(y)
        self.assertAlmostEqual(np.median(slope[8:120]), 2 * hz / RATE,
                               delta=1e-4)
        self.assertAlmostEqual(np.median(slope[136:250]), 4 * hz / RATE,
                               delta=1e-4)

    def test_a_disconnected_input_counts_as_zero(self):
        node = ao.Oscillator(sample_rate=RATE, frequency=1500.0,
                             pitch=table([3277] * 64))
        node.modulate(pitch=None)
        plain = ao.Oscillator(sample_rate=RATE, frequency=1500.0)
        self.assertTrue(np.array_equal(render(node, 512), render(plain, 512)))


class SurfaceTest(unittest.TestCase):
    def test_buffer_size_sets_the_block_and_not_the_audio(self):
        default = ao.Oscillator(sample_rate=RATE, frequency=987.0,
                                waveform=ao.PULSE, pwm=table(range(-9000,
                                                                   9000, 70)))
        probe = ao.Oscillator(sample_rate=RATE, buffer_size=64 * 2)
        self.assertEqual(len(audiocore.get_buffer(probe)[1]), 128)
        small = ao.Oscillator(sample_rate=RATE, frequency=987.0,
                              waveform=ao.PULSE, buffer_size=64 * 2,
                              pwm=table(range(-9000, 9000, 70)))
        self.assertTrue(np.array_equal(render(default, 1024),
                                       render(small, 1024)))

    def test_a_stereo_pair_shares_one_phase(self):
        node = ao.Oscillator(sample_rate=RATE, channel_count=2,
                             frequency=1000.0, waveform=ao.SAW,
                             waveform_right=ao.PULSE, level=0.5)
        y = render(node, 960).reshape(-1, 2)
        saw_resets = np.where(np.diff(y[:, 0].astype(int)) < -8000)[0]
        pulse_rises = np.where(np.diff(y[:, 1].astype(int)) > 8000)[0]
        self.assertEqual(len(saw_resets), len(pulse_rises))
        self.assertTrue(np.all(np.abs(saw_resets - pulse_rises) <= 1))

    def test_waveforms_add(self):
        both = ao.Oscillator(sample_rate=RATE, frequency=300.0,
                             waveform=ao.SAW + ao.SINE, level=0.25)
        saw = ao.Oscillator(sample_rate=RATE, frequency=300.0,
                            waveform=ao.SAW, level=0.25)
        sine = ao.Oscillator(sample_rate=RATE, frequency=300.0,
                             waveform=ao.SINE, level=0.25)
        total = render(saw, 512).astype(int) + render(sine, 512)
        self.assertLessEqual(np.abs(render(both, 512) - total).max(), 1)

    def test_it_refuses_what_it_cannot_do(self):
        with self.assertRaises(ValueError):
            ao.Oscillator(buffer_size=3)
        with self.assertRaises(ValueError):
            ao.Oscillator(buffer_size=257 * 2)
        with self.assertRaises(ValueError):
            ao.Oscillator(channel_count=3)
        with self.assertRaises(TypeError):
            ao.Oscillator(colour=1)
        with self.assertRaises(TypeError):
            ao.Oscillator().modulate(volume=None)

    def test_clear_starts_it_again(self):
        node = ao.Oscillator(sample_rate=RATE, frequency=700.0,
                             waveform=ao.TRIANGLE)
        first = render(node, 256)
        render(node, 300)
        node.clear()
        self.assertTrue(np.array_equal(render(node, 256), first))


if __name__ == "__main__":
    unittest.main()
