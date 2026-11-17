# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""What a finished render is worth looking at: levels, by track and section."""

from collections.abc import Callable
from typing import Any

from .events import _Composition
from .render import Master

def db(value: float) -> float:
    """Amplitude as dBFS, floored so silence prints as a number."""

def section_table(master: Master, out: Callable[[str], Any]) -> None:
    """Print each track's RMS in every section, in dBFS."""

def section_profile(master: Master, out: Callable[[str], Any]) -> None:
    """Print level, energy above 150 Hz and peak for each section."""

def busiest_beat(composition: _Composition) -> tuple[int, float]:
    """``(tracks, beat)`` where the most tracks sound at once."""

def report(master: Master, out: Callable[[str], Any] = ...) -> bool:
    """Print the whole report. Returns True when the render did not clip and
    did not ask for more simultaneous tracks than the piece allows."""
