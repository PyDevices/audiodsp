"""audioladder: the three things the parity gate cannot see.

`verify_dsp.py` pins what this node *renders*, byte for byte, on every
interpreter. What it cannot say is whether those bytes are a ladder. The
golden would be just as green if the loop had been wired backwards, so the
traits the module exists for are asserted here, in numbers rather than in
hashes: that the loop sustains at a feedback of 4 and not before, that the
tone it sustains sits at the cutoff and stops growing, that the curve
bounding it is odd, and that how hard the input hits it changes what comes
out.

No numpy: this runs wherever the wheel is installed, and the wheel's numpy is
an extra.
"""

import math
import unittest
from array import array

import audiocore
import audioladder

RATE = 12000
BLOCK = 256


def impulse(frames, level=30000):
    values = array("h", [0] * (frames * 2))
    values[0] = values[1] = level
    return audiocore.RawSample(values, sample_rate=RATE, channel_count=2)


def tone(frames, hz, level):
    values = array("h", [0] * (frames * 2))
    for frame in range(frames):
        value = int(level * 32767 *
                    math.sin(2 * math.pi * hz * frame / RATE))
        values[2 * frame] = values[2 * frame + 1] = value
    return values


def render(node, source, frames):
    node.play(source)
    left = []
    for _ in range(frames // BLOCK):
        data = bytes(audiocore.get_buffer(node)[1])
        block = array("h")
        block.frombytes(data)
        left.extend(block[0::2])
    return left


def peak(samples):
    return max(abs(value) for value in samples) / 32768.0


def amplitude_at(samples, hz):
    """One bin of a Hann-windowed DFT, by hand."""
    count = len(samples)
    real = imaginary = 0.0
    weight = 0.0
    for index, value in enumerate(samples):
        window = 0.5 - 0.5 * math.cos(2 * math.pi * index / count)
        weight += window
        angle = 2 * math.pi * hz * index / RATE
        real += value * window * math.cos(angle)
        imaginary -= value * window * math.sin(angle)
    return 2.0 * math.hypot(real, imaginary) / weight / 32768.0


def decibels(value, reference):
    return 20 * math.log10(max(value, 1e-15) / max(reference, 1e-15))


def crossing_rate(samples, seconds):
    crossings = sum(1 for index in range(1, len(samples))
                    if (samples[index - 1] < 0) != (samples[index] < 0))
    return crossings / 2.0 / seconds


def envelope(resonance, cutoff=600.0):
    """Peak over the second half-second after an impulse, and over the
    fourth: decaying, holding, or gone."""
    node = audioladder.Ladder(sample_rate=RATE, cutoff_hz=cutoff,
                              resonance=resonance)
    samples = render(node, impulse(RATE * 2), RATE * 2)
    return (peak(samples[RATE // 2:RATE]),
            peak(samples[3 * RATE // 2:RATE * 2]))


class SelfOscillationTest(unittest.TestCase):
    """The trait a linear filter cannot have at any setting."""

    def test_it_does_not_sustain_below_a_feedback_of_four(self):
        # Not "is silent": at 3.99 the ring is very long, which is what a
        # filter approaching its threshold should do. What is asserted is
        # that it is on its way down.
        for resonance in (3.5, 3.9, 3.95, 3.99):
            early, late = envelope(resonance)
            self.assertTrue(late == 0.0 or decibels(late, early) < -6.0,
                            "resonance %.2f held: %g -> %g"
                            % (resonance, early, late))

    def test_it_sustains_from_a_feedback_of_four(self):
        # At exactly 4 the loop is marginal - the pole pair sits on the unit
        # circle and float rounding decides the last fraction of a decibel -
        # so what is asserted there is that it is still ringing two seconds
        # after the impulse, with nothing driving it. A hair above, it holds
        # its level, because the saturator inside the loop is what sets it.
        early, late = envelope(4.0)
        self.assertGreater(late, 1e-3, "at 4 it fell silent: %g" % late)
        for resonance in (4.05, 4.2):
            early, late = envelope(resonance)
            self.assertGreater(late, 0.05,
                               "resonance %.2f fell silent" % resonance)
            self.assertLess(abs(decibels(late, early)), 1.0,
                            "resonance %.2f drifted: %g -> %g"
                            % (resonance, early, late))

    def test_the_tone_it_sustains_sits_at_the_cutoff(self):
        for cutoff in (300.0, 600.0, 1500.0):
            node = audioladder.Ladder(sample_rate=RATE, cutoff_hz=cutoff,
                                      resonance=4.2)
            tail = render(node, impulse(RATE * 2), RATE * 2)[RATE:]
            measured = crossing_rate(tail, len(tail) / RATE)
            self.assertAlmostEqual(measured / cutoff, 1.0, delta=0.02,
                                   msg="cutoff %.0f gave %.1f Hz"
                                       % (cutoff, measured))

    def test_the_nonlinearity_bounds_it(self):
        node = audioladder.Ladder(sample_rate=RATE, cutoff_hz=600.0,
                                  resonance=4.2)
        samples = render(node, impulse(RATE * 3), RATE * 3)
        windows = [peak(samples[start:start + RATE // 4])
                   for start in range(RATE, len(samples) - RATE // 4,
                                      RATE // 4)]
        self.assertLess(abs(decibels(max(windows), min(windows))), 1.0)

    def test_clear_stops_it(self):
        node = audioladder.Ladder(sample_rate=RATE, cutoff_hz=600.0,
                                  resonance=4.2)
        render(node, impulse(RATE), RATE)
        node.clear()
        node.stop()
        after = bytes(audiocore.get_buffer(node)[1])
        self.assertEqual(after, bytes(len(after)))


class NonlinearityTest(unittest.TestCase):
    def test_the_curve_in_the_loop_is_odd(self):
        # Well off the int16 rails, where the only asymmetry left is two's
        # complement's own (+32767 against -32768).
        rendered = []
        for sign in (1, -1):
            values = tone(4096, 400.0, 0.3)
            if sign < 0:
                values = array("h", [-value for value in values])
            source = audiocore.RawSample(values, sample_rate=RATE,
                                         channel_count=2)
            node = audioladder.Ladder(sample_rate=RATE, cutoff_hz=700.0,
                                      resonance=3.9, drive=2.0)
            rendered.append(render(node, source, 4096))
        self.assertLess(peak(rendered[0]), 0.9)
        self.assertEqual(rendered[0], [-value for value in rendered[1]])

    def test_drive_raises_the_third_harmonic(self):
        ratios = []
        for drive_db in (0.0, 8.0, 16.0, 24.0):
            source = audiocore.RawSample(tone(RATE, 150.0, 0.5),
                                         sample_rate=RATE, channel_count=2)
            node = audioladder.Ladder(sample_rate=RATE, cutoff_hz=600.0,
                                      resonance=3.9,
                                      drive=10 ** (drive_db / 20))
            body = render(node, source, RATE)[RATE // 4:]
            ratios.append(decibels(amplitude_at(body, 450.0),
                                   amplitude_at(body, 150.0)))
        self.assertEqual(ratios, sorted(ratios), "h3/h1 did not rise: %s"
                         % (ratios,))
        self.assertGreater(ratios[-1] - ratios[0], 10.0)


class SurfaceTest(unittest.TestCase):
    def test_mix_zero_is_a_wire(self):
        values = tone(2048, 350.0, 0.7)
        source = audiocore.RawSample(values, sample_rate=RATE,
                                     channel_count=2)
        node = audioladder.Ladder(sample_rate=RATE, cutoff_hz=200.0,
                                  resonance=4.0, drive=8.0, mix=0.0)
        node.play(source)
        rendered = b"".join(bytes(audiocore.get_buffer(node)[1])
                            for _ in range(2048 // BLOCK))
        self.assertEqual(rendered, bytes(memoryview(values).cast("B")))

    def test_it_presents_itself_as_a_stereo_sample(self):
        node = audioladder.Ladder(sample_rate=RATE)
        self.assertEqual(node.sample_rate, RATE)
        self.assertEqual(node.channel_count, 2)
        self.assertEqual(node.bits_per_sample, 16)
        self.assertTrue(node.samples_signed)

    def test_a_mono_ladder_hands_back_mono_blocks(self):
        values = array("h", [(index * 37) % 2001 - 1000
                             for index in range(1024)])
        source = audiocore.RawSample(values, sample_rate=RATE,
                                     channel_count=1)
        node = audioladder.Ladder(sample_rate=RATE, channel_count=1,
                                  cutoff_hz=800.0, resonance=3.0)
        node.play(source)
        data = bytes(audiocore.get_buffer(node)[1])
        self.assertEqual(len(data), audioladder.FRAMES * 2)
        self.assertNotEqual(data, bytes(len(data)))

    def test_a_starved_chain_gets_silence_not_a_short_block(self):
        node = audioladder.Ladder(sample_rate=RATE, resonance=4.2)
        for _ in range(2):
            data = bytes(audiocore.get_buffer(node)[1])
            self.assertEqual(len(data), audioladder.FRAMES * 4)
            self.assertEqual(data, bytes(len(data)))

    def test_it_refuses_what_it_cannot_do(self):
        with self.assertRaises(ValueError):
            audioladder.Ladder(sample_rate=RATE, channel_count=3)
        with self.assertRaises(TypeError):
            audioladder.Ladder(sample_rate=RATE, cutoff=600.0)
        node = audioladder.Ladder(sample_rate=RATE)
        with self.assertRaises(TypeError):
            node.set(resonanc=4.0)

    def test_options_clamp_rather_than_raise(self):
        node = audioladder.Ladder(sample_rate=RATE, cutoff_hz=1e6,
                                  resonance=99.0, drive=1e6, poles=9,
                                  passband_comp=4.0, oversample=5, mix=5.0)
        node.play(audiocore.RawSample(tone(1024, 300.0, 0.5),
                                      sample_rate=RATE, channel_count=2))
        data = bytes(audiocore.get_buffer(node)[1])
        self.assertEqual(len(data), audioladder.FRAMES * 4)


class LiveSettingTest(unittest.TestCase):
    """L15 and L16 - audiodsp#106.

    The issue was filed on a reading: `Ladder.set()` never calls
    `config_finish`, so the six words derived from a setting were said to stay
    stale until the node was rebuilt. They do not.
    `audiodsp_ladder_configure` ends with `ladder_update_coefficients`, so
    every write derives, and a `finish()` afterwards recomputes the same six
    numbers from the same stored values. L16 is what keeps that true: an
    option added later that derives nothing of its own fails here rather than
    under a macro at run time.

    L15 is the trait the issue's second half asked for and nothing had: move
    cutoff and resonance on a *live* node - one that has already rendered -
    and measure the response, rather than trusting that the write arrived.
    """

    RATE = 48000
    CHANNELS = 2
    BLOCKS = 40

    #: A filter that is doing something, so an option acting on the loop has a
    #: loop to act on.
    BASELINE = dict(cutoff_hz=1500.0, resonance=3.0, drive=1.5, mix=1.0,
                    passband_comp=0.5, oversample=2.0)

    #: One value per option, each far from its baseline.
    OPTIONS = {"cutoff_hz": 400.0, "resonance": 4.1, "drive": 6.0,
               "poles": 2.0, "passband_comp": 1.0, "oversample": 1.0,
               "mix": 0.3}

    def _tone(self, hz, frames, level=12000):
        values = array("h", [0] * (frames * self.CHANNELS))
        for frame in range(frames):
            value = int(level * math.sin(2 * math.pi * hz * frame / self.RATE))
            for channel in range(self.CHANNELS):
                values[frame * self.CHANNELS + channel] = value
        return audiocore.RawSample(values, sample_rate=self.RATE,
                                   channel_count=self.CHANNELS)

    def _noise(self, frames, seed=1):
        values = array("h", [0] * (frames * self.CHANNELS))
        state = seed
        for frame in range(frames):
            state = (state * 1103515245 + 12345) & 0x7FFFFFFF
            value = (state >> 8) % 40001 - 20000
            for channel in range(self.CHANNELS):
                values[frame * self.CHANNELS + channel] = value
        return audiocore.RawSample(values, sample_rate=self.RATE,
                                   channel_count=self.CHANNELS)

    def _render(self, node, blocks):
        return b"".join(bytes(audiocore.get_buffer(node)[1])
                        for _block in range(blocks))

    def _rms(self, data, skip_blocks=8):
        block = array("h")
        block.frombytes(data[skip_blocks * BLOCK * self.CHANNELS * 2:])
        return math.sqrt(sum(float(v) * v for v in block) / len(block))

    def _live(self, **changed):
        """A node that has already rendered four blocks, then changed."""
        node = audioladder.Ladder(sample_rate=self.RATE,
                                  channel_count=self.CHANNELS,
                                  **self.BASELINE)
        node.play(self._noise(BLOCK * self.BLOCKS))
        self._render(node, 4)
        if changed:
            node.set(**changed)
        return node

    def test_a_cutoff_moved_on_a_live_node_moves_the_passband(self):
        """L15, the cutoff. A 4 kHz tone passes a node open to 8 kHz; the same
        node closed to 200 Hz on the fly must stop passing it, and must land
        where a node *built* at 200 Hz lands."""
        loud = audioladder.Ladder(sample_rate=self.RATE,
                                  channel_count=self.CHANNELS, mix=1.0,
                                  resonance=0.0, cutoff_hz=8000.0)
        loud.play(self._tone(4000, BLOCK * self.BLOCKS))
        passed = self._rms(self._render(loud, 24))

        moved = audioladder.Ladder(sample_rate=self.RATE,
                                   channel_count=self.CHANNELS, mix=1.0,
                                   resonance=0.0, cutoff_hz=8000.0)
        moved.play(self._tone(4000, BLOCK * self.BLOCKS))
        self._render(moved, 4)
        moved.set(cutoff_hz=200.0)
        stopped = self._rms(self._render(moved, 24))

        self.assertGreater(passed, 1000.0)
        self.assertLess(stopped, passed / 100.0)

    def test_a_resonance_moved_on_a_live_node_moves_the_peak(self):
        """L15, the resonance. Past `AUDIODSP_LADDER_SELF_OSCILLATION` the
        loop sustains after the input stops; a node set there while running
        must sustain, and the same node set back below it must not."""
        energy = []
        for resonance in (0.5, 4.1):
            node = audioladder.Ladder(sample_rate=self.RATE,
                                      channel_count=self.CHANNELS, mix=1.0,
                                      cutoff_hz=800.0, resonance=0.5)
            node.play(self._noise(BLOCK * 8))
            self._render(node, 8)
            node.set(resonance=resonance)
            node.play(audiocore.RawSample(
                array("h", bytes(BLOCK * 24 * self.CHANNELS * 2)),
                sample_rate=self.RATE, channel_count=self.CHANNELS))
            tail = array("h")
            tail.frombytes(self._render(node, 16)[BLOCK * 8 * self.CHANNELS * 2:])
            energy.append(sum(abs(v) for v in tail))
        self.assertGreater(energy[1], energy[0] * 10)

    def test_every_option_lands_without_a_second_finish(self):
        """L16. `set(option)` then `finish()` renders what `set(option)`
        renders, for every option the node takes."""
        for name, value in self.OPTIONS.items():
            with self.subTest(option=name):
                plain = self._live(**{name: value})
                finished = self._live(**{name: value})
                finished._state.finish()
                self.assertEqual(self._render(plain, 12),
                                 self._render(finished, 12))

    def test_every_option_moves_the_render(self):
        """The control for L16: an option that changed nothing would pass L16
        for the wrong reason."""
        reference = self._render(self._live(), 12)
        for name, value in self.OPTIONS.items():
            with self.subTest(option=name):
                self.assertNotEqual(self._render(self._live(**{name: value}),
                                                 12), reference)


if __name__ == "__main__":
    unittest.main()


class UniversalTraitTest(unittest.TestCase):
    """L11-L14 - the traits every audiodsp-own node carries."""

    RATE = 12000
    CHANNELS = 2

    def _alternating(self, frames=4096, level=32767):
        values = array("h")
        for frame in range(frames):
            for _channel in range(self.CHANNELS):
                values.append(level if frame % 2 else -level)
        return audiocore.RawSample(values, sample_rate=self.RATE,
                                   channel_count=self.CHANNELS)

    def _alternating_words(self, count, level=32767):
        return [level if (index // self.CHANNELS) % 2 else -level
                for index in range(count)]

    def _silence(self, frames=4096):
        return audiocore.RawSample(
            array("h", bytes(frames * self.CHANNELS * 2)),
            sample_rate=self.RATE, channel_count=self.CHANNELS)

    def _words(self, node, blocks):
        out = []
        for _block in range(blocks):
            data = bytes(audiocore.get_buffer(node)[1])
            for position in range(0, len(data), 2):
                word = data[position] | (data[position + 1] << 8)
                out.append(word - 65536 if word >= 32768 else word)
        return out

    def test_the_neutral_setting_is_exact_at_the_rails(self):
        """L11, this module's form of the identity trait. `mix=0` is already
        covered elsewhere in this file on ordinary material; what is new here is
        **at +/-32767**, which is where an arithmetic width error shows and a
        range check does not. Measured 0 LSB."""
        node = audioladder.Ladder(sample_rate=self.RATE,
                                    channel_count=self.CHANNELS, mix=0.0)
        node.play(self._alternating())
        rendered = self._words(node, 6)
        self.assertEqual(rendered, self._alternating_words(len(rendered)))

    def test_the_identity_trait_discriminates(self):
        """Its control: the same node wet must not satisfy it."""
        node = audioladder.Ladder(sample_rate=self.RATE,
                                   channel_count=self.CHANNELS, mix=1.0,
                                   cutoff_hz=800.0, resonance=3.0)
        node.play(self._alternating())
        rendered = self._words(node, 6)
        self.assertNotEqual(rendered, self._alternating_words(len(rendered)))

    def test_silence_in_is_exactly_zero_out(self):
        """L12. Measured exact."""
        node = audioladder.Ladder(sample_rate=self.RATE,
                                   channel_count=self.CHANNELS, mix=1.0,
                                   cutoff_hz=800.0, resonance=3.0)
        node.play(self._silence())
        for _block in range(6):
            data = bytes(audiocore.get_buffer(node)[1])
            self.assertEqual(data, bytes(len(data)))

    def test_clear_leaves_the_node_as_a_freshly_built_one(self):
        """L13. Not the same claim as "clear() stops it": this is that a
        cleared node and a node that never played render the same bytes."""
        used = audioladder.Ladder(sample_rate=self.RATE,
                                   channel_count=self.CHANNELS, mix=1.0,
                                   cutoff_hz=800.0, resonance=3.0)
        used.play(self._alternating())
        for _block in range(4):
            audiocore.get_buffer(used)
        used.clear()
        used.play(self._silence())

        fresh = audioladder.Ladder(sample_rate=self.RATE,
                                   channel_count=self.CHANNELS, mix=1.0,
                                   cutoff_hz=800.0, resonance=3.0)
        fresh.play(self._silence())
        for _block in range(6):
            self.assertEqual(bytes(audiocore.get_buffer(used)[1]),
                             bytes(audiocore.get_buffer(fresh)[1]))

    def test_the_clear_trait_discriminates(self):
        """Its control: without the clear the two must differ."""
        used = audioladder.Ladder(sample_rate=self.RATE,
                                   channel_count=self.CHANNELS, mix=1.0,
                                   cutoff_hz=800.0, resonance=3.0)
        used.play(self._alternating())
        for _block in range(4):
            audiocore.get_buffer(used)
        used.play(self._silence())               # deliberately not cleared

        fresh = audioladder.Ladder(sample_rate=self.RATE,
                                   channel_count=self.CHANNELS, mix=1.0,
                                   cutoff_hz=800.0, resonance=3.0)
        fresh.play(self._silence())
        differed = any(bytes(audiocore.get_buffer(used)[1])
                       != bytes(audiocore.get_buffer(fresh)[1])
                       for _block in range(6))
        self.assertTrue(differed, "an uncleared node already matches a fresh "
                        "one, so the clear trait cannot fail")
