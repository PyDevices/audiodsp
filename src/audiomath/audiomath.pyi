# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Arithmetic on audio streams.

`Multiply` multiplies one stream by another, which is ring and amplitude
modulation. `SubOctave` divides a stream down in frequency, as an analog
octave divider does. `remix_s16` changes between mono and stereo, and
`cast_into` turns a block of floats into the 16-bit buffer a `RawSample`
plays, without allocating.

Both nodes are audio samples, so they play wherever a `RawSample` does and
take any audio sample as their source.
"""

from typing import Protocol, Self

__version__: str
"""The audiodsp release this module came from."""
__revision__: str
"""Which source this build was made from, from `git describe`: a release tag,
or the nearest one and the commit. "unknown" when the build had no git."""

class _AudioSample(Protocol):
    @property
    def sample_rate(self) -> int: ...
    @property
    def bits_per_sample(self) -> int: ...
    @property
    def channel_count(self) -> int: ...

class Multiply:
    """Multiplies a stream by a modulator, sample by sample.

    The output is the source blended toward the source times the modulator:
    ``mix`` 1.0 is the full product and 0.0 is the source untouched. With no
    modulator, or after the modulator has run dry, the source passes through
    unchanged rather than going silent. With no source, the node outputs
    silence and never reports itself finished.

    The source and the modulator must have this node's channel count. Output
    is 16-bit signed.

    Args:
        source: The stream to modulate, or None to set it later with `play`.
        modulator: The stream to multiply by, or None to set it later with
            `modulate`. A short looping table such as a sine in a
            `RawSample` is the usual choice.
        mix: Wet amount from 0.0 to 1.0, clamped. The default is 1.0.
        sample_rate: Frames per second.
        channel_count: 1 or 2.

    Raises:
        ValueError: ``channel_count`` is not 1 or 2, or a stream's channel
            count differs from it.
    """

    def __init__(
        self,
        source: _AudioSample | None = None,
        modulator: _AudioSample | None = None,
        mix: float | None = None,
        sample_rate: int = 48000,
        channel_count: int = 2,
    ) -> None: ...
    @property
    def sample_rate(self) -> int:
        """Frames per second."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: always 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    def play(self, source: _AudioSample, /) -> None:
        """Replace the stream being modulated.

        Raises:
            ValueError: The stream's channel count differs from this node's.
        """
    def modulate(self, modulator: _AudioSample, /) -> None:
        """Replace the stream that multiplies the source.

        Raises:
            ValueError: The stream's channel count differs from this node's.
        """
    def set(self, *, mix: float = ...) -> None:
        """Change an option while the node plays.

        A move of ``mix`` on a playing node walks to the new value in a
        straight line over 20 ms instead of stepping, so it doesn't click.
        Before the first pull, or after a reset, it lands at once.

        Args:
            mix: Wet amount from 0.0 to 1.0, clamped.

        Raises:
            TypeError: An option other than ``mix`` was given.
        """
    def deinit(self) -> None:
        """Release the node and let go of its source and modulator. Every
        later use raises `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class SubOctave:
    """An analog-style octave divider.

    A Schmitt trigger watches the source for rising edges and two flip-flops
    halve the count, so the output is a square wave one or two octaves below
    the input's pitch, blended with the dry signal by ``mix``. It works best
    on a single note: filter a chord to its fundamental first.

    Args:
        source: The stream to divide, or None to set it later with `play`.
            Its channel count must match this node's.
        order: 1 for one octave down, 2 for two. Any value of 2 or more
            means 2.
        mix: How much of the divided signal replaces the dry signal, 0.0 to
            1.0, clamped. The default is 1.0.
        threshold: How far from zero the comparator's two thresholds sit, as
            a fraction of full scale, 0.0 to 1.0, clamped. Signal between
            them leaves the comparator where it is. 0.0 is a bare
            zero-crossing detector.
        hold_ms: Milliseconds after an accepted edge during which another is
            ignored, to keep noise from counting twice. Negative values mean 0.
        sample_rate: Frames per second, 1 or more.
        channel_count: 1 or 2.

    Raises:
        ValueError: ``channel_count`` is not 1 or 2, ``sample_rate`` is below
            1, or the source's channel count differs.
    """

    def __init__(
        self,
        source: _AudioSample | None = None,
        order: float | None = None,
        mix: float | None = None,
        threshold: float | None = None,
        hold_ms: float | None = None,
        sample_rate: int = 48000,
        channel_count: int = 2,
    ) -> None: ...
    @property
    def sample_rate(self) -> int:
        """Frames per second."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: always 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    def play(self, source: _AudioSample, /) -> None:
        """Replace the stream being divided.

        Raises:
            ValueError: The stream's channel count differs from this node's.
        """
    def set(
        self,
        *,
        order: float = ...,
        mix: float = ...,
        threshold: float = ...,
        hold_ms: float = ...,
    ) -> None:
        """Change options while the node plays. The count keeps running, so a
        change never flips the output's polarity.

        Args:
            order: As for the constructor.
            mix: As for the constructor.
            threshold: As for the constructor.
            hold_ms: As for the constructor.

        Raises:
            TypeError: An unknown option was given.
        """
    def clear(self) -> None:
        """Reset the comparator and the dividers to where a new node starts."""
    def deinit(self) -> None:
        """Release the node and let go of its source. Every later use raises
        `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

def remix_s16(
    source: bytes | bytearray | memoryview,
    src_channels: int,
    dst_channels: int,
    dest: bytearray | memoryview | None = None,
    /,
) -> bytearray | memoryview:
    """Convert interleaved 16-bit audio between mono and stereo.

    Args:
        source: Signed 16-bit samples, a whole number of frames.
        src_channels: The source's channel count, 1 or 2.
        dst_channels: The channel count wanted, 1 or 2.
        dest: A buffer to write into, at least as long as the result.
            Leave it out to get a new `bytearray`.

    Returns:
        ``dest``, or the new `bytearray` when ``dest`` was left out.

    Raises:
        ValueError: A channel count is not 1 or 2, ``source`` is not a whole
            number of frames, or ``dest`` is too small.
    """

def cast_into(dst: bytearray | memoryview, src: object, scale: float = 32767.0) -> None:
    """Convert a block of floats into 16-bit samples without allocating.

    Each value is multiplied by ``scale``, rounded half away from zero and
    saturated to the 16-bit range.

    Args:
        dst: Where to write: a buffer of signed 16-bit samples, as many as
            ``src`` has floats.
        src: A buffer of 32-bit or 64-bit floats, such as an `array.array`
            of type ``'f'`` or ``'d'``.
        scale: What a value of 1.0 becomes.

    Raises:
        TypeError: ``dst`` is not a 16-bit buffer or ``src`` is not floats.
        ValueError: ``dst`` and ``src`` hold a different number of samples.
    """
