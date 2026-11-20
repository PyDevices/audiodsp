# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""audiofm: every algorithm sounds, the voice format round-trips, and notes behave.

`tests/parity/fm_probe.py` pins what the module renders on every
interpreter. This file holds what a render cannot say by itself: pitches
measured against the note asked for, the bank format checked byte for byte,
and the voice pool's press, release, sustain and stealing.

The three `*_patch()` builders are small voices written for these tests, one
each of the classic FM sounds (an electric piano, a bell and a bass). They are
also what the module's docs page renders.
"""

import array
import math
import unittest

import audiocore
import audiofm


RATE = 48000


def _op(patch, number, **values):
    op = patch.operators[number - 1]
    for name, value in values.items():
        setattr(op, name, value)


def ep_patch():
    """An electric piano: three modulator and carrier pairs, the third a tine."""
    patch = audiofm.Patch()
    patch.name = "TEST EP"
    patch.algorithm = 5
    patch.feedback = 6
    carrier = dict(rates=(96, 25, 25, 67), levels=(99, 75, 0, 0), coarse=1,
                   velocity=2)
    _op(patch, 1, level=99, detune=10, **carrier)
    _op(patch, 3, level=95, detune=4, **carrier)
    _op(patch, 5, level=85, **carrier)
    _op(patch, 2, level=72, coarse=1, rates=(95, 50, 35, 78),
        levels=(99, 75, 0, 0), velocity=6, rate_scaling=3)
    _op(patch, 4, level=58, coarse=14, rates=(95, 29, 20, 50),
        levels=(99, 95, 0, 0), velocity=7, rate_scaling=3)
    _op(patch, 6, level=70, coarse=1, rates=(95, 20, 20, 50),
        levels=(99, 95, 0, 0), velocity=4)
    return patch


def bell_patch():
    """A bell: inharmonic ratios and a long, even decay."""
    patch = audiofm.Patch()
    patch.name = "TEST BELL"
    patch.algorithm = 5
    ring = dict(rates=(99, 32, 20, 40), levels=(99, 0, 0, 0))
    _op(patch, 1, level=99, coarse=1, **ring)
    _op(patch, 2, level=82, coarse=3, fine=50, **ring)
    _op(patch, 3, level=88, coarse=2, detune=9, **ring)
    _op(patch, 4, level=76, coarse=5, fine=41, **ring)
    _op(patch, 5, level=80, coarse=4, detune=5, **ring)
    _op(patch, 6, level=70, coarse=7, fine=11, **ring)
    return patch


def bass_patch():
    """A bass: a half-ratio carrier with a modulator that closes fast."""
    patch = audiofm.Patch()
    patch.name = "TEST BASS"
    patch.algorithm = 1
    patch.feedback = 5
    _op(patch, 1, level=99, coarse=0, rates=(99, 45, 30, 70),
        levels=(99, 86, 0, 0))
    _op(patch, 2, level=86, coarse=0, rates=(99, 60, 30, 70),
        levels=(99, 48, 0, 0), velocity=3)
    _op(patch, 3, level=78, coarse=1, rates=(99, 50, 30, 70),
        levels=(99, 80, 0, 0))
    _op(patch, 4, level=74, coarse=1, rates=(99, 70, 30, 70),
        levels=(99, 40, 0, 0))
    _op(patch, 5, level=60, coarse=3, rates=(99, 80, 30, 70),
        levels=(99, 0, 0, 0))
    _op(patch, 6, level=62, coarse=1, rates=(99, 80, 30, 70),
        levels=(99, 0, 0, 0))
    return patch


def render(synth, seconds):
    blocks = int(seconds * synth.sample_rate) // audiofm.FRAMES + 1
    out = array.array("h")
    for _ in range(blocks):
        result, data = audiocore.get_buffer(synth)
        out.frombytes(bytes(data))
    return out


def frequency(samples, rate):
    """Rising zero crossings per second, interpolated."""
    crossings = []
    for i in range(1, len(samples)):
        a, b = samples[i - 1], samples[i]
        if a < 0 <= b:
            crossings.append(i - 1 + (-a) / (b - a))
    span = crossings[-1] - crossings[0]
    return (len(crossings) - 1) * rate / span


def peak(samples):
    return max(abs(v) for v in samples)


def rms(samples):
    return math.sqrt(sum(v * v for v in samples) / len(samples))


def sine_patch(**values):
    patch = audiofm.Patch()
    _op(patch, 1, **values)
    return patch


class AlgorithmTests(unittest.TestCase):

    def everything_on(self, algorithm):
        patch = audiofm.Patch()
        patch.algorithm = algorithm
        patch.feedback = 7
        for number in range(1, 7):
            _op(patch, number, level=88, coarse=number, rates=(99, 40, 40, 60),
                levels=(99, 80, 60, 0))
        return patch

    def test_every_algorithm_sounds_and_differs(self):
        seen = {}
        for algorithm in range(1, 33):
            synth = audiofm.Synth(sample_rate=RATE, patch=self.everything_on(algorithm))
            synth.press(57, 110)
            samples = render(synth, 0.3)
            self.assertGreater(peak(samples), 250, "algorithm %d" % algorithm)
            self.assertGreater(rms(samples), 50, "algorithm %d" % algorithm)
            self.assertLess(peak(samples), 32767, "algorithm %d clips" % algorithm)
            seen.setdefault(samples.tobytes(), []).append(algorithm)
        self.assertEqual(len(seen), 32, "algorithms that render alike: %r"
                         % [v for v in seen.values() if len(v) > 1])

    def test_feedback_changes_the_sound(self):
        renders = []
        for feedback in (0, 7):
            # In algorithm 32 every operator is a carrier and operator 6
            # feeds back on itself.
            patch = audiofm.Patch()
            patch.operators[0].level = 0
            _op(patch, 6, level=99)
            patch.algorithm = 32
            patch.feedback = feedback
            synth = audiofm.Synth(sample_rate=RATE, patch=patch)
            synth.press(60)
            renders.append(render(synth, 0.1).tobytes())
        self.assertNotEqual(renders[0], renders[1])

    def test_feedback_around_a_chain(self):
        # Algorithms 4 and 6 feed a carrier's output back to the top of its
        # stack, through two or three operators.
        for algorithm in (4, 6):
            renders = []
            for feedback in (0, 7):
                patch = audiofm.Patch()
                patch.algorithm = algorithm
                patch.feedback = feedback
                for number in range(1, 7):
                    _op(patch, number, level=80, coarse=1)
                synth = audiofm.Synth(sample_rate=RATE, patch=patch)
                synth.press(60)
                renders.append(render(synth, 0.1))
            self.assertNotEqual(renders[0].tobytes(), renders[1].tobytes(),
                                "algorithm %d" % algorithm)
            self.assertLess(peak(renders[1]), 32767)

    def test_disabled_operator_is_silent(self):
        patch = sine_patch(level=99)
        patch.operators[0].enabled = False
        synth = audiofm.Synth(sample_rate=RATE, patch=patch)
        synth.press(60)
        self.assertEqual(peak(render(synth, 0.1)), 0)


class PitchTests(unittest.TestCase):

    def measure(self, patch, note=69, setup=None):
        synth = audiofm.Synth(sample_rate=RATE, patch=patch)
        if setup:
            setup(synth)
        synth.press(note, 100)
        return frequency(render(synth, 0.5)[2400:], RATE)

    def test_note_69_is_440(self):
        self.assertAlmostEqual(self.measure(sine_patch(level=99)), 440.0, delta=440 * 0.002)

    def test_ratio_and_fine(self):
        # coarse 2 with fine 50 is a ratio of 3.
        self.assertAlmostEqual(self.measure(sine_patch(level=99, coarse=2, fine=50), note=57),
                               660.0, delta=660 * 0.003)

    def test_fixed_frequency_ignores_the_key(self):
        patch = sine_patch(level=99, fixed=True, coarse=2, fine=0)
        for note in (40, 80):
            self.assertAlmostEqual(self.measure(patch, note=note), 100.0, delta=0.5)

    def test_transpose_moves_an_octave(self):
        patch = sine_patch(level=99)
        patch.transpose = 36
        self.assertAlmostEqual(self.measure(patch, note=57), 440.0, delta=1.0)

    def test_detune_step_follows_the_key(self):
        # Seven detune steps, in cents, as measured on a reference renderer:
        # wide low on the keyboard, narrow high up, whatever the ratio.
        for note, coarse, cents in ((36, 1, 13.70), (60, 4, 8.37), (84, 1, 4.73)):
            plain = self.measure(sine_patch(level=99, coarse=coarse), note=note)
            sharp = self.measure(sine_patch(level=99, coarse=coarse, detune=14), note=note)
            got = 1200 * math.log2(sharp / plain)
            self.assertAlmostEqual(got, cents, delta=cents * 0.02, msg="note %d" % note)

    def test_pitch_bend(self):
        def bend(synth):
            synth.bend_range = 2
            synth.pitch_bend = 1.0
        got = self.measure(sine_patch(level=99), note=67, setup=bend)
        self.assertAlmostEqual(got, 440.0, delta=1.5)


class VoiceTests(unittest.TestCase):

    def test_release_frees_the_voice(self):
        synth = audiofm.Synth(sample_rate=RATE, patch=sine_patch(level=99, rates=(99, 99, 99, 80)))
        synth.press(60)
        render(synth, 0.05)
        self.assertEqual(synth.active, 1)
        synth.release(60)
        render(synth, 1.0)
        self.assertEqual(synth.active, 0)
        self.assertEqual(peak(render(synth, 0.05)), 0)

    def test_held_level_keeps_sounding(self):
        synth = audiofm.Synth(sample_rate=RATE, patch=sine_patch(level=99))
        synth.press(60)
        render(synth, 1.0)
        self.assertEqual(synth.active, 1)
        self.assertGreater(peak(render(synth, 0.05)), 750)

    def test_sustain_holds_until_lifted(self):
        synth = audiofm.Synth(sample_rate=RATE, patch=sine_patch(level=99, rates=(99, 99, 99, 80)))
        synth.sustain = True
        synth.press(60)
        synth.release(60)
        render(synth, 1.0)
        self.assertEqual(synth.active, 1)
        synth.sustain = False
        render(synth, 1.0)
        self.assertEqual(synth.active, 0)

    def test_stealing_keeps_the_newest(self):
        synth = audiofm.Synth(voices=2, sample_rate=RATE, patch=sine_patch(level=99))
        for note in (60, 64, 67):
            synth.press(note)
        self.assertEqual(synth.active, 2)
        synth.release(64)
        synth.release(67)
        render(synth, 1.0)
        self.assertEqual(synth.active, 0, "60 was stolen, so nothing still holds")

    def test_amplitude_modulation_curve(self):
        # Measured on a reference renderer: any sensitivity costs 1.1 dB, and
        # a square LFO at depth 60, sensitivity 3 swings from there to 15.9 dB
        # down.
        def levels(depth, sens):
            patch = sine_patch(level=99, amp_mod=sens, rates=(99, 99, 99, 99))
            patch.lfo_amp_depth = depth
            patch.lfo_wave = 3
            patch.lfo_speed = 35
            synth = audiofm.Synth(sample_rate=RATE, patch=patch)
            synth.press(60, 100)
            samples = render(synth, 0.6)[4800:]
            blocks = [peak(samples[i:i + 480]) for i in range(0, len(samples) - 480, 480)]
            return max(blocks), min(blocks)
        plain, _ = levels(0, 0)
        top, bottom = levels(0, 1)
        self.assertAlmostEqual(20 * math.log10(top / plain), -1.1, delta=0.1)
        top, bottom = levels(60, 3)
        self.assertAlmostEqual(20 * math.log10(top / plain), -1.1, delta=0.1)
        self.assertAlmostEqual(20 * math.log10(bottom / plain), -15.9, delta=0.5)

    def test_velocity_sensitivity(self):
        levels = []
        for velocity in (30, 127):
            synth = audiofm.Synth(sample_rate=RATE, patch=sine_patch(level=99, velocity=7))
            synth.press(60, velocity)
            levels.append(peak(render(synth, 0.2)))
        self.assertLess(levels[0], levels[1] / 3)

    def test_patch_edits_reach_the_next_note(self):
        patch = sine_patch(level=99)
        synth = audiofm.Synth(sample_rate=RATE, patch=patch)
        synth.press(69)
        render(synth, 0.05)
        patch.operators[0].level = 0
        sounding = peak(render(synth, 0.05))
        self.assertGreater(sounding, 750, "a sounding note keeps its patch")
        synth.release_all()
        render(synth, 1.0)
        synth.press(72)
        self.assertEqual(peak(render(synth, 0.2)), 0, "the next note has level 0")

    def test_stereo_is_the_mono_signal_twice(self):
        synth = audiofm.Synth(sample_rate=RATE, channel_count=2, patch=ep_patch())
        synth.press(60)
        samples = render(synth, 0.1)
        self.assertEqual(samples[0::2], samples[1::2])
        self.assertGreater(peak(samples), 1000)

    def test_sixteen_note_chord_has_headroom(self):
        # Six carriers, an organ's drawbars, is about as loud as a voice gets.
        patch = audiofm.Patch()
        patch.algorithm = 32
        for number, (coarse, fine, level) in enumerate(
                ((0, 0, 95), (1, 0, 99), (1, 50, 88), (2, 0, 92), (3, 0, 80), (4, 0, 76)), 1):
            _op(patch, number, coarse=coarse, fine=fine, level=level,
                rates=(95, 99, 99, 82), levels=(99, 99, 99, 0))
        synth = audiofm.Synth(voices=16, sample_rate=RATE, patch=patch)
        for note in (48, 52, 55, 59, 60, 62, 64, 67, 69, 71, 72, 74, 76, 79, 81, 84):
            synth.press(note, 127)
        self.assertLess(peak(render(synth, 1.0)), 32767)

    def test_a_full_pool_clips_rather_than_wraps(self):
        # 64 voices of six full carriers in phase is far past full scale: the
        # output must hold at the rail with the single voice's sign.
        patch = audiofm.Patch()
        patch.algorithm = 32
        for number in range(1, 7):
            _op(patch, number, level=99, coarse=1)
        one = audiofm.Synth(voices=1, sample_rate=RATE, patch=patch)
        full = audiofm.Synth(voices=audiofm.MAX_VOICES, sample_rate=RATE, patch=patch)
        one.press(60, 127)
        for _ in range(audiofm.MAX_VOICES):
            full.press(60, 127)
        quiet, loud = render(one, 0.05), render(full, 0.05)
        self.assertGreaterEqual(peak(loud), 32767)
        for a, b in zip(quiet, loud):
            self.assertFalse(a * b < 0, "the sum wrapped")

    def test_arguments_are_checked(self):
        synth = audiofm.Synth(sample_rate=RATE)
        with self.assertRaises(ValueError):
            synth.press(128)
        with self.assertRaises(ValueError):
            synth.press(60, 128)
        with self.assertRaises(TypeError):
            synth.patch = b"\x00" * 155
        with self.assertRaises(ValueError):
            audiofm.Synth(voices=0)
        with self.assertRaises(ValueError):
            audiofm.Synth(voices=audiofm.MAX_VOICES + 1)

    def test_deinit_refuses_further_use(self):
        synth = audiofm.Synth(sample_rate=RATE)
        synth.deinit()
        with self.assertRaises(ValueError):
            synth.press(60)


class FormatTests(unittest.TestCase):

    def scrambled(self, seed):
        """A patch with every parameter set to a different in-range value."""
        patch = audiofm.Patch()
        state = seed * 2654435761 + 1

        def pick(top):
            nonlocal state
            state = (state * 1103515245 + 12345) & 0x7FFFFFFF
            return state % (top + 1)

        for op in patch.operators:
            op.rates = tuple(pick(99) for _ in range(4))
            op.levels = tuple(pick(99) for _ in range(4))
            op.breakpoint = pick(99)
            op.left_depth = pick(99)
            op.right_depth = pick(99)
            op.left_curve = pick(3)
            op.right_curve = pick(3)
            op.rate_scaling = pick(7)
            op.amp_mod = pick(3)
            op.velocity = pick(7)
            op.level = pick(99)
            op.fixed = pick(1)
            op.coarse = pick(31)
            op.fine = pick(99)
            op.detune = pick(14)
        patch.pitch_rates = tuple(pick(99) for _ in range(4))
        patch.pitch_levels = tuple(pick(99) for _ in range(4))
        patch.algorithm = pick(31) + 1
        patch.feedback = pick(7)
        patch.key_sync = pick(1)
        patch.lfo_speed = pick(99)
        patch.lfo_delay = pick(99)
        patch.lfo_pitch_depth = pick(99)
        patch.lfo_amp_depth = pick(99)
        patch.lfo_key_sync = pick(1)
        patch.lfo_wave = pick(5)
        patch.lfo_pitch_sens = pick(7)
        patch.transpose = pick(48)
        patch.name = "VOICE %d" % seed
        return patch

    def test_single_voice_round_trip(self):
        for seed in range(8):
            patch = self.scrambled(seed)
            data = patch.pack()
            self.assertEqual(len(data), 155)
            again = audiofm.Patch.unpack(data)
            self.assertEqual(again.pack(), data)
            self.assertEqual(again.name, "VOICE %d" % seed)

    def test_bank_round_trip(self):
        patches = [self.scrambled(seed) for seed in range(32)]
        bank = audiofm.Patch.pack_bank(patches)
        self.assertEqual(len(bank), 4104)
        self.assertEqual(bank[:6], b"\xf0\x43\x00\x09\x20\x00")
        self.assertEqual(bank[-1], 0xF7)
        self.assertEqual((sum(bank[6:4102]) + bank[4102]) & 0x7F, 0)
        self.assertTrue(all(byte < 0x80 for byte in bank[1:-1]))
        back = audiofm.Patch.bank(bank)
        self.assertEqual(len(back), 32)
        for original, copy in zip(patches, back):
            self.assertEqual(copy.pack(), original.pack())
        self.assertEqual([p.pack() for p in audiofm.Patch.bank(bank[6:4102])],
                         [p.pack() for p in patches])

    def test_bank_checksum_is_checked(self):
        bank = bytearray(audiofm.Patch.pack_bank([ep_patch()]))
        bank[100] ^= 1
        with self.assertRaises(ValueError):
            audiofm.Patch.bank(bytes(bank))

    def test_short_bank_is_padded_with_the_initial_voice(self):
        back = audiofm.Patch.bank(audiofm.Patch.pack_bank([bell_patch()]))
        self.assertEqual(back[0].name, "TEST BELL")
        self.assertEqual(back[31].pack(), audiofm.Patch().pack())

    def test_packed_bit_layout(self):
        # Operator 6 is stored first. Its packed bytes 11..15 hold two fields
        # each; check them against hand-computed values.
        patch = audiofm.Patch()
        op6 = patch.operators[5]
        op6.left_curve, op6.right_curve = 1, 3
        op6.rate_scaling, op6.detune = 5, 14
        op6.amp_mod, op6.velocity = 2, 6
        op6.fixed, op6.coarse = True, 21
        patch.feedback, patch.key_sync = 6, True
        patch.lfo_key_sync, patch.lfo_wave, patch.lfo_pitch_sens = True, 4, 5
        packed = audiofm.Patch.pack_bank([patch])[6:6 + 128]
        self.assertEqual(packed[11], (3 << 2) | 1)
        self.assertEqual(packed[12], (14 << 3) | 5)
        self.assertEqual(packed[13], (6 << 2) | 2)
        self.assertEqual(packed[15], (21 << 1) | 1)
        self.assertEqual(packed[111], (1 << 3) | 6)
        self.assertEqual(packed[116], (5 << 4) | (4 << 1) | 1)
        self.assertEqual(packed[117], 24)
        self.assertEqual(packed[118:128], b"INIT VOICE")

    def test_single_voice_message(self):
        body = ep_patch().pack()
        checksum = (128 - (sum(body) & 127)) & 127
        message = b"\xf0\x43\x00\x00\x01\x1b" + body + bytes((checksum, 0xF7))
        self.assertEqual(audiofm.Patch.unpack(message).pack(), body)
        broken = bytearray(message)
        broken[161] ^= 1
        with self.assertRaises(ValueError):
            audiofm.Patch.unpack(bytes(broken))

    def test_out_of_range_bytes_are_clamped(self):
        data = bytearray(audiofm.Patch().pack())
        data[0] = 127       # operator 6, rate 1
        data[134] = 99      # algorithm
        patch = audiofm.Patch.unpack(bytes(data))
        self.assertEqual(patch.operators[5].rates[0], 99)
        self.assertEqual(patch.algorithm, 32)

    def test_attributes_are_checked(self):
        patch = audiofm.Patch()
        with self.assertRaises(ValueError):
            patch.algorithm = 33
        with self.assertRaises(ValueError):
            patch.operators[0].detune = 15
        with self.assertRaises(ValueError):
            patch.operators[0].rates = (1, 2, 3)
        with self.assertRaises(ValueError):
            patch.name = "ELEVEN CHAR"
        with self.assertRaises(ValueError):
            audiofm.Patch.unpack(bytes(154))

    def test_operator_numbering(self):
        patch = audiofm.Patch()
        self.assertEqual([op.number for op in patch.operators], [1, 2, 3, 4, 5, 6])
        patch.operators[0].level = 42
        self.assertEqual(patch.pack()[5 * 21 + 16], 42)


if __name__ == "__main__":
    unittest.main()
