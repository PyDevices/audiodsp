# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Reassigning ``note.envelope`` takes effect on the next render block.

The MicroPython and CircuitPython builds fetch a note's envelope on every
block (``synthio_synth_get_note_envelope``, called from the render loop), so
assigning ``note.envelope`` is live. This target used to cache the envelope
definition inside the state object at press time, so a re-pressed note kept
stepping the PREVIOUS envelope.

That divergence was invisible to a one-shot render and only appeared when one
circuit was shared by two voices with different decays - the pattern several
audioinstruments kits use for closed/open hats and for clap/maracas. Measured
on a shared claves+cowbell circuit before the fix: claves inherited the
cowbell's 203 ms tail (53 ms alone) and cowbell was cut to the claves' 55 ms.
Both runtimes and the built oracle were correct; only this target was wrong.

Verified against the parent workspace's built ``circuitpython`` and
``micropython``: all three now agree to the sample.
"""

import array
import math
import unittest

import audiocore
import synthio

SINE = array.array("h", [int(28000 * math.sin(2 * math.pi * i / 256))
                         for i in range(256)])

SHORT = synthio.Envelope(attack_time=0.001, decay_time=0.05,
                         release_time=0.02, attack_level=1.0,
                         sustain_level=0.0)
LONG = synthio.Envelope(attack_time=0.001, decay_time=0.40,
                        release_time=0.02, attack_level=1.0,
                        sustain_level=0.0)


def tail_ms(chunks, sample_rate=48000):
    """Milliseconds from the start until the signal falls under 1% of peak."""
    a = array.array("h")
    for c in chunks:
        a.frombytes(c)
    peak = max((abs(v) for v in a), default=0)
    if peak < 1:
        return 0.0
    threshold = peak // 100
    last = 0
    for i, v in enumerate(a):
        if abs(v) > threshold:
            last = i
    return last / sample_rate * 1000.0


class EnvelopeReassignment(unittest.TestCase):
    def _run(self, first, second, gap_blocks=8):
        synth = synthio.Synthesizer(sample_rate=48000, channel_count=1)
        note = synthio.Note(440.0, waveform=SINE, amplitude=1.0)
        note.envelope = first
        synth.press(note)
        for _ in range(gap_blocks):
            audiocore.get_buffer(synth)
        note.envelope = second      # reassign while it is still sounding
        synth.press(note)           # re-press
        chunks = []
        for _ in range(400):
            _result, buf = audiocore.get_buffer(synth)
            chunks.append(bytes(buf))
        return tail_ms(chunks)

    def test_repress_adopts_a_longer_envelope(self):
        # Was ~59 ms before the fix: it kept stepping SHORT.
        self.assertGreater(self._run(SHORT, LONG), 300.0)

    def test_repress_adopts_a_shorter_envelope(self):
        # Was ~405 ms before the fix: it kept stepping LONG.
        self.assertLess(self._run(LONG, SHORT), 150.0)

    def test_two_voices_sharing_one_note_keep_their_own_decays(self):
        """The shape that actually broke: one circuit, two voices."""
        short_tail = self._run(LONG, SHORT)
        long_tail = self._run(SHORT, LONG)
        self.assertLess(short_tail, long_tail / 2.0)

    def test_unchanged_envelope_is_not_disturbed(self):
        """The guard must not perturb the common case."""
        self.assertGreater(self._run(LONG, LONG), 300.0)
        self.assertLess(self._run(SHORT, SHORT), 150.0)


if __name__ == "__main__":
    unittest.main()
