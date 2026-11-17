# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""The block render loop: a composition in, a mixed master out."""

from collections.abc import Callable, Iterable, Mapping
from pathlib import Path
from typing import Any, Protocol

from .events import _Composition, _Track
from .tempo import TempoMap
from .voices import Clock

BLOCK: int
"""Frames per block: 256."""

class _Voice(Protocol):
    def deliver(self, event: Any, sample_position: int) -> None: ...
    def pull_frames(self, frames: int) -> bytes: ...

class Master:
    """A finished render: the audio, and the numbers a report is made of."""

    composition: _Composition
    tempo: TempoMap
    data: Any
    """The mixed audio, a float32 numpy array shaped ``(frames, 2)``."""
    sample_rate: int
    section_rms: dict[str, list[float]]
    bounds: list[tuple[int, int]]
    def __init__(
        self,
        composition: _Composition,
        tempo: TempoMap,
        data: Any,
        section_rms: dict[str, list[float]],
        bounds: list[tuple[int, int]],
    ) -> None: ...
    @property
    def peak(self) -> float:
        """The largest absolute sample in the master."""

def render_track(
    track: _Track,
    composition: _Composition,
    tempo: TempoMap,
    voice: _Voice,
    total_frames: int,
    patch: Mapping[int, float] | None = None,
    effects: Iterable[Callable[[bytes], Any]] = (),
    clock: Clock | None = None,
    block: int = ...,
) -> Any:
    """One track's audio as a float32 ``(total_frames, 2)`` array in -1.0..1.0."""

def apply_mix(track: _Track, data: Any, composition: _Composition, tempo: TempoMap, block: int = ...) -> Any:
    """Apply the fader moves and pan in place, and return the same array."""

def render(
    composition: _Composition,
    voice_for: Callable[[_Track, Clock], _Voice],
    patch_for: Callable[[_Track], Mapping[int, float]] | None = None,
    effects_for: Callable[[_Track], Iterable[Callable[[bytes], Any]]] | None = None,
    clock: Clock | None = None,
    sample_rate: int | None = None,
    block: int = ...,
    out: Callable[[str], Any] | None = None,
    stems: Path | None = None,
) -> Master:
    """Render every track of `composition` and mix them into a master.

    Args:
        composition: The composition module or object.
        voice_for: ``voice_for(track, clock)`` returns the voice to render a
            track with.
        patch_for: ``patch_for(track)`` returns ``{macro index: normalized
            value}`` for the macros the composition does not set.
        effects_for: ``effects_for(track)`` returns the track's effect stages.
        clock: The transport to publish. One is made when omitted.
        sample_rate: Overrides the composition's rate.
        block: Frames per block.
        out: Where per-track progress lines go. None keeps them quiet.
        stems: A folder to write each track's audio to, as WAV files.
    """
