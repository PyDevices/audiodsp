# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""A released note's channel comes back before its tail has finished.

The oracle does not simply refuse a press at capacity. When no channel is
free, ``find_channel_with_note`` is called with ``SYNTHIO_SILENCE``
(``src/synthio/__init__.c:361``) and scans the channels where the note is
*not playing* — released ones, still sounding out their tails — taking the
quietest. Only when every channel is genuinely held does it return -1 and
refuse.

This target used to hold a released note's slot for its entire release tail,
because ``_render`` drops a note only once its level reaches 0. The effect was
not subtle: with all fourteen notes released a fresh press was still refused,
and the slot returned 758 render blocks later. Driving ``rhodes`` through the
parity sequence, **117 of its 122 refusals happened with no key held at all** —
the engine clogged by its own decaying voices rather than full.

Verified against the parent workspace's built ``circuitpython`` and
``micropython``: all three now report the same pair of outcomes.
"""

import unittest

import audiocore
import synthio

SAMPLE_RATE = 48000
ENVELOPE = synthio.Envelope(attack_time=0.001, decay_time=0.3,
                            release_time=0.5, attack_level=1.0,
                            sustain_level=0.4)


class ReleasedChannelReclaim(unittest.TestCase):
    def _fill(self, release_them):
        synth = synthio.Synthesizer(sample_rate=SAMPLE_RATE, channel_count=1)
        cap = synth.max_polyphony
        held = [synthio.Note(220.0 + 11 * i, envelope=ENVELOPE, amplitude=0.2)
                for i in range(cap)]
        for note in held:
            synth.press(note)
        if release_them:
            for note in held:
                synth.release(note)
        for _ in range(3):
            audiocore.get_buffer(synth)
        return synth, held

    def test_released_notes_give_their_channels_back(self):
        """The case that was broken: nothing held, everything still ringing."""
        synth, _held = self._fill(release_them=True)
        fresh = synthio.Note(999.0, envelope=ENVELOPE, amplitude=0.2)
        synth.press(fresh)
        self.assertIn(fresh, synth.pressed)

    def test_a_held_note_is_never_evicted(self):
        """The guard: at capacity with everything held, the press is refused.

        A press that succeeded here would mean a note the player is holding
        was stolen, which is the opposite defect and worse than the one above.
        """
        synth, held = self._fill(release_them=False)
        fresh = synthio.Note(999.0, envelope=ENVELOPE, amplitude=0.2)
        synth.press(fresh)
        self.assertNotIn(fresh, synth.pressed)
        for note in held:
            self.assertIn(note, synth.pressed)

    def test_reclaim_is_repeatable_and_does_not_leak(self):
        """Reclaiming must not lose a slot each time it happens.

        Which victim is chosen cannot be asserted through the public API -
        ``pressed`` excludes released notes, so it cannot show which released
        note survived. The oracle takes the QUIETEST, and this target now runs
        the same rule; that specific choice is covered by the shared behaviour
        test against the built oracle rather than here. What is checkable, and
        what would break first if the reclaim leaked, is that it keeps working.
        """
        synth, held = self._fill(release_them=True)
        for i in range(synth.max_polyphony * 2):
            fresh = synthio.Note(900.0 + i, envelope=ENVELOPE, amplitude=0.2)
            synth.press(fresh)
            self.assertIn(fresh, synth.pressed,
                          "reclaim stopped working after %d presses - a slot "
                          "is being lost each time" % i)
            synth.release(fresh)
            audiocore.get_buffer(synth)


if __name__ == "__main__":
    unittest.main()
