"""The synthesizer's press semantics, held to CircuitPython's own.

These behaviors were sourced from the oracle's synthio_span_change_note
(shared-module/synthio/__init__.c) and verified byte-identical against a
built oracle before landing (issues #8 and #9):

- a note already playing re-enters ATTACK from its CURRENT level with its
  oscillator phase intact (and its filter reset);
- a fresh press claims a free channel starting at phase zero;
- a press with no free channel is REFUSED - never evicted, and since
  audiodsp#127 it is COUNTED, so a refusal can be seen from outside;
- an at-cap re-press of a member changes nothing about the others.
"""

import array
import math
import unittest

import audiocore
import synthio

SINE = array.array("h", [int(28000 * math.sin(2 * math.pi * i / 256))
                         for i in range(256)])


def sounding(synth):
    return [round(n.frequency, 0) for n in synth.pressed]


class PressSemantics(unittest.TestCase):
    def _synth(self):
        return synthio.Synthesizer(sample_rate=48000, channel_count=1)

    def test_refuses_when_full(self):
        s = self._synth()
        notes = [synthio.Note(100.0 + 50 * i, waveform=SINE, amplitude=0.05)
                 for i in range(synthio.Synthesizer.max_polyphony + 7)]
        for n in notes:
            s.press(n)
        self.assertEqual(len(s.pressed), synthio.Synthesizer.max_polyphony)
        # the FIRST max_polyphony notes survive; the excess was refused
        self.assertIn(100.0, sounding(s))
        self.assertNotIn(notes[-1].frequency, sounding(s))

    def test_at_cap_repress_evicts_nothing(self):
        s = self._synth()
        notes = [synthio.Note(100.0 + 50 * i, waveform=SINE, amplitude=0.05)
                 for i in range(synthio.Synthesizer.max_polyphony)]
        for n in notes:
            s.press(n)
        s.press(notes[7])
        self.assertEqual(len(s.pressed), synthio.Synthesizer.max_polyphony)
        self.assertIn(notes[0], s.pressed)

    def test_repress_keeps_phase_and_reattacks_from_level(self):
        # Render a press/release/re-press sequence twice; the blocks after
        # the re-press must be deterministic and must NOT equal a fresh
        # press's first block (phase continues rather than restarting).
        def run():
            s = self._synth()
            env = synthio.Envelope(attack_time=0.001, decay_time=0.1,
                                   release_time=0.1, attack_level=1.0,
                                   sustain_level=0.0)
            n = synthio.Note(50.0, waveform=SINE, envelope=env, amplitude=0.8)
            s.press(n)
            first = bytes(audiocore.get_buffer(s)[1])
            audiocore.get_buffer(s)
            s.release(n)
            audiocore.get_buffer(s)
            s.press(n)
            re1 = bytes(audiocore.get_buffer(s)[1])
            return first, re1
        first_a, re_a = run()
        first_b, re_b = run()
        self.assertEqual(re_a, re_b)
        self.assertEqual(first_a, first_b)
        self.assertNotEqual(first_a, re_a)

    def test_fresh_press_starts_at_phase_zero(self):
        s = self._synth()
        n = synthio.Note(50.0, waveform=SINE, amplitude=0.8)
        n._accum = 12345
        s.press(n)
        first = bytes(audiocore.get_buffer(s)[1])
        s2 = self._synth()
        n2 = synthio.Note(50.0, waveform=SINE, amplitude=0.8)
        s2.press(n2)
        self.assertEqual(first, bytes(audiocore.get_buffer(s2)[1]))


class FilterCascade(unittest.TestCase):
    """Note.filter as a tuple of Biquads - the audiodsp extension (#11)."""

    def _noise_render(self, filt, blocks=40):
        noise = array.array("h", [((i * 12347) % 30001) - 15000
                                  for i in range(8192)])
        s = synthio.Synthesizer(sample_rate=48000, channel_count=1)
        n = synthio.Note(48000 / 8192.0, waveform=noise, amplitude=0.8,
                         filter=filt)
        s.press(n)
        out = bytearray()
        for _ in range(blocks):
            out.extend(bytes(audiocore.get_buffer(s)[1]))
        return bytes(out)

    def test_single_element_tuple_matches_single_filter(self):
        one = self._noise_render(
            synthio.Biquad(synthio.FilterMode.LOW_PASS, 2000.0, Q=0.707))
        tup = self._noise_render(
            (synthio.Biquad(synthio.FilterMode.LOW_PASS, 2000.0, Q=0.707),))
        self.assertEqual(one, tup)

    def test_cascade_is_steeper(self):
        def hf_energy(pcm):
            total = 0
            count = len(pcm) // 2
            previous = 0
            for i in range(count):
                v = pcm[2 * i] | (pcm[2 * i + 1] << 8)
                if v >= 32768:
                    v -= 65536
                d = v - previous
                total += d * d
                previous = v
            return total
        b = lambda: synthio.Biquad(synthio.FilterMode.LOW_PASS, 2000.0, Q=0.707)
        one = hf_energy(self._noise_render(b()))
        two = hf_energy(self._noise_render((b(), b())))
        self.assertLess(two, one // 2)

    def test_validation(self):
        b = synthio.Biquad(synthio.FilterMode.LOW_PASS, 2000.0, Q=0.707)
        with self.assertRaises(ValueError):
            synthio.Note(100.0, filter=(b, b, b, b, b))
        with self.assertRaises(TypeError):
            synthio.Note(100.0, filter=(b, "nope"))
        synthio.Note(100.0, filter=(b, b, b, b))  # four stages: fine


class RefusedCountTest(unittest.TestCase):
    """audiodsp#127. A refused press is the one failure nobody can hear go
    wrong: the note never sounds, and until this counter nothing inside or
    outside could see it happen -- audiocomponents#26 had to wrap `press`
    with its own predicate to count 86 of them over the parity sequence.

    The counter, and nothing else: no stealing policy, and `max_polyphony`
    has not moved (Brad, 2026-09-22).
    """

    def _synth(self):
        return synthio.Synthesizer(sample_rate=8000, channel_count=1)

    def test_it_starts_at_zero_and_stays_there_while_there_is_room(self):
        synth = self._synth()
        self.assertEqual(synth.refused, 0)
        synth.press([synthio.Note(frequency=110.0 + step)
                     for step in range(synth.max_polyphony)])
        self.assertEqual(synth.refused, 0,
                         "a press that fits must not count as refused")

    def test_it_counts_exactly_the_presses_that_had_nowhere_to_go(self):
        synth = self._synth()
        over = 12
        notes = [synthio.Note(frequency=110.0 + step)
                 for step in range(synth.max_polyphony + over)]
        synth.press(notes)
        self.assertEqual(synth.refused, over)

    def test_a_release_that_finds_nothing_is_not_a_refusal(self):
        """The same code path returns False for a release of a note that is
        not held. That is not a note which never sounded, and counting it
        would make the number mean two things."""
        synth = self._synth()
        synth.release([synthio.Note(frequency=440.0)])
        self.assertEqual(synth.refused, 0)

    def test_it_is_monotonic_across_a_release_all(self):
        synth = self._synth()
        synth.press([synthio.Note(frequency=110.0 + step)
                     for step in range(synth.max_polyphony + 3)])
        before = synth.refused
        synth.release_all()
        self.assertEqual(synth.refused, before)
        self.assertEqual(before, 3)


if __name__ == "__main__":
    unittest.main()
