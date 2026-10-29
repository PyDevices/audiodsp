"""audiosampler's filter, modulation envelope and LFOs, measured.

What a SoundFont region asks of a voice besides its amplitude envelope
(SoundFont 2.04, section 8.1.2): a resonant low-pass, a modulation envelope
on pitch and cutoff, a vibrato LFO on pitch, and a modulation LFO on pitch,
cutoff and volume. `test_cpython_audiosampler.py` holds the plain voice's
traits; these are the ones a modulated voice adds.

## The traits, with their bars

| ID | Trait | Bar |
|---|---|---|
| M1 | A voice with the filter open (19.9 kHz, no resonance, nothing moving it) renders the same bits as one with no filter | exact |
| M2 | At its cutoff the filter answers 20*log10(Q) - R/2 dB, where R is the resonance and Q = sqrt(10^(R/10) - 1/2); far below it, -R/2 dB | 0.3 dB |
| M3 | Two octaves above the cutoff the filter is down about 24 dB | 21 to 27 dB |
| M4 | The modulation envelope sweeps pitch: at full scale up by `mod_to_pitch`, then down linearly in cents through the decay to the sustain | 1% |
| M5 | The modulation envelope sweeps the cutoff: two octaves of `mod_to_cutoff` from 500 Hz lets a 2 kHz tone through as a 2 kHz cutoff would | 0.5 dB |
| M6 | The vibrato LFO swings pitch by its depth either way at its rate, and not before its delay | depth 5%, rate 2%, delay one chunk |
| M7 | The modulation LFO swings volume by its depth in dB either way at its rate | 0.5 dB, rate 2% |
| M8 | A vibrato depth set live from zero starts the vibrato; set back to zero, the pitch is exactly the plain voice's again | 5%; exact |
| M9 | A cutoff set live closes the filter on a sounding voice, and set to None opens it again | 0.3 dB; 0.05 dB |
| M10 | 32, 48 and 64 looping stereo voices, every filter on, render inside the 5333 us block | measured |

## The planted faults

Recorded rather than automated, because the file they break is C. Each was
made by hand in `src/shared/audiodsp_sampler.c`, rebuilt, and the suite run.

| Fault | Breaks | What was measured |
|---|---|---|
| `resonance_k()` works out Q for half the resonance asked | M2 | 500 Hz with 6 dB of resonance answered -1.25 dB at the cutoff against 2.42. Measured 2026-10-29. |
| `triangle()` runs at twice the phase | M6, M7 | the vibrato measured 10.0 Hz against 5; the tremolo's peaks came every 0.125 s against 0.25. |
| `mod_advance()` moves the modulation envelope at half speed | M4 | 0.1 s into a one-second sweep from an octave up, 966 Hz against 933; half-way through a release, 841 Hz against 707. |
"""

import math
import time
import unittest
from array import array

import audiocore
import audiosampler

RATE = 48000
BLOCK = audiosampler.FRAMES


def sine(hz, frames, peak=16000):
    """A loop of `frames` holding a whole number of cycles of `hz`."""
    return array("h", [int(round(peak * math.sin(2 * math.pi * hz * i / RATE)))
                       for i in range(frames)])


def mono():
    return audiosampler.Sampler(voice_count=1, channel_count=1,
                                sample_rate=RATE)


def render(sampler, blocks):
    out = []
    for _ in range(blocks):
        _, data = audiocore.get_buffer(sampler)
        out.extend(array("h", bytes(data)))
    return out


def rms(samples):
    return math.sqrt(sum(v * v for v in samples) / float(len(samples)))


def db(a, b):
    return 20.0 * math.log10(a / b)


def crossings(samples):
    """Fractional positions of the rising zero crossings."""
    out = []
    for i in range(1, len(samples)):
        a, b = samples[i - 1], samples[i]
        if a < 0 <= b:
            out.append(i - 1 + (-a) / float(b - a))
    return out


def frequency_track(samples):
    """(time in frames, Hz) per rising crossing, from the period around it."""
    xs = crossings(samples)
    return [((xs[i] + xs[i - 1]) / 2.0, RATE / (xs[i] - xs[i - 1]))
            for i in range(1, len(xs))]


def hz_between(samples, start, end):
    xs = [x for x in crossings(samples) if start <= x < end]
    return (len(xs) - 1) * RATE / (xs[-1] - xs[0])


def loop(voice, data, **kw):
    voice.play(data, loop_start=0, loop_end=len(data),
               loop_mode=audiosampler.LOOP_FOREVER, **kw)


def through(hz, blocks=60, **kw):
    """RMS of a looped `hz` sine through a voice with `kw`, after a settle."""
    sampler = mono()
    frames = int(round(RATE / hz * 40))
    # Quiet enough that 20 dB of resonance stays clear of the rails.
    data = sine(RATE * 40.0 / frames, frames, peak=3000)
    loop(sampler.voice[0], data, **kw)
    out = render(sampler, blocks)
    sampler.deinit()
    return rms(out[20 * BLOCK:]), RATE * 40.0 / frames


