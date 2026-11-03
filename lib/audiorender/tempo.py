# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Beats to seconds and back, through a piecewise-constant tempo map.

A tempo map is a list of rows, each ``(beat, bpm)`` or
``(beat, bpm, ts_numerator, ts_denominator)``, giving the tempo in force
from that beat until the next row. The first row starts at beat 0.

Both directions are exact rather than incremental: a render walks the map
for every block, and accumulating seconds block by block would drift.
"""


class TempoMap:
    """The tempo map of one composition, queryable in both directions."""

    def __init__(self, rows, total_beats, sample_rate):
        self.rows = [tuple(row) for row in rows]
        self.total_beats = total_beats
        self.sample_rate = sample_rate
        #: (beat, bpm) only - the shape the arithmetic below needs.
        self._spans = [(row[0], row[1]) for row in self.rows]

    @classmethod
    def of(cls, composition, sample_rate=None):
        """The tempo map of a composition module (see the package docstring)."""
        return cls(composition.TEMPO_MAP, composition.TOTAL_BEATS,
                   sample_rate or composition.SAMPLE_RATE)

    def beats_to_seconds(self, beat):
        """Where `beat` falls, in seconds from the start."""
        seconds = 0.0
        for index, (start, bpm) in enumerate(self._spans):
            end = (self._spans[index + 1][0]
                   if index + 1 < len(self._spans) else None)
            if end is None or beat <= end:
                return seconds + max(0.0, beat - start) * 60.0 / bpm
            seconds += (end - start) * 60.0 / bpm
        return seconds

    def beat_at_sample(self, sample):
        """The inverse of `beats_to_seconds`, piecewise exact."""
        seconds = sample / self.sample_rate
        elapsed = 0.0
        for index, (start, bpm) in enumerate(self._spans):
            end = (self._spans[index + 1][0]
                   if index + 1 < len(self._spans) else None)
            span = None if end is None else (end - start) * 60.0 / bpm
            if span is None or seconds <= elapsed + span:
                return start + (seconds - elapsed) * bpm / 60.0
            elapsed += span
        return self.total_beats

    def bpm_at_beat(self, beat):
        current = self._spans[0][1]
        for start, bpm in self._spans:
            if beat >= start:
                current = bpm
        return current

    def timesig_at_beat(self, beat):
        """The time signature in force, defaulting to 4/4.

        Only rows that carry one change it, so a map of bare ``(beat, bpm)``
        pairs stays in 4/4 throughout.
        """
        numerator, denominator = 4, 4
        for row in self.rows:
            if beat >= row[0] and len(row) >= 4:
                numerator, denominator = row[2], row[3]
        return numerator, denominator

    def transport_at(self, sample):
        """A host transport reading for `sample`: what an instrument sees.

        ``(playing, seconds, bpm, ts_numerator, ts_denominator)`` - the
        shape `audioinstruments` factories expect from their `transport`
        callable.
        """
        beat = self.beat_at_sample(sample)
        numerator, denominator = self.timesig_at_beat(beat)
        return (True, sample / self.sample_rate, self.bpm_at_beat(beat),
                numerator, denominator)
