# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Six-operator FM synthesis that plays the classic six-operator voice format.

`Synth` is an audio source with a pool of voices: press and release notes on
it and play it like any other source. `Patch` is one voice, with every
parameter as an attribute, and reads and writes the 155-byte single voice and
the 4104-byte 32-voice bank that the format's instruments and editors
exchange.
"""

from types import TracebackType
from typing import Self, Sequence

__version__: str
"""The audiodsp release this module came from."""
__revision__: str
"""Which source this build was made from, from `git describe`: a release tag,
or the nearest one and the commit. "unknown" when the build had no git."""

FRAMES: int
"""Frames in one block: 256."""
MAX_VOICES: int
"""The most voices a `Synth` can have: 64."""

class Operator:
    """One of a `Patch`'s six operators, read and written in place.

    Every value is the format's own whole number. Setting one outside its
    range raises `ValueError`.
    """

    @property
    def number(self) -> int:
        """Which operator this is, 1 to 6."""
    @property
    def enabled(self) -> bool:
        """False mutes the operator without changing its settings. Not part
        of the 155 bytes `Patch.pack` writes."""
    @enabled.setter
    def enabled(self, value: bool) -> None: ...
    rates: tuple[int, int, int, int]
    """The envelope's four rates, 0 to 99. Higher is faster."""
    levels: tuple[int, int, int, int]
    """The envelope's four levels, 0 to 99. The fourth is where a released
    note falls to."""
    breakpoint: int
    """The key keyboard level scaling turns on, 0 to 99. 39 is middle C."""
    left_depth: int
    """Level scaling below the breakpoint, 0 to 99."""
    right_depth: int
    """Level scaling above the breakpoint, 0 to 99."""
    left_curve: int
    """0 falls linearly, 1 falls exponentially, 2 rises exponentially, 3
    rises linearly."""
    right_curve: int
    """As `left_curve`, above the breakpoint."""
    rate_scaling: int
    """How much faster the envelope runs on higher keys, 0 to 7."""
    amp_mod: int
    """How far the LFO moves this operator's level, 0 to 3."""
    velocity: int
    """How much key velocity changes the level, 0 to 7."""
    level: int
    """Output level, 0 to 99."""
    fixed: bool
    """True plays a fixed frequency instead of a ratio of the key's."""
    coarse: int
    """The frequency ratio, 0 to 31, where 0 is one half; as a fixed
    frequency, 0 to 3 picks 1, 10, 100 or 1000 Hz."""
    fine: int
    """Adds 0 to 99 percent to the ratio, or moves a fixed frequency up to
    nearly ten times."""
    detune: int
    """0 to 14, where 7 is in tune."""

class Patch:
    """One voice: six operators, the algorithm that wires them, and the rest.

    A new patch is the initial voice: operator 1 alone, a sine at full level.
    """

    def __init__(self) -> None: ...
    @property
    def operators(self) -> tuple[Operator, Operator, Operator, Operator, Operator, Operator]:
        """The six operators. ``operators[0]`` is operator 1."""
    @property
    def algorithm(self) -> int:
        """How the six operators are wired, 1 to 32."""
    @algorithm.setter
    def algorithm(self, value: int) -> None: ...
    @property
    def name(self) -> str:
        """Up to ten ASCII characters."""
    @name.setter
    def name(self, value: str) -> None: ...
    pitch_rates: tuple[int, int, int, int]
    """The pitch envelope's four rates, 0 to 99."""
    pitch_levels: tuple[int, int, int, int]
    """The pitch envelope's four levels, 0 to 99. 50 is no change."""
    feedback: int
    """How much the feedback operator feeds itself, 0 to 7."""
    key_sync: bool
    """True restarts every operator's phase on each note."""
    lfo_speed: int
    """0 to 99."""
    lfo_delay: int
    """How long the LFO waits after a note before fading in, 0 to 99."""
    lfo_pitch_depth: int
    """Vibrato depth, 0 to 99."""
    lfo_amp_depth: int
    """Tremolo depth, 0 to 99."""
    lfo_key_sync: bool
    """True restarts the LFO on each note."""
    lfo_wave: int
    """0 triangle, 1 saw down, 2 saw up, 3 square, 4 sine, 5 sample and
    hold."""
    lfo_pitch_sens: int
    """How far `lfo_pitch_depth` moves the pitch, 0 to 7."""
    transpose: int
    """0 to 48 semitones, where 24 is no change."""

    def pack(self) -> bytes:
        """The voice as 155 bytes, in the single-voice parameter order."""
    @staticmethod
    def unpack(data: bytes) -> Patch:
        """A patch from 155 voice bytes, or from a 163-byte single-voice
        system exclusive message.

        Values past a parameter's range are clamped.

        Raises:
            ValueError: the length is neither, or a message's header or
                checksum is wrong.
        """
    @staticmethod
    def bank(data: bytes) -> list[Patch]:
        """The 32 voices of a bank.

        Args:
            data: The 4104-byte system exclusive message, checked against
                its checksum, or its 4096 data bytes alone.

        Raises:
            ValueError: the length is neither, or the header or checksum is
                wrong.
        """
    @staticmethod
    def pack_bank(patches: Sequence[Patch]) -> bytes:
        """Up to 32 patches as a 4104-byte bank message. Missing slots get
        the initial voice."""

