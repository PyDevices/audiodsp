# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Route audio: fan one stream out, scale stereo width, re-point a wire.

`Splitter` reads one source and hands out up to four `SplitterTap` streams
that each play the same audio, so a stream can feed parallel branches.
`MidSide` scales the difference between the left and right channels. `Port`
is a wire whose source can be changed while it plays.

All of them are audio samples (a `Splitter` itself is not; its taps are), so
they play wherever a `audiocore.RawSample` does.
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

class Splitter:
    """Fans one source out to parallel branches.

    The splitter reads the source once and keeps the audio in a ring of 8192
    frames, so the taps may be read at different paces, but a tap that falls
    further behind than the ring holds loses audio. Every tap exists from the
    start, because audio nobody has read yet is dropped.

    Args:
        source: The stream to read, mono or stereo.
        taps: How many taps to make, 1 to 4.

    Raises:
        ValueError: ``taps`` is out of range, or the source has more than two
            channels.
    """

    def __init__(self, source: _AudioSample, taps: int = 2) -> None: ...
    def tap(self, index: int, /) -> SplitterTap:
        """Return one of the taps.

        Raises:
            ValueError: ``index`` is not below the number of taps, or the
                splitter was deinitialised.
        """
    def clear(self) -> None:
        """Drop the audio that is buffered and unread, so every tap starts on
        the source's next frames. Use it after the source was read some other
        way, so the taps do not come back on stale audio."""
    def deinit(self) -> None:
        """Release the splitter and its taps, and let go of the source. Every
        later use raises `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class SplitterTap:
    """One branch of a `Splitter`: the same audio, as an audio sample.

    Make taps with `Splitter.tap`; they take no arguments of their own. The
    format is the source's, as signed 16-bit audio.
    """

    sample_rate: int
    """Frames per second, from the source."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: always 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    def deinit(self) -> None:
        """Release this tap. Every later use raises `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class MidSide:
    """Scales the difference between the left and right channels.

    At a width of 1.0 the output is the input exactly, so the node can stay in
    a chain that does not use it. A mono node copies its input, since a mono
    signal has no difference to scale.

    Args:
        source: The stream to process, or None to set it later with `play`.
            Its channel count must match this node's.
        width: 0.0 collapses the signal to mono, 1.0 leaves it as it is and
            2.0 doubles the difference. Values outside 0.0 to 2.0 are
            clamped. The default is 1.0.
        sample_rate: Frames per second.
        channel_count: 1 or 2.

    Raises:
        ValueError: ``channel_count`` is not 1 or 2, or the source's channel
            count differs.
    """

    def __init__(
        self,
        source: _AudioSample | None = None,
        width: float | None = None,
        sample_rate: int = 48000,
        channel_count: int = 2,
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
        """Replace the stream being processed.

        Raises:
            ValueError: The stream's channel count differs from this node's.
        """
    def set(self, *, width: float = ...) -> None:
        """Change an option while the node plays.

        Args:
            width: As for the constructor.

        Raises:
            TypeError: An option other than ``width`` was given.
        """
    def deinit(self) -> None:
        """Release the node and let go of its source. Every later use raises
        `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class Port:
    """A wire whose source can be changed while it plays.

    A port hands back its source's audio untouched and without copying it, and
    takes its format from the source it is made with. Hand a port to a player
    once, then `play` a different source into it to change what is heard,
    without touching the player.

    Args:
        source: The stream to pass through. It cannot be None.

    Raises:
        TypeError: ``source`` is not an audio sample.
        ValueError: ``source`` is None or was deinitialised.
    """

    def __init__(self, source: _AudioSample) -> None: ...
    sample_rate: int
    """Frames per second. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits, from the source."""
    @property
    def channel_count(self) -> int:
        """Number of channels, from the source."""
    @property
    def source(self) -> _AudioSample:
        """The stream the port is passing through now."""
    def play(self, source: _AudioSample, /, *, after: int = 0) -> None:
        """Point the port at a different source. It takes effect between
        blocks, and the player does not need to stop.

        Args:
            source: The stream to pass through from now on.
            after: Frames to keep playing the current source first. The port
                switches at the start of the first pull after that many
                frames have played, which gives a level the old source is
                walking time to land. Until then `source` is still the old
                one, and another `play` replaces the switch that was waiting.
                A port nothing has pulled since it was pointed at what it
                plays switches at once, and so does ``after=0``.

        Raises:
            TypeError: ``source`` is not an audio sample.
            ValueError: ``source`` is None, ``after`` is negative, or the
                source's sample rate, channel count, bit depth or signedness
                differs from the port's.
        """
    def deinit(self) -> None:
        """Let go of the source and release the port; the source itself is
        not released. Every later use raises `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...
