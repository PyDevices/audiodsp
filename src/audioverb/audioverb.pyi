# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""A plate reverberator whose network you hand in.

`Tank` is Dattorro's plate reverberator, with the network passed in rather
than compiled in. That is the difference from `audiofreeverb.Freeverb`, whose
comb and all-pass lengths are constants.

::

    plate = audioverb.Tank(
        sample_rate=48000, channel_count=2, max_predelay_ms=200,
        decay=0.7, diffusion=0.75, bandwidth_hz=9000, damping_hz=4500,
        mod_rate_hz=1.0, mod_depth_ms=0.27, mix=0.35)
    plate.play(source)
    audio_out.play(plate)
"""

from typing import Protocol, Self, Sequence

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

class Tank:
    """A plate reverberator, played like any audio sample.

    Construction fixes the allocation: ``sample_rate``, ``channel_count`` and
    ``max_predelay_ms``. It also takes the network, ``delays`` and ``taps``.
    `set` moves every other option while the node plays and without emptying
    the lines; giving it ``delays`` or ``taps`` cuts a new network, which
    does empty them.
    Every filter is out of the path at zero, so a ``Tank()`` given nothing
    else is that network with nothing added to it.

    There is no latency from dry to wet. The tail reaches exact zero rather
    than sitting at one least significant bit, because every line write is a
    magnitude truncation, which is the standard cure for the limit cycles of
    a recirculating fixed-point network. When the source ends, the tail rings
    out on silence to that zero and the node rests, so a source played into
    it afterwards meets an empty plate. `audioecho.FeedbackDelay` and
    `audioconvolve.Convolver` do the same.

    ``delays`` is twelve line lengths in frames: the four input diffusers,
    then each tank half's modulated all-pass, delay, all-pass and delay. Each
    is at least 4. ``taps`` is four values per tap: the output channel (0 or
    1), the line (0 to 11), the offset in frames within that line and the
    gain, with at most 32 taps. Both default to Dattorro's published table,
    scaled from 29761 Hz to ``sample_rate``.

    Every argument is a keyword.

    Args:
        sample_rate: Frames per second, 1 or more. The default is 48000.
        channel_count: 1 or 2. The default is 2.
        max_predelay_ms: The longest pre-delay `predelay_ms` can ask for, in
            milliseconds, 0 or more. It sets how much memory the pre-delay
            line takes. The default is 200.
        delays: The twelve line lengths, in frames.
        taps: The output taps, a flat sequence of four numbers a tap.
        decay: How much of the tail comes round again, 0.0 to 0.999,
            clamped. The default is 0.5.
        diffusion: The all-pass coefficient of the diffusers, 0.0 to 0.9,
            clamped. The default is 0.75.
        damping_hz: A low-pass in the tank's loop, in Hz. 0, the default,
            is out of the path.
        bandwidth_hz: A low-pass on the input, in Hz. 0, the default, is out
            of the path.
        low_cut_hz: A high-pass on the input, in Hz. 0, the default, is out
            of the path.
        predelay_ms: Delay before the reverb, in milliseconds, held between
            0 and ``max_predelay_ms``. The default is 0.
        mod_depth_ms: How far the modulated all-passes move, in
            milliseconds, 0 to 100, clamped. The default is 0.
        mod_rate_hz: How fast they move, 0 to 100 Hz, clamped. The default
            is 0.
        drive: A saturation on the input, 0.0 to 1.0, clamped. The default
            is 0.
        width: The stereo width of the wet signal, 0.0 to 2.0, clamped. The
            default is 1.
        tone_db: A tilt of the wet signal, bright to dark, -24 to 24 dB,
            clamped: half the change up at the top and the same down at the
            bottom, pivoting at 1 kHz. A balance control, not a trim. The
            default is 0.
        mix: How much of the reverb is added, 0.0 to 2.0, clamped. It
            follows `audiodelays.Echo`: the dry signal stays at unity until
            1, and from there it fades out. The default is 0.3.

    Raises:
        ValueError: ``sample_rate`` is less than 1, ``channel_count`` is not
            1 or 2, ``max_predelay_ms`` is negative, ``delays`` is not
            twelve lengths of at least 4 frames that fit, or ``taps`` is not
            whole taps naming a channel, a line and an offset that exist.
        TypeError: An unknown option was given.
        MemoryError: The lines do not fit.
    """

    def __init__(
        self,
        *,
        sample_rate: int = 48000,
        channel_count: int = 2,
        max_predelay_ms: float = 200.0,
        delays: Sequence[int] | None = None,
        taps: Sequence[float] | None = None,
        decay: float = 0.5,
        diffusion: float = 0.75,
        damping_hz: float = 0.0,
        bandwidth_hz: float = 0.0,
        low_cut_hz: float = 0.0,
        predelay_ms: float = 0.0,
        mod_depth_ms: float = 0.0,
        mod_rate_hz: float = 0.0,
        drive: float = 0.0,
        width: float = 1.0,
        tone_db: float = 0.0,
        mix: float = 0.3,
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
        """Set the stream to reverberate, replacing the last one."""
    def set(
        self,
        *,
        delays: Sequence[int] = ...,
        taps: Sequence[float] = ...,
        decay: float = ...,
        diffusion: float = ...,
        damping_hz: float = ...,
        bandwidth_hz: float = ...,
        low_cut_hz: float = ...,
        predelay_ms: float = ...,
        mod_depth_ms: float = ...,
        mod_rate_hz: float = ...,
        drive: float = ...,
        width: float = ...,
        tone_db: float = ...,
        mix: float = ...,
    ) -> None:
        """Change options while the tank plays, without emptying the lines.

        Giving ``delays`` or ``taps`` cuts a new network, which does empty
        every line and filter. All the options given change together or not
        at all.

        Args:
            delays: As for the constructor.
            taps: As for the constructor.
            decay: As for the constructor.
            diffusion: As for the constructor.
            damping_hz: As for the constructor.
            bandwidth_hz: As for the constructor.
            low_cut_hz: As for the constructor.
            predelay_ms: As for the constructor.
            mod_depth_ms: As for the constructor.
            mod_rate_hz: As for the constructor.
            drive: As for the constructor.
            width: As for the constructor.
            tone_db: As for the constructor.
            mix: As for the constructor.

        Raises:
            TypeError: An unknown option was given, or one of
                ``sample_rate``, ``channel_count`` or ``max_predelay_ms``,
                which are fixed when the node is made.
            ValueError: ``delays`` or ``taps`` are not valid, as for the
                constructor.
        """
    def clear(self) -> None:
        """Empty every line and every filter."""
    def deinit(self) -> None:
        """Release the node and let go of its source. Every later use
        raises `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...
