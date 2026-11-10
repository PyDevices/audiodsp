# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""CircuitPython's filter effects: a biquad chain, a phaser and a distortion.

Each class is an audio source that reads another source, changes it and hands
the result on, so it sits in a chain like any other effect:
``mixer.play(effect)`` after ``effect.play(source)``. The module is
CircuitPython's own and takes the same arguments on every port, so a program
written for a board runs here unchanged. The biquad filter of
`audiobiquad`, which carries more options and whose tail reaches exact zero,
is a separate module.

A setting that takes a `synthio.BlockInput` can be a number, or a
`synthio.LFO` or `synthio.Math` that is read once a block.
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

class DistortionMode:
    """The way `Distortion` shapes the signal."""

    CLIP: DistortionMode
    """Hard clipping: anything past full scale is cut off."""
    LOFI: DistortionMode
    """Bit reduction, which makes a gritty low-resolution sound."""
    OVERDRIVE: DistortionMode
    """A rounded saturation that squeezes the peaks."""
    WAVESHAPE: DistortionMode
    """A smooth curve that folds the peaks back."""

class Filter:
    """A chain of biquad filters.

    Args:
        filter: A `synthio.Biquad`, or a sequence of them that run one after
            another, or None for a wire. The same object is returned from
            the `filter` property.
        mix: How much of the filtered signal goes out: 0 is the dry signal
            and 1 is the filtered one. A `synthio.BlockInput`.
        buffer_size: The size in bytes of each block the filter hands out.
        sample_rate: The rate of the audio in Hz. It must match the source.
        bits_per_sample: 8 or 16. It must match the source.
        samples_signed: Whether the samples are signed.
        channel_count: 1 or 2. It must match the source.

    Raises:
        ValueError: ``channel_count`` is not 1 or 2, ``sample_rate`` is less
            than 1, or ``bits_per_sample`` is not 8 or 16.
        TypeError: ``filter`` holds something that is not a `synthio.Biquad`.
    """

    def __init__(
        self,
        *,
        filter: _Biquad | tuple[_Biquad, ...] | list[_Biquad] | None = None,
        mix: _BlockInput = 1.0,
        buffer_size: int = 512,
        sample_rate: int = 8000,
        bits_per_sample: int = 16,
        samples_signed: bool = True,
        channel_count: int = 1,
    ) -> None: ...
    def deinit(self) -> None:
        """Free the filter. Later calls other than `deinit` raise
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
        """True while a source is playing through the filter."""
    filter: _Biquad | tuple[_Biquad, ...] | list[_Biquad] | None
    """The biquad or biquads applied to the signal."""
    mix: _BlockInput
    """How much of the filtered signal goes out, 0 to 1."""
    def play(self, sample: _AudioSample, *, loop: bool = False) -> None:
        """Read from a source and filter it.

        Args:
            sample: The audio source to filter.
            loop: Start the source over when it ends.
        """
    def stop(self) -> None:
        """Stop reading the source."""

class Phaser:
    """A phaser: a cascade of all-pass stages mixed with the dry signal.

    Args:
        frequency: The corner frequency of the all-pass stages in Hz.
            A `synthio.BlockInput`.
        feedback: How much of the output goes back into the input, 0 to 1.
            None leaves it off. A `synthio.BlockInput`.
        mix: How much of the processed signal goes out, 0 to 1. A
            `synthio.BlockInput`.
        stages: How many all-pass stages. More stages make more notches.
        buffer_size: The size in bytes of each block the effect hands out.
        sample_rate: The rate of the audio in Hz. It must match the source.
        bits_per_sample: 8 or 16. It must match the source.
        samples_signed: Whether the samples are signed.
        channel_count: 1 or 2. It must match the source.

    Raises:
        ValueError: ``channel_count`` is not 1 or 2, ``sample_rate`` is less
            than 1, or ``bits_per_sample`` is not 8 or 16.
    """

    def __init__(
        self,
        *,
        frequency: _BlockInput = 1000.0,
        feedback: _BlockInput = None,
        mix: _BlockInput = 1.0,
        stages: int = 6,
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
    frequency: _BlockInput
    """The corner frequency of the all-pass stages in Hz."""
    feedback: _BlockInput
    """How much of the output goes back into the input."""
    mix: _BlockInput
    """How much of the processed signal goes out, 0 to 1."""
    stages: int
    """How many all-pass stages."""
    def play(self, sample: _AudioSample, *, loop: bool = False) -> None:
        """Read from a source and process it.

        Args:
            sample: The audio source.
            loop: Start the source over when it ends.
        """
    def stop(self) -> None:
        """Stop reading the source."""

class Distortion:
    """A distortion with four modes.

    Args:
        drive: How hard the signal is pushed, 0 to 1. A `synthio.BlockInput`.
        pre_gain: Gain in dB before the distortion. A `synthio.BlockInput`.
        post_gain: Gain in dB after the distortion. A `synthio.BlockInput`.
        mode: A `DistortionMode`. The default is `DistortionMode.CLIP`.
        soft_clip: Round off what the mode leaves past full scale instead of
            wrapping it.
        mix: How much of the distorted signal goes out, 0 to 1. A
            `synthio.BlockInput`.
        buffer_size: The size in bytes of each block the effect hands out.
        sample_rate: The rate of the audio in Hz. It must match the source.
        bits_per_sample: 8 or 16. It must match the source.
        samples_signed: Whether the samples are signed.
        channel_count: 1 or 2. It must match the source.

    Raises:
        ValueError: ``channel_count`` is not 1 or 2, ``sample_rate`` is less
            than 1, or ``bits_per_sample`` is not 8 or 16.
    """

    def __init__(
        self,
        *,
        drive: _BlockInput = 0.0,
        pre_gain: _BlockInput = 0.0,
        post_gain: _BlockInput = 0.0,
        mode: DistortionMode = ...,
        soft_clip: bool = False,
        mix: _BlockInput = 1.0,
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
    drive: _BlockInput
    """How hard the signal is pushed, 0 to 1."""
    pre_gain: _BlockInput
    """Gain in dB before the distortion."""
    post_gain: _BlockInput
    """Gain in dB after the distortion."""
    mode: DistortionMode
    """The `DistortionMode` in use."""
    soft_clip: bool
    """Whether the output is softly clipped."""
    mix: _BlockInput
    """How much of the distorted signal goes out, 0 to 1."""
    def play(self, sample: _AudioSample, *, loop: bool = False) -> None:
        """Read from a source and distort it.

        Args:
            sample: The audio source.
            loop: Start the source over when it ends.
        """
    def stop(self) -> None:
        """Stop reading the source."""
