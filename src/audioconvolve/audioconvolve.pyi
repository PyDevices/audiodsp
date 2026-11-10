# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Apply an impulse response to a stream, by partitioned FFT.

`Convolver` convolves its source with a measured impulse response, such as a
room, a cabinet or a spring, or with a room it synthesizes itself. It adds one
block of latency once an impulse is loaded.
"""

from typing import Protocol, Self

__version__: str
"""The audiodsp release this module came from."""
__revision__: str
"""Which source this build was made from, from `git describe`: a release tag,
or the nearest one and the commit. "unknown" when the build had no git."""

FRAMES: int
"""Frames in one block, and the size of one partition: 256."""
MAX_PARTITIONS: int
"""The most partitions an impulse can take: 512, so impulses up to
``MAX_PARTITIONS * FRAMES`` frames long."""

class _AudioSample(Protocol):
    @property
    def sample_rate(self) -> int: ...
    @property
    def bits_per_sample(self) -> int: ...
    @property
    def channel_count(self) -> int: ...

class Convolver:
    """Convolves a stream with an impulse response, played like any audio
    sample.

    The tail keeps ringing after the source ends, so a room rings out. A
    convolver with nothing loaded passes its input through. The capacity for
    the impulse is fixed when the convolver is made, because something
    downstream may already be pulling from it: size it with ``max_taps``, or
    give an ``impulse`` to size it from.

    Args:
        impulse: Optional. The impulse response, as signed 16-bit samples,
            interleaved when ``impulse_channels`` is 2. If given, it is
            loaded now.
        impulse_channels: Channels in ``impulse``: 1, which is shared by both
            audio channels, or 2, which gives each channel its own.
        sample_rate: Frames per second.
        max_taps: The longest impulse, in frames, that `load` and `synthesize`
            can later bring. The default is the length of ``impulse``, or 4096
            frames when there is none. A longer ``impulse`` raises it.
        ir_channels: Channels the stored impulse has, 1 or 2. The default is
            ``impulse_channels``.
        gain: A linear gain on ``impulse`` as it is loaded. The default is 1.0.
        mix: Wet and dry balance, 0.0 to 1.0, clamped, where the dry stays at
            unity until 0.5 and the wet is already at full level there, as
            in `audiofreeverb`. The default is 0.5.
        channel_count: Audio channels, 1 or 2.
        wet_gain: A linear gain on the wet signal alone, 0.0 to 4.0, clamped.
            The default is 1.0.

    Raises:
        ValueError: A channel count is out of range, ``impulse`` is not a
            whole number of 16-bit frames, or the impulse is too long.
    """

    def __init__(
        self,
        impulse: bytes | bytearray | memoryview | None = None,
        impulse_channels: int = 1,
        sample_rate: int = 48000,
        max_taps: int = 0,
        ir_channels: int = 0,
        gain: float | None = None,
        mix: float | None = None,
        channel_count: int = 2,
        wet_gain: float | None = None,
    ) -> None: ...
    sample_rate: int
    """Frames per second. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: always 16."""
    @property
    def channel_count(self) -> int:
        """Number of audio channels: 1 or 2."""
    @property
    def taps(self) -> int:
        """The length of the loaded impulse in frames, rounded up to a whole
        number of partitions. 0 when nothing is loaded."""
    @property
    def latency(self) -> int:
        """Frames of delay the convolver adds: `FRAMES` once an impulse is
        loaded, 0 before, since an empty convolver passes its input through."""
    @property
    def room_peak_gain(self) -> float:
        """The most the last `synthesize` room brings a low full-scale input
        back, as a linear gain, before ``wet_gain``. A dark room gathers its
        energy low and reads high. 0.0 after `load` or before any
        `synthesize`."""
    def play(self, source: _AudioSample, /) -> None:
        """Set the stream to convolve, replacing the last one.

        Raises:
            ValueError: The stream's channel count differs from this node's.
        """
    def set(self, *, mix: float = ..., wet_gain: float = ...) -> None:
        """Change options while the node plays.

        Args:
            mix: As for the constructor.
            wet_gain: As for the constructor.

        Raises:
            TypeError: An unknown option was given.
        """
    def load(self, impulse: bytes | bytearray | memoryview, channels: int = 1, gain: float | None = None) -> None:
        """Load an impulse response while the node plays.

        Audio in flight is kept: the block being played crossfades from the
        old impulse to the new one, with no frame dropped or repeated. An empty
        impulse unloads it.

        Args:
            impulse: Signed 16-bit samples, interleaved when ``channels`` is 2.
            channels: Channels in ``impulse``, 1 or 2.
            gain: A linear gain applied as it is loaded. The default is 1.0.

        Raises:
            ValueError: ``channels`` is out of range, ``impulse`` is not a
                whole number of frames, or it is longer than ``max_taps``.
        """
    def synthesize(
        self,
        decay: float | None = None,
        damping_hz: float | None = None,
        predelay_ms: float | None = None,
        diffusion_ms: float | None = None,
        seed: int = 1,
    ) -> None:
        """Load a synthesized room: decaying noise, normalised to unit energy.
        The same arguments always give the same room, bit for bit, on every
        interpreter.

        Args:
            decay: The time the room takes to fall by 60 dB, in seconds. The
                default is 2.0.
            damping_hz: Rolls the tail's top off as it decays, in Hz. The
                default is 0.0, which leaves it bright.
            predelay_ms: Silence before the room starts, in milliseconds. The
                default is 0.0.
            diffusion_ms: Fades the tail in over this many milliseconds
                rather than starting it at full amplitude, so the room does
                not sound like a burst of noise. The default is 0.0.
            seed: Chooses the noise. Different seeds are different rooms.
        """
    def room_peak_gain_for(
        self,
        decay: float | None = None,
        damping_hz: float | None = None,
        predelay_ms: float | None = None,
        diffusion_ms: float | None = None,
        seed: int = 1,
    ) -> float:
        """The `room_peak_gain` that `synthesize` would give for the same
        arguments, worked out without loading anything. Set the wet level from
        it before calling `synthesize`, so the audio in flight fades to the
        new room at its new level.
        """
    def clear(self) -> None:
        """Empty the convolver's memory of past audio, so the room stops
        ringing."""
    def deinit(self) -> None:
        """Release the convolver and let go of its source. Every later use
        raises `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...
