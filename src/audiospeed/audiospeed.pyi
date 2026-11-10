# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""CircuitPython's speed and sample-rate changers.

`SpeedChanger` plays a source faster or slower, which also moves its pitch.
`Resampler` plays a source at the rate its destination asks for. Both read
every frame by position, with no interpolation. The module is CircuitPython's
own and takes the same arguments on every port.
"""

from typing import Protocol, Self

class _AudioSample(Protocol):
    @property
    def sample_rate(self) -> int: ...
    @property
    def bits_per_sample(self) -> int: ...
    @property
    def channel_count(self) -> int: ...

class SpeedChanger:
    """Plays a source at a changed speed.

    The output has the source's rate, channel count and bit depth. At a
    ``rate`` of 2.0 the source plays twice as fast, an octave higher; at 0.5
    it plays half as fast, an octave lower. The node reports done when its
    source does.

    Args:
        source: The audio source to read.
        rate: The speed relative to the source, from 0 to 1000. The default,
            None, is 1.0.

    Raises:
        TypeError: ``source`` is not an audio source.
        ValueError: ``rate`` is outside 0 to 1000.
    """

    def __init__(self, source: _AudioSample, rate: float | None = None) -> None: ...
    rate: float
    """The speed relative to the source, from 0 to 1000. It is held in
    16.16 fixed point, rounded to the nearest step, so what you read back can
    differ from what you wrote in the sixth decimal place."""
    sample_rate: int
    """Frames per second, the source's. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits, the source's."""
    @property
    def channel_count(self) -> int:
        """Number of channels, the source's."""
    def deinit(self) -> None:
        """Release the node and let go of its source. Every later use
        raises `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class Resampler:
    """Plays a source at the sample rate the destination asks for.

    Set `sample_rate` to the destination's rate and the node reads the source
    at the ratio of the source's rate to that. Playing 16 kHz material into
    an 8 kHz destination consumes two source frames for each frame out. Until
    a rate is set the ratio is 1.0.

    Args:
        source: The audio source to read.

    Raises:
        TypeError: ``source`` is not an audio source.
    """

    def __init__(self, source: _AudioSample) -> None: ...
    @property
    def rate(self) -> float:
        """The source frames consumed for each frame out. It is read-only: it
        is whatever the destination's `sample_rate` asked for."""
    sample_rate: int
    """The rate the destination wants, in Hz. Set it to change `rate`."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits, the source's."""
    @property
    def channel_count(self) -> int:
        """Number of channels, the source's."""
    def deinit(self) -> None:
        """Release the node and let go of its source. Every later use
        raises `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...