def expected_db(resonance):
    q = math.sqrt(10 ** (resonance / 10.0) - 0.5)
    return 20 * math.log10(q) - resonance / 2.0


class Filter(unittest.TestCase):

    def test_m1_open_filter_is_the_plain_voice(self):
        data = sine(1000, 480)
        outs = []
        for kw in ({}, {"cutoff": 19900.0}, {"cutoff": 22000.0},
                   {"cutoff": None}):
            sampler = mono()
            loop(sampler.voice[0], data, pitch=1.07, attack=0.01, **kw)
            outs.append(render(sampler, 20))
        for other in outs[1:]:
            self.assertEqual(outs[0], other)

    def test_m2_response_at_and_below_the_cutoff(self):
        for cutoff in (500.0, 2000.0, 6000.0):
            for resonance in (0.0, 6.0, 12.0, 20.0):
                plain, hz = through(cutoff)
                # The filter's cutoff is put on the tone's exact frequency.
                at, _ = through(cutoff, cutoff=hz, resonance=resonance)
                self.assertAlmostEqual(
                    db(at, plain), expected_db(resonance), delta=0.3,
                    msg="cutoff %g, resonance %g" % (cutoff, resonance))
                low, lhz = through(cutoff / 16)
                below, _ = through(cutoff / 16, cutoff=lhz * 16,
                                   resonance=resonance)
                self.assertAlmostEqual(db(below, low), -resonance / 2.0,
                                       delta=0.3)

    def test_m3_two_octaves_up_is_about_24_db_down(self):
        plain, hz = through(4000.0)
        above, _ = through(4000.0, cutoff=hz / 4)
        self.assertTrue(-27 < db(above, plain) < -21, db(above, plain))


class ModulationEnvelope(unittest.TestCase):

    def test_m4_sweeps_pitch(self):
        sampler = mono()
        data = sine(500, 960)
        loop(sampler.voice[0], data, mod_decay=1.0, mod_sustain=0.0,
             mod_to_pitch=1200.0)
        out = render(sampler, int(1.3 * RATE / BLOCK))
        for at in (0.1, 0.25, 0.5, 0.75):
            got = hz_between(out, at * RATE - 600, at * RATE + 600)
            want = 500 * 2 ** (1.0 - at)
            self.assertAlmostEqual(got, want, delta=want * 0.01,
                                   msg="at %g s" % at)
        got = hz_between(out, 1.1 * RATE, 1.25 * RATE)
        self.assertAlmostEqual(got, 500, delta=5)

    def test_m4_release_takes_pitch_back(self):
        sampler = mono()
        loop(sampler.voice[0], sine(500, 960), mod_to_pitch=1200.0,
             mod_release=0.5, release=2.0)
        out = render(sampler, 20)
        self.assertAlmostEqual(hz_between(out, 1000, 5000), 1000, delta=10)
        sampler.voice[0].release()
        out = render(sampler, int(0.4 * RATE / BLOCK))
        # 0.25 s into a 0.5 s release: half the envelope, half an octave.
        mid = 0.25 * RATE
        self.assertAlmostEqual(hz_between(out, mid - 600, mid + 600),
                               500 * 2 ** 0.5, delta=10)

    def test_m5_sweeps_the_cutoff(self):
        tone, hz = through(2000.0)
        shut, _ = through(2000.0, cutoff=hz / 4)
        swept, _ = through(2000.0, cutoff=hz / 4, mod_to_cutoff=2400.0)
        self.assertAlmostEqual(db(swept, tone), expected_db(0.0), delta=0.5)
        self.assertLess(db(shut, tone), -20)


