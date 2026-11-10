# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""A waveshaper whose curve is data, and an exact-ratio sample-and-hold.

`Waveshaper` pushes audio through a transfer curve you supply, between a pair
of oversampling stages, because a nonlinearity at the base rate folds the
harmonics it makes above Nyquist straight back onto the signal. `SampleHold`
is the other half of a lo-fi effect: the waveshaper quantises the value, the
hold quantises the time.
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

class Waveshaper:
    """A waveshaper with oversampling, played like any audio sample.

    The shaping happens at ``oversample`` times the sample rate, between a
    matched pair of polyphase all-pass half-bands. It hands out 256 frames at
    a time and renders 16-bit audio. Every option is a keyword. ``curve``,
    ``sample_rate``, ``channel_count`` and ``oversample`` are fixed when the
    node is made, except that `set` can replace the curve. The rest can be
    given here or to `set`.

    ``curve`` is signed 16-bit samples (Q15) of at least two points,
    spanning -1 to +1 of input. Compute it once on a desktop and ship it as
    data; never rebuild it on a board, whose float is single precision where
    the desktop's is double. The node copies it.

    A curve that reaches the rails has a ceiling of its own, because the
    decimator can ring past the rails on a hard edge. Keep
    ``post_gain * max(abs(curve))`` at or below about 0.74 and put the rest
    of the level you want on a mixer voice after this node.

    The ``mix`` and ``post_gain`` of a playing node walk to a new value in a
    straight line over 20 ms instead of stepping, so turning one in a single
    call does not click. A node that has not played yet, or has just been
    cleared, takes them at once.

    Args:
        curve: The transfer curve: a buffer of signed 16-bit values, at
            least two points. Required.
        sample_rate: Frames per second. The default is 48000.
        channel_count: 1 or 2. The default is 2.
        oversample: 1, 2, 4 or 8. The default is 4. The oversampling adds a
            group delay of about 0, 2.2, 3.3 and 3.9 base-rate samples for
            those factors, for a component that has to report its latency.
        pre_gain: The drive knob: a gain into the normalised curve, -1000 to
            1000, clamped. The default is 1.
        bias: Moves the operating point, -1.0 to 1.0, clamped. The default
            is 0.
        post_gain: A gain after the curve, -1000 to 1000, clamped. The
            default is 1.
        mix: A crossfade, 0.0 to 1.0, clamped. 0 is the dry signal and 1 the
            shaped one. The default is 1.
        hysteresis: How much memory the curve has, 0.0 to 1.0, clamped.
            At 0, the default, there is none. Above 0 a slow signal in and
            out traces two paths and encloses an area.
        hysteresis_width: The half-width of that loop as a fraction of full
            scale at the input, 0.0 to 0.5, clamped. The default is 0.02.
        hysteresis_bias: How the width is split between the rising and
            falling branches, -1.0 to 1.0, clamped. The default is 0.

    Raises:
        ValueError: ``curve`` is missing, shorter than two points or not a
            whole number of 16-bit values; ``channel_count`` is not 1 or 2;
            or ``oversample`` is not 1, 2, 4 or 8.
        TypeError: An unknown option was given.
        MemoryError: The curve does not fit.
    """

    def __init__(
        self,
        *,
        curve: bytes | bytearray | memoryview,
        sample_rate: int = 48000,
        channel_count: int = 2,
        oversample: int = 4,
        pre_gain: float = 1.0,
        bias: float = 0.0,
        post_gain: float = 1.0,
        mix: float = 1.0,
        hysteresis: float = 0.0,
        hysteresis_width: float = 0.02,
        hysteresis_bias: float = 0.0,
    ) -> None: ...
    sample_rate: int
    """Frames per second. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: always 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    def play(self, source: _AudioSample, /) -> None:
        """Set the stream to shape, replacing the last one."""
    def set(
        self,
        *,
        curve: bytes | bytearray | memoryview = ...,
        pre_gain: float = ...,
        bias: float = ...,
        post_gain: float = ...,
        mix: float = ...,
        hysteresis: float = ...,
        hysteresis_width: float = ...,
        hysteresis_bias: float = ...,
    ) -> None:
        """Change options while the node plays. All the options given change
        together or not at all. ``sample_rate``, ``channel_count`` and
        ``oversample`` are accepted and ignored, because they are fixed when
        the node is made.

        Args:
            curve: A new transfer curve, as for the constructor.
            pre_gain: As for the constructor.
            bias: As for the constructor.
            post_gain: As for the constructor.
            mix: As for the constructor.
            hysteresis: As for the constructor.
            hysteresis_width: As for the constructor.
            hysteresis_bias: As for the constructor.

        Raises:
            ValueError: ``curve`` is shorter than two points or not a whole
                number of 16-bit values.
            TypeError: An unknown option was given.
        """
    def clear(self) -> None:
        """Empty the oversampling stages and put the hysteresis memory back
        to the start."""
    def recentre(self) -> bool:
        """Settle the node on its operating point after silence.

        Once every channel's input has been exactly zero for a whole block,
        this puts the hysteresis memory where a silent input would hold it,
        and moves the decimator's memories to match, so the output steps to
        the curve's answer at the operating point at once instead of ringing
        down from the old position. Under signal it does nothing.

        Returns:
            True if it moved anything, False under signal.
        """
    def deinit(self) -> None:
        """Release the node and let go of its source. Every later use
        raises `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class SampleHold:
    """A zero-order hold in which ``num`` source frames carry ``den`` new
    values.

    So 26040 Hz at 48 kHz is 400 over 217, and it is exact: one frame in, one
    frame out, at the source's own rate, channels and bit depth, with an
    integer accumulator that cannot drift however long the render runs. When
    ``num`` equals ``den`` the node is a wire, byte for byte. Its `latency` is
    0: what a hold displaces is an event landing on a frame it drops, up to
    ``ceil(num / den) - 1`` frames, which is the effect and for the caller to
    report.

    It reads mono or stereo audio of 8 or 16 bits.

    Args:
        source: The audio to hold.
        num: Source frames in the ratio, 1 or more.
        den: New values in the ratio, 1 or more and no more than ``num``,
            because a hold cannot invent frames. ``num`` can be up to 2**30.

    Raises:
        ValueError: The ratio is not whole numbers with ``den <= num``, or
            the source is not 1 or 2 channels of 8 or 16 bits.
        TypeError: ``source`` is not an audio source.
    """

    def __init__(self, source: _AudioSample, num: int = 1, den: int = 1) -> None: ...
    sample_rate: int
    """Frames per second, the source's to begin with. Writable, to 1 or
    more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits, the source's."""
    @property
    def channel_count(self) -> int:
        """Number of channels, the source's."""
    @property
    def num(self) -> int:
        """Source frames in the ratio."""
    @property
    def den(self) -> int:
        """New values in the ratio."""
    @property
    def latency(self) -> int:
        """Frames of delay the node adds: always 0."""
    def play(self, source: _AudioSample, /) -> None:
        """Hold a new source, and start the hold over.

        Raises:
            ValueError: The source's bit depth or channel count is not the
                one this node was made with.
        """
    def set(self, num: int, den: int) -> None:
        """Move the ratio while the node plays, as for the constructor.

        Raises:
            ValueError: The ratio is not whole numbers with ``den <= num``.
        """
    def clear(self) -> None:
        """Start the hold over."""
    def deinit(self) -> None:
        """Release the node and let go of its source. Every later use
        raises `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...
