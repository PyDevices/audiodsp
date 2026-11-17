# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Beats to seconds and back, through a piecewise-constant tempo map.

A tempo map is a list of rows, each ``(beat, bpm)`` or
``(beat, bpm, ts_numerator, ts_denominator)``, giving the tempo in force from
that beat until the next row. The first row starts at beat 0.
"""

from collections.abc import Iterable, Sequence
from typing import Any, Self

class TempoMap:
    """The tempo map of one composition, queryable in both directions."""

    rows: list[tuple[float, ...]]
    total_beats: float
    sample_rate: int
    def __init__(self, rows: Iterable[Sequence[float]], total_beats: float, sample_rate: int) -> None: ...
    @classmethod
    def of(cls, composition: Any, sample_rate: int | None = None) -> Self:
        """The tempo map of a composition module or object."""
    def beats_to_seconds(self, beat: float) -> float:
        """Where `beat` falls, in seconds from the start."""
    def beat_at_sample(self, sample: int) -> float:
        """The inverse of `beats_to_seconds`."""
    def bpm_at_beat(self, beat: float) -> float:
        """The tempo in force at `beat`."""
    def timesig_at_beat(self, beat: float) -> tuple[int, int]:
        """The time signature in force, 4/4 unless a row says otherwise."""
    def transport_at(self, sample: int) -> tuple[bool, float, float, int, int]:
        """A host transport reading for `sample`: ``(playing, seconds, bpm,
        ts_numerator, ts_denominator)``."""
