# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Ring modulation actually modulates.

The CPython target used to accept `ring_frequency`/`ring_waveform` on a
Note, store them, and never read them again - so a ringed note rendered
byte-identically to an unringed one while MicroPython and the
CircuitPython oracle both applied the ring. These tests fail loudly if
that regresses, because the previous failure mode was silence rather
than an error.
"""

import array
import math
import unittest

import audiocore
import synthio

SINE = array.array("h", [int(30000 * math.sin(2 * math.pi * i / 256))
                         for i in range(256)])


def render(note, blocks=12, sample_rate=48000):
    synth = synthio.Synthesizer(sample_rate=sample_rate, channel_count=1)
    synth.press(note)
    out = bytearray()
    for _ in range(blocks):
        out.extend(bytes(audiocore.get_buffer(synth)[1]))
    return bytes(out)


class RingModulation(unittest.TestCase):
    def _note(self, **kwargs):
        return synthio.Note(220.0, waveform=SINE, amplitude=0.8, **kwargs)

    def test_ring_changes_the_output(self):
        plain = render(self._note())
        ringed = render(self._note(ring_frequency=55.0, ring_waveform=SINE))
        self.assertNotEqual(plain, ringed)

    def test_ring_needs_both_frequency_and_waveform(self):
        # The usermod requires ring_frequency_scaled != 0 AND a ring
        # waveform buffer; either alone is a no-op.
        plain = render(self._note())
        self.assertEqual(plain, render(self._note(ring_frequency=55.0)))
        self.assertEqual(plain, render(self._note(ring_waveform=SINE)))

    def test_ring_frequency_matters(self):
        a = render(self._note(ring_frequency=55.0, ring_waveform=SINE))
        b = render(self._note(ring_frequency=110.0, ring_waveform=SINE))
        self.assertNotEqual(a, b)

    def test_ring_is_deterministic(self):
        a = render(self._note(ring_frequency=55.0, ring_waveform=SINE))
        b = render(self._note(ring_frequency=55.0, ring_waveform=SINE))
        self.assertEqual(a, b)


if __name__ == "__main__":
    unittest.main()
