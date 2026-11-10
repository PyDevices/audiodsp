# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""A bank of resonators, which is what a struck object is.

A drum head, a marimba bar, a bell or a wine glass rings as a sum of decaying
sinusoids at frequencies that are not harmonics of anything. Give `Bank` that
list of modes and a short noise burst for the stick, and it is that object.
"""

from typing import Protocol, Self

__version__: str
"""The audiodsp release this module came from."""
__revision__: str
"""Which source this build was made from, from `git describe`: a release tag,
or the nearest one and the commit. "unknown" when the build had no git."""

FRAMES: int
"""Frames in one block: 256."""
MAX_MODES: int
"""The most modes a bank can have: 64."""
MIN_DECAY_MS: int
"""The shortest decay a mode takes, in milliseconds: 1."""
MAX_DECAY_MS: int
"""The longest decay a mode takes, in milliseconds: 30000."""

class _AudioSample(Protocol):
    @property
    def sample_rate(self) -> int: ...
    @property
    def bits_per_sample(self) -> int: ...
    @property
    def channel_count(self) -> int: ...

class Bank:
    """A bank of resonators, played like any audio sample.

    The bank is fed a stimulus with `play`, usually a short noise burst, and
    rings at the modes you set. The tail keeps ringing after the stimulus
    stops, because a drum whose decay ended the instant the stick left would
    be the one thing this node exists not to be.

    A new bank has no modes set, so with the default ``mix`` of 1.0 it is
    silent, not a passthrough. Use ``mix=0.0`` for the untouched signal.

    Every argument is a keyword. ``modes``, ``sample_rate`` and
    ``channel_count`` fix the allocation, so they can only be given here.

    Args:
        modes: How many resonators, 1 to `MAX_MODES`. The default is 8.
        sample_rate: Frames per second.
        channel_count: 1 or 2.
        mix: How much of the ringing replaces the dry signal, 0.0 to 1.0,
            clamped. The default is 1.0.
        gain: Output gain, 0.0 to 16.0, clamped. The default is 1.0.

    Raises:
        ValueError: ``modes`` is out of range or ``channel_count`` is not
            1 or 2.
        TypeError: An unknown keyword was given.
    """

    def __init__(
        self,
        *,
        modes: int = 8,
        sample_rate: int = 48000,
        channel_count: int = 2,
        mix: float = ...,
        gain: float = ...,
    ) -> None: ...
    sample_rate: int
    """Frames per second. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: always 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    @property
    def modes(self) -> int:
        """How many modes the bank holds."""
    @property
    def ringing(self) -> bool:
        """Whether any mode still holds energy. This is exact, not a
        threshold, so a voice allocator can ask whether a drum has stopped."""
    def play(self, source: _AudioSample, /) -> None:
        """Set the stream that excites the modes, replacing the last one."""
    def set_mode(self, index: int, frequency: float, decay: float, gain: float, /) -> None:
        """Place one mode.

        Args:
            index: The mode, 0 up to ``modes - 1``.
            frequency: The mode's frequency in Hz, clamped just below half
                the sample rate.
            decay: The time the mode takes to fall by 60 dB, in seconds, from
                0.001 to 30.0. This is not a Q.
            gain: The peak of the mode's impulse response, from -16.0 to 16.0.
                A modal table read out of a paper goes in as published.

        Raises:
            IndexError: ``index`` is out of range.
        """
    def set_modes(self, table: list[tuple[float, float, float]] | tuple[tuple[float, float, float], ...], /) -> None:
        """Place the whole list of modes in one call.

        Args:
            table: Rows of ``(frequency, decay, gain)``, as for `set_mode`,
                placed from mode 0. A table shorter than the bank silences
                the rest, so a shorter table is a smaller drum and not a
                chord of two.

        Raises:
            ValueError: The table has more rows than the bank has modes, or a
                row is not three values.
        """
    def set(self, *, mix: float = ..., gain: float = ...) -> None:
        """Change options while the bank plays, without stopping anything
        ringing. All of them change together or not at all.

        Args:
            mix: As for the constructor.
            gain: As for the constructor.

        Raises:
            TypeError: An unknown option was given.
        """
    def clear(self) -> None:
        """Stop everything ringing at once."""
    def deinit(self) -> None:
        """Release the bank and let go of its stimulus. Every later use
        raises `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...
