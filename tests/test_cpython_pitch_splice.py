"""audiodelays.PitchShift: a held note plays its shifted pitch at every splice.

`PitchShift` reads a window of what was played at the shifted rate, and when
the read catches the write (or the write the read) it splices to audio a
whole window away, crossfading over `overlap` frames. Upstream CircuitPython
splices wherever that lands, so on every note whose period goes into the
window an odd number of halves the two sides of the crossfade are in
antiphase, and a short crossfade restarts the note's phase at every splice:
what comes out is the note's neighbours a splice rate either side, and
almost nothing at the shifted pitch. Here the incoming side is moved, by up
to a 50th of a second, to where it matches the outgoing one
(docs/upstream-diff.md). This file holds that on the CPython twin, which
runs the same kernel as the native node; `verify_dsp.py` holds the native
builds to the same bytes.

| ID | Trait | Bar |
|---|---|---|
| P1 | A held note, C2 to C6, plays its shifted pitch, at the windows the effect classes use, up and down, at 48, 44.1 and 22.05 kHz | the line at f * r within 3 dB of the note's own level |
| P2 | Silence and a click splice where upstream's does | the bytes of a node that never moves a splice |

Before the move, 52 of P1's 100 notes failed, some by 50 dB and more.
"""

import unittest
from array import array

import audiocore
import audiodelays

try:
    import numpy as np
except ImportError:     # pragma: no cover - numpy is a CI dependency
    np = None


LEVEL = 10000


def _render(node, values, rate):
    node.play(audiocore.RawSample(values, sample_rate=rate))
    out = array("h")
    while len(out) < len(values):
        out.frombytes(bytes(audiocore.get_buffer(node)[1]))
    return out[:len(values)]


def _shifter(rate, semitones, window_ms, overlap_ms):
    return audiodelays.PitchShift(
        semitones=semitones, mix=1.0,
        window=int(window_ms * rate / 1000) * 2,
        overlap=int(overlap_ms * rate / 1000) * 2,
        sample_rate=rate, channel_count=1, buffer_size=512)


@unittest.skipIf(np is None, "P1 measures a spectrum and needs numpy")
class HeldNoteTest(unittest.TestCase):
    """P1."""

    #: (rate, semitones, window ms, overlap ms): ShimmerHall's Bloom, the
    #: rebuilt PitchShifter, and the plain shifter's 1024-frame window.
    CELLS = ((48000, 12.0, 40.0, 10.0), (22050, -12.0, 40.0, 10.0),
             (44100, 7.0, 30.0, 4.0 / 3.0), (48000, 12.0, 21.333, 4.0 / 3.0))

    def _level_db(self, rate, midi, semitones, window_ms, overlap_ms):
        hz = 440.0 * 2.0 ** ((midi - 69) / 12.0)
        values = array("h", [int(LEVEL * np.sin(2 * np.pi * hz * n / rate))
                             for n in range(rate)])
        out = _render(_shifter(rate, semitones, window_ms, overlap_ms),
                      values, rate)
        held = np.array(out[int(0.3 * rate):], dtype=float)
        window = np.hanning(len(held))
        spectrum = np.abs(np.fft.rfft(held * window)) / (window.sum() / 2)
        bins = np.fft.rfftfreq(len(held), 1.0 / rate)
        shifted = hz * 2.0 ** (semitones / 12.0)
        near = (bins > shifted * 0.985) & (bins < shifted * 1.015)
        return 20.0 * np.log10(spectrum[near].max() / LEVEL + 1e-12)

    def test_held_notes_play_their_shifted_pitch(self):
        for rate, semitones, window_ms, overlap_ms in self.CELLS:
            for midi in range(36, 85, 2):
                with self.subTest(rate=rate, semitones=semitones,
                                  window_ms=window_ms, midi=midi):
                    self.assertGreater(
                        self._level_db(rate, midi, semitones, window_ms,
                                       overlap_ms), -3.0)


class QuietSpliceTest(unittest.TestCase):
    """P2: a move is only taken where it is better, and a tie keeps none, so
    silence and a lone click render what they rendered before the move
    existed. The reference is the kernel with the move switched off, which
    is upstream's splice."""

    def test_silence_and_a_click_are_unmoved(self):
        import _audiodsp
        rate = 8000
        for name, values in (
                ("silence", array("h", [0] * 4096)),
                ("click", array("h", [0] * 300 + [20000] + [0] * 3795))):
            for read_rate in (128, 384):
                with self.subTest(material=name, read_rate=read_rate):
                    out = []
                    for span in (0, rate // 50):
                        window = array("h", [0] * 512)
                        overlap = array("h", [0] * 64)
                        result = _audiodsp.pitchshift_s16(
                            values.tobytes(), window, overlap, 512, 64, 1,
                            read_rate, 0, 0, 0, 2.0, False, span, 0,
                            False, False)
                        out.append(result[0])
                    self.assertEqual(out[0], out[1])


if __name__ == "__main__":
    unittest.main()
