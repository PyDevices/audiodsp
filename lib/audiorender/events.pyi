# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Turn a track's notes and automation into a time-ordered event list.

An event is ``(sample_position, kind, data, value)``, where `kind` is one of the
constants below and `value` is normalized 0.0 to 1.0 for a macro, or a velocity
for a note-on.
"""

from collections.abc import Mapping
from typing import Any, Protocol

from .tempo import TempoMap

NOTE_OFF: int
NOTE_ON: int
MACRO: int
PROGRAM: int
ORDER: dict[int, int]
"""The order events at one position are delivered in, by kind."""
MACRO_COUNT: int
"""How many macros a track can set: 16."""

# Types the API takes, for the checker only.
_Track = Mapping[str, Any]
_Event = tuple[int, int, int, float]

class _Composition(Protocol):
    """What a composition module or object carries. See the package docstring."""

    TITLE: str
    SAMPLE_RATE: int
    MASTER_GAIN_DB: float
    TEMPO_MAP: list[tuple[float, ...]]
    TOTAL_BEATS: float
    SONG_SECONDS: float
    RENDER_SECONDS: float
    SECTIONS: list[tuple[str, float, float]]
    TRACKS: list[dict[str, Any]]
    def beats_to_seconds(self, beat: float) -> float: ...
    def macro_value(self, track: _Track, index: int, beat: float) -> float: ...
    def track_gain(self, track: _Track, beat: float) -> float: ...
    def active_track_count(self, beat: float) -> int: ...

class _Instrument(Protocol):
    """An `audioinstruments` instrument, as far as a render drives it."""

    def note_on(self, note: int, velocity: int, *, sample_position: int = ...) -> None: ...
    def note_off(self, note: int, *, sample_position: int = ...) -> None: ...
    def set_macro(self, index: int, value: float, *, sample_position: int = ...) -> None: ...
    def program_change(self, program: int, *, sample_position: int = ...) -> None: ...

def build_events(
    track: _Track,
    composition: _Composition,
    tempo: TempoMap,
    patch: Mapping[int, float] | None = None,
    block: int = 256,
    macro_count: int = ...,
) -> list[_Event]:
    """Every event for one track, sorted by position and then by kind.

    Args:
        track: The track, with ``notes``, ``macros`` and ``macro_env``, and
            optionally ``programs``.
        composition: The composition the track belongs to.
        tempo: Its tempo map.
        patch: The instrument's patch 0 as ``{index: normalized value}``. A macro
            the composition does not mention takes its value from here, or 0.5.
        block: Frames between automation samples.
        macro_count: How many macros to set at the start.
    """

def deliver(instrument: _Instrument, event: _Event, sample_position: int) -> None:
    """Play one event on an `audioinstruments` instrument."""
