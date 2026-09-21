"""A voice at envelope level 0 contributes nothing, on every target.

CircuitPython ends a voice the moment its envelope level hits exactly 0: the
channel goes back to ``SYNTHIO_SILENCE`` and is skipped *before* anything is
rendered from it (``shared-module/synthio/__init__.c``; ``src/synthio/__init__.c``
does the same). This target used to render it anyway, and for a long time that
was harmless — pending loudness 0 times any sample is 0, so the voice
contributed silence and nobody could tell.

CircuitPython 10.3.0's zero-crossing loudness gate ended that. The gate holds
the loudness a voice LAST rendered at until the waveform is zero or changes
sign, so a voice dropping to level 0 kept sounding at its previous loudness for
as much as a whole block — material this target added and the native builds did
not. It cost one block of ``synthtools_acceptance``'s ``bass``, the first after
``note_off``, and nothing else: the gate is forced to pending at every block
boundary, so it could not propagate past the block it happened in. That is what
made it look like rounding rather than an extra voice. audiodsp#78.

## Why the material is a constant

The trait needs a waveform with **no zero crossing at all**, because a crossing
opens the gate and hides the defect — the voice would then correctly fall to
zero part-way through the block and the totals would be close enough to argue
about. Held at full scale the gate can never open, so a rendered level-0 voice
emits its whole previous loudness for the whole block and the reading is
unambiguous: exactly zero, or nowhere near it.

## The block to look at is the SECOND one after release

With ``release_time=0`` the envelope steps to 0 at the *end* of the block that
follows the release, so that block still renders at full level — measured
identically on all three targets, and the trait asserts it as the control. The
voice is at level 0 when the block after THAT comes round, and that is the one
the defect showed up in.
"""

import array
import unittest

import audiocore
import synthio


RATE = 48000
BLOCK = 256


class SilencedVoiceTest(unittest.TestCase):
    @staticmethod
    def _block(synth):
        return array.array("h", bytes(audiocore.get_buffer(synth)[1]))

    def _sounding_then_released(self):
        """A loud voice on a constant waveform, released with no release ramp.

        ``release_time=0`` puts the level at exactly 0 on the first step after
        release, which is the case that matters: there is no ramp to hide
        behind.
        """
        synth = synthio.Synthesizer(sample_rate=RATE, channel_count=1)
        note = synthio.Note(
            frequency=110.0,
            waveform=array.array("h", [32767] * 64),
            envelope=synthio.Envelope(
                attack_time=0.0, decay_time=0.0, release_time=0.0,
                attack_level=1.0, sustain_level=1.0),
        )
        synth.press(note)
        loud = [self._block(synth) for _ in range(3)]
        synth.release(note)
        return synth, loud

    def test_the_first_silent_block_is_exactly_silent(self):
        synth, loud = self._sounding_then_released()
        # Two controls, so "silent" below is a measurement and not a
        # description of a synth that never started or stopped early.
        self.assertGreater(max(abs(v) for v in loud[-1]), 8000)
        still_ramping = self._block(synth)
        self.assertGreater(max(abs(v) for v in still_ramping), 8000)

        after = self._block(synth)
        self.assertEqual(
            set(after), {0},
            "a voice at envelope level 0 was rendered; peak %d"
            % max(abs(v) for v in after))

    def test_it_stays_silent_and_the_voice_is_gone(self):
        synth, _loud = self._sounding_then_released()
        self._block(synth)
        for _ in range(4):
            self.assertEqual(set(self._block(synth)), {0})
        self.assertEqual(synth.pressed, ())

    def test_the_freed_channel_keeps_its_gate_state(self):
        """The skip frees the channel; it does NOT clear the channel's gate.

        CircuitPython never clears ``active_loudness[chan]`` — not on
        reset_buffer, not when a channel changes hands. So a re-press at the
        same loudness finds the gate already open and sounds on its very first
        block, with no gated block at the front. Measured on all three targets
        (2026-09-09): peak 16382 on the first block and every one after.

        This is asserted because the obvious "tidy" reading of the fix — free
        the channel and zero its gate — is wrong, and would cost a silent block
        at the head of every reused voice.
        """
        synth, _loud = self._sounding_then_released()
        self._block(synth)
        self._block(synth)

        again = synthio.Note(
            frequency=110.0,
            waveform=array.array("h", [32767] * 64),
            envelope=synthio.Envelope(
                attack_time=0.0, decay_time=0.0, release_time=0.0,
                attack_level=1.0, sustain_level=1.0),
        )
        synth.press(again)
        for index in range(3):
            block = self._block(synth)
            self.assertGreater(
                max(abs(v) for v in block), 8000,
                "reused channel was gated on block %d" % index)


if __name__ == "__main__":
    unittest.main()
