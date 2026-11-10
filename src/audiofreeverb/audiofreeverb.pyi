# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""CircuitPython's Freeverb reverb.

`Freeverb` is an audio source that reads another source and adds a room
around it, with eight comb filters and four all-pass filters per channel. Its
line lengths are fixed in the code. To choose your own network, see
`audioverb.Tank`. The class is CircuitPython's own and takes the same
arguments on every port.
"""

from typing import Protocol, Self, Union

_BlockInput = Union[float, object, None]
"""A number, or a `synthio.LFO` or `synthio.Math` read once a block."""
_Biquad = object
"""A `synthio.Biquad`."""

class _AudioSample(Protocol):
    @property
    def sample_rate(self) -> int: ...
    @property
    def bits_per_sample(self) -> int: ...
    @property
    def channel_count(self) -> int: ...

class Freeverb:
    """A reverb.

    Args:
        roomsize: The size of the room, 0 to 1. The default is 0.5. A
            `synthio.BlockInput`.
        damp: How quickly the high frequencies die away, 0 to 1. The default
            is 0.5. A `synthio.BlockInput`.
        pre_filter: A `synthio.Biquad`, or a sequence of them, run on the
            signal before the reverb. None for none.
        post_filter: The same, run after the reverb.
        mix: How much of the reverberated signal goes out, 0 to 1. The default
            is 0.5. A `synthio.BlockInput`.
        buffer_size: The size in bytes of each block the effect hands out.
        sample_rate: The rate of the audio in Hz. It must match the source.
        bits_per_sample: Must be 16.
        samples_signed: Must be True.
        channel_count: 1 or 2. It must match the source.

    Raises:
        ValueError: ``channel_count`` is not 1 or 2, ``sample_rate`` is less
            than 1, ``bits_per_sample`` is not 16 or ``samples_signed`` is
            False.
        MemoryError: The comb and all-pass lines do not fit.
    """

    def __init__(
        self,
        *,
        roomsize: _BlockInput = ...,
        damp: _BlockInput = ...,
        pre_filter: _Biquad | tuple[_Biquad, ...] | list[_Biquad] | None = None,
        post_filter: _Biquad | tuple[_Biquad, ...] | list[_Biquad] | None = None,
        mix: _BlockInput = ...,
        buffer_size: int = 512,
        sample_rate: int = 8000,
        bits_per_sample: int = 16,
        samples_signed: bool = True,
        channel_count: int = 1,
    ) -> None: ...
    def deinit(self) -> None:
        """Free the effect. Later calls other than `deinit` raise
        `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...
    sample_rate: int
    """Frames per second. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    @property
    def playing(self) -> bool:
        """True while a source is playing through the effect."""
    roomsize: _BlockInput
    """The size of the room, 0 to 1."""
    damp: _BlockInput
    """How quickly the high frequencies die away, 0 to 1."""
    pre_filter: _Biquad | tuple[_Biquad, ...] | list[_Biquad] | None
    """The biquad or biquads run before the reverb."""
    post_filter: _Biquad | tuple[_Biquad, ...] | list[_Biquad] | None
    """The biquad or biquads run after the reverb."""
    mix: _BlockInput
    """How much of the reverberated signal goes out, 0 to 1."""
    def play(self, sample: _AudioSample, *, loop: bool = False) -> None:
        """Read from a source and add reverb to it.

        Args:
            sample: The audio source.
            loop: Start the source over when it ends.
        """
    def stop(self) -> None:
        """Stop reading the source."""
