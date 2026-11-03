# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""audiorender: a whole (small) piece, from a composition to a WAV.

The sound source is the smallest instrument audiorender can drive - a
`synthio.Synthesizer` behind the MIDI-shaped surface
`audiorender.events.deliver` speaks - defined here rather than borrowed from
`audioinstruments`. That library lives in the audiocomponents repository
now, and audiorender's contract never asked for it: `render()` wants a
`voice_for(track, clock)` and only `deliver()` and `pull_frames()` back.
Nothing here asserts what an instrument sounds like, only what the renderer
does with one.
"""

import hashlib
import os
import tempfile
import unittest
import wave

import audiorender
import synthio

SAMPLE_RATE = 48000

#: Fast attack, a short tail: every note in the piece is audible in its own
#: section and none rings far into the next, which is what the per-section
#: levels below are counting on.
ENVELOPE = synthio.Envelope(attack_time=0.005, decay_time=0.1,
                            release_time=0.05, attack_level=1.0,
                            sustain_level=0.6)


class Instrument:
    """A synthio voice with the surface `audiorender.events.deliver` speaks."""

    def __init__(self, sample_rate, transport=None):
        self.transport = transport
        self.output = synthio.Synthesizer(sample_rate=sample_rate,
                                          channel_count=2, envelope=ENVELOPE)
        self._notes = {}
        self.macros = {}

    def note_on(self, pitch, velocity, sample_position=None):
        self.note_off(pitch)
        note = synthio.Note(synthio.midi_to_hz(pitch),
                            amplitude=0.4 * velocity / 127.0)
        self._notes[pitch] = note
        self.output.press(note)

    def note_off(self, pitch, sample_position=None):
        note = self._notes.pop(pitch, None)
        if note is not None:
            self.output.release(note)

    def set_macro(self, index, value, sample_position=None):
        self.macros[index] = value

    def program_change(self, index, sample_position=None):
        pass


class Piece:
    """Two bars of two instruments - the composition contract, minimally.

    Half of it at 120 bpm and half at 60, so nothing here can pass by
    ignoring the tempo map.
    """

    TITLE = "Smoke"
    SAMPLE_RATE = SAMPLE_RATE
    MASTER_GAIN_DB = -3.0
    ACTIVE_LIMIT = 2
    TEMPO_MAP = [(0.0, 120.0, 4, 4), (4.0, 60.0, 4, 4)]
    TOTAL_BEATS = 8.0
    SECTIONS = [("A Fast", 0.0, 4.0), ("B Slow", 4.0, 8.0)]

    TRACKS = [
        {
            "name": "Drums",
            "script": "drums.py",
            "instrument": "drums",
            "pan": -0.2,
            "vol": 1.0,
            "gain_db": -6.0,
            # Every beat, alternating kick and snare.
            "notes": [(float(beat), 0.5, 36 if beat % 2 == 0 else 38, 1.0)
                      for beat in range(8)],
            "macros": {0: 0.7},
            "macro_env": {},
        },
        {
            "name": "Bass",
            "script": "bass.py",
            "instrument": "bass",
            "pan": 0.0,
            "vol": 1.0,
            "gain_db": -3.0,
            "notes": [(0.0, 3.5, 36, 0.9), (4.0, 3.5, 41, 0.9)],
            "macros": {},
            # A filter sweep across the whole piece, so the macro sampler
            # has something to sample.
            "macro_env": {2: [(0.0, 0.2), (8.0, 0.9)]},
        },
    ]

    @staticmethod
    def beats_to_seconds(beat):
        return min(beat, 4.0) * 0.5 + max(0.0, beat - 4.0) * 1.0

    @staticmethod
    def macro_value(track, index, beat):
        envelope = track["macro_env"].get(index)
        if envelope:
            first, last = envelope[0], envelope[-1]
            if beat <= first[0]:
                return first[1]
            if beat >= last[0]:
                return last[1]
            span = (beat - first[0]) / (last[0] - first[0])
            return first[1] + (last[1] - first[1]) * span
        return track["macros"].get(index, 0.5)

    @staticmethod
    def track_gain(track, _beat):
        return 10.0 ** (track["gain_db"] / 20.0) * track["vol"]

    @staticmethod
    def active_track_count(beat):
        return sum(1 for track in Piece.TRACKS
                   if any(start <= beat < start + duration
                          for start, duration, _p, _v in track["notes"]))


Piece.SONG_SECONDS = Piece.beats_to_seconds(Piece.TOTAL_BEATS)
Piece.RENDER_SECONDS = Piece.SONG_SECONDS + 1.0


def voice_for(track, clock):
    return audiorender.Voice(Instrument(SAMPLE_RATE, transport=clock))


def render(out=None):
    return audiorender.render(Piece, voice_for, out=out)


class TempoMapTest(unittest.TestCase):

    def setUp(self):
        self.tempo = audiorender.TempoMap.of(Piece)

    def test_matches_the_composition(self):
        beat = 0.0
        while beat <= Piece.TOTAL_BEATS:
            self.assertEqual(self.tempo.beats_to_seconds(beat),
                             Piece.beats_to_seconds(beat), "at beat %s" % beat)
            beat += 0.125

    def test_inverts_itself(self):
        for sample in range(0, int(Piece.SONG_SECONDS * SAMPLE_RATE), 997):
            seconds = self.tempo.beats_to_seconds(
                self.tempo.beat_at_sample(sample))
            self.assertAlmostEqual(seconds, sample / SAMPLE_RATE, places=9)

    def test_reads_the_tempo_and_the_meter(self):
        self.assertEqual(self.tempo.bpm_at_beat(0.0), 120.0)
        self.assertEqual(self.tempo.bpm_at_beat(6.0), 60.0)
        self.assertEqual(self.tempo.timesig_at_beat(6.0), (4, 4))
        playing, seconds, bpm, numerator, denominator = (
            self.tempo.transport_at(SAMPLE_RATE))
        self.assertTrue(playing)
        self.assertEqual((seconds, bpm, numerator, denominator),
                         (1.0, 120.0, 4, 4))


class EventTest(unittest.TestCase):

    def setUp(self):
        self.tempo = audiorender.TempoMap.of(Piece)
        self.events = audiorender.build_events(
            Piece.TRACKS[0], Piece, self.tempo, patch={3: 0.25})

    def test_sorted_by_position_then_kind(self):
        keys = [(position, audiorender.ORDER[kind])
                for position, kind, _data, _value in self.events]
        self.assertEqual(keys, sorted(keys))

    def test_opens_with_every_macro(self):
        opening = [event for event in self.events if event[0] == 0
                   and event[1] == audiorender.MACRO]
        self.assertEqual(len(opening), audiorender.MACRO_COUNT)
        values = {event[2]: event[3] for event in opening}
        # What the composition sets, then what the patch says, and only
        # then the middle of the range.
        self.assertEqual(values[0], 0.7)
        self.assertEqual(values[3], 0.25)
        self.assertEqual(values[9], 0.5)

    def test_every_note_ends_after_it_starts(self):
        for position, kind, pitch, _value in self.events:
            if kind != audiorender.NOTE_ON:
                continue
            offs = [event[0] for event in self.events
                    if event[1] == audiorender.NOTE_OFF and event[2] == pitch
                    and event[0] > position]
            self.assertTrue(offs, "note %d at %d never ends" % (pitch,
                                                                position))


class RenderTest(unittest.TestCase):

    def test_renders_both_tracks_and_both_sections(self):
        lines = []
        master = render(out=lines.append)
        self.assertEqual(len(master.data), int(Piece.RENDER_SECONDS
                                               * SAMPLE_RATE))
        self.assertEqual(len(lines), 1 + len(Piece.TRACKS))
        self.assertIn("Smoke", lines[0])
        for track in Piece.TRACKS:
            for level in master.section_rms[track["name"]]:
                self.assertGreater(level, 1e-4,
                                   "%s is silent somewhere" % track["name"])

    def test_the_master_is_audible_and_does_not_clip(self):
        master = render()
        self.assertGreater(master.peak, 0.05)
        self.assertLess(master.peak, 1.0)
        report = []
        self.assertTrue(audiorender.report(master, out=report.append))
        self.assertTrue(any("max simultaneous tracks: 2" in line
                            for line in report))

    def test_the_same_piece_renders_the_same_wav_twice(self):
        digests = []
        with tempfile.TemporaryDirectory() as scratch:
            for attempt in range(2):
                path = os.path.join(scratch, "smoke%d.wav" % attempt)
                master = render()
                audiorender.write_wav(path, master.data, master.sample_rate)
                with wave.open(path) as handle:
                    self.assertEqual(handle.getnchannels(), 2)
                    self.assertEqual(handle.getsampwidth(), 2)
                    self.assertEqual(handle.getframerate(), SAMPLE_RATE)
                    self.assertEqual(handle.getnframes(),
                                     int(Piece.RENDER_SECONDS * SAMPLE_RATE))
                with open(path, "rb") as handle:
                    digests.append(
                        hashlib.sha256(handle.read()).hexdigest())
        self.assertEqual(digests[0], digests[1])


if __name__ == "__main__":
    unittest.main()