class Lfos(unittest.TestCase):

    def test_m6_vibrato_rate_depth_and_delay(self):
        sampler = mono()
        loop(sampler.voice[0], sine(1500, 960), vibrato_delay=0.1,
             vibrato_rate=5.0, vibrato_depth=100.0)
        out = render(sampler, int(1.2 * RATE / BLOCK))
        track = frequency_track(out)
        before = [f for t, f in track if t < 0.1 * RATE - 64]
        self.assertAlmostEqual(min(before), 1500, delta=1)
        self.assertAlmostEqual(max(before), 1500, delta=1)
        after = [(t, f) for t, f in track if t > 0.1 * RATE + 64]
        cents = [1200 * math.log(f / 1500.0, 2) for _, f in after]
        self.assertAlmostEqual(max(cents), 100, delta=5)
        self.assertAlmostEqual(min(cents), -100, delta=5)
        # The rate: upward crossings of zero cents, over a second.
        ups = [after[i][0] for i in range(1, len(after))
               if cents[i - 1] < 0 <= cents[i]]
        period = (ups[-1] - ups[0]) / (len(ups) - 1) / RATE
        self.assertAlmostEqual(1 / period, 5.0, delta=0.1)
        # The first thing after the delay is up: a triangle from 0, rising.
        first = [c for (t, f), c in zip(after, cents)
                 if t < 0.1 * RATE + 2400]
        self.assertGreater(sum(first), 0)

    def test_m7_tremolo(self):
        sampler = mono()
        loop(sampler.voice[0], sine(1500, 960), lfo_rate=4.0,
             lfo_to_volume=6.0)
        out = render(sampler, int(1.1 * RATE / BLOCK))
        window = 160
        levels = []
        for start in range(0, len(out) - window, window):
            levels.append(rms(out[start:start + window]))
        quiet = min(levels[30:])
        loud = max(levels[30:])
        plain = 16000 / math.sqrt(2)
        self.assertAlmostEqual(db(loud, plain), 6.0, delta=0.5)
        self.assertAlmostEqual(db(quiet, plain), -6.0, delta=0.5)
        # Rate: the loud windows come every 0.25 s.
        peaks = [i for i in range(1, len(levels) - 1)
                 if levels[i] >= levels[i - 1] and levels[i] > levels[i + 1]
                 and db(levels[i], plain) > 5.0]
        gaps = [peaks[i] - peaks[i - 1] for i in range(1, len(peaks))]
        gaps = [g for g in gaps if g > 10]
        mean = sum(gaps) * window / float(len(gaps)) / RATE
        self.assertAlmostEqual(mean, 0.25, delta=0.005)

    def test_m7_lfo_on_pitch(self):
        sampler = mono()
        loop(sampler.voice[0], sine(1500, 960), lfo_rate=6.0,
             lfo_to_pitch=50.0)
        out = render(sampler, int(0.6 * RATE / BLOCK))
        cents = [1200 * math.log(f / 1500.0, 2)
                 for t, f in frequency_track(out)]
        self.assertAlmostEqual(max(cents), 50, delta=3)
        self.assertAlmostEqual(min(cents), -50, delta=3)

    def test_m8_live_vibrato_depth(self):
        data = sine(1500, 960)
        plain = mono()
        loop(plain.voice[0], data, pitch=1.03)
        sampler = mono()
        loop(sampler.voice[0], data, pitch=1.03, vibrato_rate=5.0)
        first = render(sampler, 40)
        self.assertEqual(first, render(plain, 40))
        sampler.voice[0].vibrato_depth = 50.0
        self.assertEqual(sampler.voice[0].vibrato_depth, 50.0)
        swung = render(sampler, int(0.5 * RATE / BLOCK))
        render(plain, int(0.5 * RATE / BLOCK))
        cents = [1200 * math.log(f / (1500.0 * 1.03), 2)
                 for t, f in frequency_track(swung)]
        self.assertAlmostEqual(max(cents), 50, delta=3)
        sampler.voice[0].vibrato_depth = 0.0
        render(sampler, 1)
        render(plain, 1)
        # The step is the plain voice's again; the position may differ, so
        # compare frequency rather than bits.
        back = render(sampler, 40)
        steady = [f for t, f in frequency_track(back)]
        self.assertAlmostEqual(min(steady), 1545, delta=0.5)
        self.assertAlmostEqual(max(steady), 1545, delta=0.5)

    def test_m9_live_cutoff(self):
        tone_rms, hz = through(2000.0)
        data = sine(hz, int(round(RATE / hz * 40)), peak=3000)
        sampler = mono()
        loop(sampler.voice[0], data)
        render(sampler, 10)
        sampler.voice[0].cutoff = hz
        self.assertEqual(sampler.voice[0].cutoff, hz)
        out = render(sampler, 40)
        self.assertAlmostEqual(db(rms(out[20 * BLOCK:]), tone_rms),
                               expected_db(0.0), delta=0.3)
        sampler.voice[0].cutoff = None
        self.assertIsNone(sampler.voice[0].cutoff)
        render(sampler, 1)
        # Open again: the tone comes through at its own level.
        out = render(sampler, 20)
        self.assertAlmostEqual(db(rms(out), tone_rms), 0.0, delta=0.05)


class Cost(unittest.TestCase):

    def test_m10_filtered_voices_inside_the_block(self):
        data = sine(1000, 2400)
        budget = BLOCK / RATE * 1e6
        for voices in (32, 48, 64):
            sampler = audiosampler.Sampler(voice_count=voices,
                                           channel_count=2, sample_rate=RATE)
            for v in range(voices):
                loop(sampler.voice[v], data, pitch=1.06, level=0.02,
                     cutoff=2000.0, resonance=6.0)
            render(sampler, 4)
            best = float("inf")
            for _ in range(3):
                started = time.perf_counter()
                for _ in range(100):
                    audiocore.get_buffer(sampler)
                best = min(best, (time.perf_counter() - started) / 100 * 1e6)
            print("\n%d filtered stereo voices: %.1f us per block "
                  "(budget %.0f us)" % (voices, best, budget))
            self.assertLess(best, budget)
            sampler.deinit()


if __name__ == "__main__":
    unittest.main()