class Synth:
    """A polyphonic source that plays a `Patch`.

    Play it like any audio sample, or put it in a mixer. A new note takes a
    free voice, else the oldest released one, else the oldest held one.

    Args:
        voices: How many notes can sound at once, 1 to `MAX_VOICES`.
        sample_rate: Frames per second, 1000 or more.
        channel_count: 1, or 2 for the same signal on both channels.
        patch: The `Patch` to play. A new initial voice if not given.

    Raises:
        ValueError: an argument is out of range.
        TypeError: ``patch`` is not a `Patch`.
    """

    def __init__(self, voices: int = 16, sample_rate: int = 48000,
                 channel_count: int = 1, patch: Patch | None = None) -> None: ...
    def deinit(self) -> None:
        """Silence every voice and release the synth."""
    def __enter__(self) -> Self: ...
    def __exit__(self, exc_type: type[BaseException] | None, exc: BaseException | None, tb: TracebackType | None, /) -> None: ...
    @property
    def sample_rate(self) -> int: ...
    @property
    def bits_per_sample(self) -> int: ...
    @property
    def channel_count(self) -> int: ...
    def press(self, note: int, velocity: int = 100) -> None:
        """Start `note`, 0 to 127, at `velocity`, 0 to 127. The patch is read
        now, so edits to it reach the next note, never a sounding one."""
    def release(self, note: int) -> None:
        """Release every voice holding `note`."""
    def release_all(self) -> None:
        """Release every held note, as if every key came up."""
    @property
    def patch(self) -> Patch:
        """The patch new notes play."""
    @patch.setter
    def patch(self, value: Patch) -> None: ...
    @property
    def pitch_bend(self) -> float:
        """-1.0 to 1.0, clamped, scaled by `bend_range`."""
    @pitch_bend.setter
    def pitch_bend(self, value: float) -> None: ...
    @property
    def bend_range(self) -> int:
        """How many semitones a full `pitch_bend` moves, 0 to 24. The
        default is 2."""
    @bend_range.setter
    def bend_range(self, value: int) -> None: ...
    @property
    def mod_wheel(self) -> float:
        """0.0 to 1.0, clamped. Adds LFO pitch and amplitude depth on top of
        the patch's own, up to full depth."""
    @mod_wheel.setter
    def mod_wheel(self, value: float) -> None: ...
    @property
    def sustain(self) -> bool:
        """The sustain pedal. Lifting it releases the notes it held."""
    @sustain.setter
    def sustain(self, value: bool) -> None: ...
    @property
    def voices(self) -> int:
        """How many notes can sound at once."""
    @property
    def active(self) -> int:
        """How many voices are sounding, releases included."""
