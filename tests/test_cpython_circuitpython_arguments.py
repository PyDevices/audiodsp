# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Where CircuitPython has a call, the CPython twin takes the arguments it does.

Each case was run on a CircuitPython 11.0.0-alpha.2 desktop build: the calls
below that raise there raise here, and the ones that work there work here.

    python -m unittest tests.test_cpython_circuitpython_arguments
"""

import io
import unittest

import audiocore
import audiomixer
import audiomp3
import synthio


class SynthesizerArgumentTests(unittest.TestCase):
    def setUp(self):
        self.synth = synthio.Synthesizer(sample_rate=22050)

    def test_press_release_and_note_info_take_the_note_by_position_only(self):
        for name in ("press", "release", "release_all_then_press"):
            with self.subTest(name=name):
                call = getattr(self.synth, name)
                call(60)
                with self.assertRaises(TypeError):
                    call(notes=60)
        self.synth.note_info(60)
        with self.assertRaises(TypeError):
            self.synth.note_info(note=60)

    def test_midi_to_hz_and_voct_to_hz_take_one_positional_argument(self):
        self.assertAlmostEqual(synthio.midi_to_hz(60), 261.625, places=3)
        self.assertAlmostEqual(synthio.voct_to_hz(1), 130.8125, places=4)
        with self.assertRaises(TypeError):
            synthio.midi_to_hz(note=60)
        with self.assertRaises(TypeError):
            synthio.voct_to_hz(ctrl=1)

    def test_change_takes_release_press_and_retrigger_by_position_or_name(self):
        self.synth.press(60)
        self.synth.change(60)
        self.assertEqual(self.synth.note_info(60)[0],
                         synthio.EnvelopeState.RELEASE)
        self.synth.change(press=61)
        self.synth.release_then_press(61, 62)
        self.synth.release_then_press(release=62, press=63)

    def test_lfo_tick_needs_a_block(self):
        with self.assertRaises(TypeError):
            synthio.lfo_tick()
        self.assertEqual(len(synthio.lfo_tick(synthio.LFO())), 1)


class FileAndMixerArgumentTests(unittest.TestCase):
    def test_a_mixer_voice_stop_accepts_and_ignores_an_argument(self):
        mixer = audiomixer.Mixer(voice_count=1, sample_rate=8000)
        mixer.voice[0].stop()
        mixer.voice[0].stop(0)
        with self.assertRaises(TypeError):
            mixer.voice[0].stop(0, 1)

    def test_decoders_take_their_file_by_position_only(self):
        for decoder in (audiocore.WaveFile, audiomp3.MP3Decoder):
            with self.subTest(decoder=decoder.__name__):
                with self.assertRaises(TypeError):
                    decoder(file=io.BytesIO(b""))

    def test_exit_takes_its_three_arguments_by_position_only(self):
        sample = audiomixer.Mixer(voice_count=1, sample_rate=8000)
        with self.assertRaises(TypeError):
            sample.__exit__(exc_type=None, exc=None, tb=None)
        with self.assertRaises(TypeError):
            sample.__exit__()
        sample.__exit__(None, None, None)


if __name__ == "__main__":
    unittest.main()
