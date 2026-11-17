# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""What a render pulls audio from, and how it tells them the time.

A voice is anything a render can drive: it takes the events
`audiorender.events` builds and hands back frames, with
``deliver(event, sample_position)`` and ``pull_frames(frames)``.
"""

from typing import Any

from .events import _Event, _Instrument
from .events import deliver as deliver
from .tempo import TempoMap

class Puller:
    """Frames from an audiodsp node, in whatever size the caller asks for."""

    node: Any
    def __init__(self, node: Any) -> None: ...
    def pull_frames(self, frames: int) -> bytes:
        """Interleaved little-endian stereo int16, `frames` frames of it."""

class Voice(Puller):
    """One `audioinstruments` instrument, ready to be rendered."""

    instrument: _Instrument
    def __init__(self, instrument: _Instrument) -> None: ...
    def deliver(self, event: _Event, sample_position: int) -> None: ...

class Clock:
    """The transport reading a render publishes, one block at a time.

    Pass one as the `transport` callable of an instrument that locks to tempo,
    and give the same object to the render loop.
    """

    tempo: TempoMap
    reading: tuple[bool, float, float, int, int]
    def __init__(self, tempo: TempoMap) -> None: ...
    def __call__(self) -> tuple[bool, float, float, int, int]: ...
    def move_to(self, sample: int) -> None: ...

class PcmSource:
    """A finite audiodsp source over interleaved stereo int16 `pcm`."""

    sample_rate: int
    channel_count: int
    bits_per_sample: int
    samples_signed: bool
    def __init__(
        self, pcm: bytes | bytearray | memoryview, sample_rate: int, channel_count: int = 2, chunk_bytes: int = 2048
    ) -> None: ...
