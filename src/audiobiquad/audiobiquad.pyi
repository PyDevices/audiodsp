# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Biquad and all-pass filters whose tails reach exact zero.

`Biquad` is one filter section in any of seven modes, and `AllPass` is a
cascade of first-order all-pass stages, which is what a phaser is made of.
Both keep their state in floating point, so a ringing tail decays to exactly
zero instead of sitting at one least-significant bit. `Q` is bounded to
0.05 to 60 and `feedback` to plus or minus 0.99: a pole on the unit circle is a
filter that never stops ringing, which is the one thing this module exists to
avoid.

This module is named apart from CircuitPython's `audiofilters` on purpose. It
is not an extension of `audiofilters.Filter` or `audiofilters.Phaser` but a
second set of kernels, so the ported modules stay the same as the ones on a
stock board. For a task-shaped introduction, see "Filter a sound".

Both nodes hand out 256 frames at a time (or fewer, with ``buffer_size``), sit
in an audio chain like any other effect, add no latency, and never report
themselves finished: a chain with nothing left to read gets silence.

A level moved on a playing node walks to its new value in a straight line
over 20 ms instead of stepping, so turning ``mix`` in one call does not click.
A node that has not played yet, or has just been reset, takes its levels at
once.
"""

from typing import Protocol, Self, Union

__version__: str
"""The audiodsp release this module came from."""
__revision__: str
"""Which source this build was made from, from `git describe`: a release tag,
or the nearest one and the commit. "unknown" when the build had no git."""

LOW_PASS: int
"""A `Biquad` mode: pass what is below ``frequency``. This is 0."""
HIGH_PASS: int
"""A `Biquad` mode: pass what is above ``frequency``. This is 1."""
BAND_PASS: int
"""A `Biquad` mode: pass a band around ``frequency``. This is 2."""
NOTCH: int
"""A `Biquad` mode: remove a band around ``frequency``. This is 3."""
PEAKING_EQ: int
"""A `Biquad` mode: boost or cut a band around ``frequency`` by ``gain_db``.
This is 4."""
LOW_SHELF: int
"""A `Biquad` mode: boost or cut everything below ``frequency`` by
``gain_db``. This is 5."""
HIGH_SHELF: int
"""A `Biquad` mode: boost or cut everything above ``frequency`` by
``gain_db``. This is 6."""
MODES: tuple[int, int, int, int, int, int, int]
"""The seven mode numbers, `LOW_PASS` to `HIGH_SHELF`, in order. They are
numbered exactly as ``synthio.FilterMode`` numbers its modes, so either can be
handed to either."""
FRAMES: int
"""Frames in one block when ``buffer_size`` is not given: 256."""
MAX_STAGES: int
"""The most stages an `AllPass` can have: 16."""

# A number, or a synthio.LFO or synthio.Math read once a block.
_BlockInput = Union[float, object, None]

class _AudioSample(Protocol):
    @property
    def sample_rate(self) -> int: ...
    @property
    def bits_per_sample(self) -> int: ...
    @property
    def channel_count(self) -> int: ...

class Biquad:
    """One filter section.

    Every argument is a keyword. ``frequency``, ``Q``, ``gain_db`` and ``mix``
    are block inputs: a number, or a ``synthio.LFO`` or ``synthio.Math`` that
    is read once a block, so an LFO on ``frequency`` arrives about a block
    late.

    When ``frequency``, ``Q``, ``gain_db`` or ``mode`` changes while the
    filter plays, it glides to the new filter over about 20 ms instead of
    jumping, so a preset change that drops the corner from 20 kHz to 20 Hz does
    not thump. A filter that has not played yet takes its new settings at once.

    Args:
        mode: One of `LOW_PASS`, `HIGH_PASS`, `BAND_PASS`, `NOTCH`,
            `PEAKING_EQ`, `LOW_SHELF` and `HIGH_SHELF`. The default is
            `LOW_PASS`.
        frequency: The corner or centre in Hz. The default is 1000.
        Q: The resonance, bounded to 0.05 to 60. The default, None, is 0.7071
            (Butterworth), and reads back as that number.
        gain_db: The boost or cut in dB, for `PEAKING_EQ`, `LOW_SHELF` and
            `HIGH_SHELF`. The default is 0.
        mix: A crossfade: 0 is a wire, 1 is the filtered signal alone. The
            default is 1.
        sample_rate: Frames per second. The default is 48000.
        channel_count: 1 or 2. The default is 2.
        buffer_size: Bytes in each block, a whole number of frames from one to
            256. The default, None, is 256 frames.

    Raises:
        ValueError: ``mode`` is not one of the seven, ``channel_count`` is not
            1 or 2, ``sample_rate`` is below 1, or ``buffer_size`` is not whole
            frames from one to 256.
    """

    def __init__(
        self,
        *,
        mode: int = 0,
        frequency: _BlockInput = 1000,
        Q: _BlockInput = None,
        gain_db: _BlockInput = 0,
        mix: _BlockInput = 1,
        sample_rate: int = 48000,
        channel_count: int = 2,
        buffer_size: int | None = None,
    ) -> None: ...
    mode: int
    """The filter's mode. Setting it raises `ValueError` for a number that is
    not one of the seven."""
    frequency: _BlockInput
    """The corner or centre in Hz."""
    Q: _BlockInput
    """The resonance, bounded to 0.05 to 60."""
    gain_db: _BlockInput
    """The boost or cut in dB."""
    mix: _BlockInput
    """The crossfade from the dry signal (0) to the filtered one (1)."""
    sample_rate: int
    """Frames per second. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: always 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    @property
    def playing(self) -> bool:
        """Whether a source is set with `play`."""
    @property
    def coefficients(self) -> tuple[float, float, float, float, float]:
        """The filter's ``(b0, b1, b2, a1, a2)``, normalised, at the settings
        in force. Reading it does not advance an LFO on a block input."""
    def play(self, source: _AudioSample, /) -> None:
        """Set the stream to filter, replacing the last one."""
    def stop(self) -> None:
        """Forget the source. The node then hands out silence."""
    def clear(self) -> None:
        """Empty the filter's memory, which stops a ringing tail."""
    def deinit(self) -> None:
        """Release the node. Later calls other than `deinit` raise
        `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class AllPass:
    """A cascade of first-order all-pass stages.

    ``frequency`` really is the frequency at which one stage's phase passes
    minus 90 degrees, so nothing has to be pre-warped. With ``mix`` at 0.5 the
    cascade sums with the dry signal into the notches a script phaser makes,
    and that is where they are deepest.

    Every argument is a keyword. ``frequency``, ``feedback`` and ``mix`` are
    block inputs, as for `Biquad`. ``stages`` sizes the node's state, so it can
    only be given here.

    Args:
        stages: How many stages, 1 to `MAX_STAGES`. The default is 4.
        frequency: The frequency in Hz at which one stage reaches minus 90
            degrees. The default is 1000.
        feedback: Output fed back to the input, bounded to plus or minus 0.99.
            The default is 0.
        mix: A crossfade: 0 is a wire, 1 is the all-pass cascade alone. The
            default is 1.
        sample_rate: Frames per second. The default is 48000.
        channel_count: 1 or 2. The default is 2.
        buffer_size: Bytes in each block, as for `Biquad`.

    Raises:
        ValueError: ``stages`` is out of range, ``channel_count`` is not 1 or
            2, ``sample_rate`` is below 1, or ``buffer_size`` is not whole
            frames from one to 256.
    """

    def __init__(
        self,
        *,
        stages: int = 4,
        frequency: _BlockInput = 1000,
        feedback: _BlockInput = 0,
        mix: _BlockInput = 1,
        sample_rate: int = 48000,
        channel_count: int = 2,
        buffer_size: int | None = None,
    ) -> None: ...
    frequency: _BlockInput
    """The frequency in Hz at which one stage's phase passes minus 90 degrees."""
    feedback: _BlockInput
    """Output fed back to the input, bounded to plus or minus 0.99."""
    mix: _BlockInput
    """The crossfade from the dry signal (0) to the cascade (1)."""
    sample_rate: int
    """Frames per second. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: always 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    @property
    def playing(self) -> bool:
        """Whether a source is set with `play`."""
    @property
    def stages(self) -> int:
        """How many stages the cascade has."""
    @property
    def coefficient(self) -> float:
        """The coefficient of each stage at the settings in force. Reading it
        does not advance an LFO on a block input."""
    def play(self, source: _AudioSample, /) -> None:
        """Set the stream to filter, replacing the last one."""
    def stop(self) -> None:
        """Forget the source. The node then hands out silence."""
    def clear(self) -> None:
        """Empty the filter's memory, which stops a ringing tail."""
    def deinit(self) -> None:
        """Release the node. Later calls other than `deinit` raise
        `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...
