# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""A delay with a filter, a soft-clip and a cross-feed in its feedback loop.

`FeedbackDelay` is a delay line with the things a tape or bucket-brigade echo
has in its loop: a low-pass that darkens each repeat, a high-pass that thins
it, a soft-clip, a cross-feed that ping-pongs the repeats between channels,
and a slow wobble in the delay time ("wow") applied per sample, so it bends
pitch the way tape does instead of stepping once a block. It is named apart
from `audiodelays` on purpose: that module is CircuitPython's own, and the
additions here would not exist on a stock board.

Four options go further, each off by default. ``wow_shape`` swaps the built-in
modulation sine for one period of your own, because a bucket brigade's delay
is its line length over its clock, so a triangle on the clock is a reciprocal
on the delay, and no sine is that. ``delay_slew`` walks the read head to a new
``delay_ms`` at a constant rate instead of jumping to it, which is both an
Echoplex's varispeed and the reason a delay-time knob can be turned mid-take
without a click. ``wow_am_depth`` puts the same oscillator on the wet gain,
dipping only. ``loop_semitones`` pitch-shifts the line read *inside* the loop,
so every repeat rises again: a shimmer, which chaining
``audiodelays.PitchShift`` after a delay is not. Shifting up, the climb fades
out at the top of the band rather than folding back past Nyquist.

The node is one 256-frame block at a time, 16-bit audio, 1 or 2 channels. A
level moved on a playing node, such as ``mix``, walks to its new value over
20 ms instead of stepping. Latency from dry to wet is the delay itself. When
the source ends the tail rings out on silence until it reaches exactly zero,
and the node rests, so a source played into it afterwards meets an empty
line; `audiomodal.Bank`, `audioverb.Tank` and `audioconvolve.Convolver` do
the same.
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

class FeedbackDelay:
    """A delay with a processed feedback loop.

    Every argument is a keyword. ``sample_rate``, ``max_delay_ms`` and
    ``channel_count`` size the delay line and the audio, so they can only be
    given here; every other option can be changed with `set`. A keyword that
    is not one of these raises `TypeError`.

    Args:
        sample_rate: Frames per second. The default is 48000.
        max_delay_ms: The longest delay in milliseconds, above 0. It sizes the
            line. The default is 250.
        channel_count: 1 or 2. The default is 2.
        delay_ms: The delay in milliseconds, clamped to the line. The default
            is half of ``max_delay_ms``. Give this or ``delay_frames``, not
            both.
        delay_frames: The delay in frames, read exactly that many frames back.
            At 44.1 and 22.05 kHz some whole frames have no ``delay_ms`` that
            lands on them, so a lossless repeat at those rates needs this.
        feedback: How much of each repeat feeds back, 0.0 to 0.99 (clamped).
            The default is 0.4.
        mix: The wet level, 0.0 to 2.0, clamped. The dry stays at unity until
            1.0, as for `audiodelays.Echo`, so 0.3 is the dry plus 30 percent
            wet and 2.0 is the wet alone. The default is 0.3.
        damping_hz: A low-pass in the loop at this corner, in Hz, that darkens
            each repeat. 0 takes the filter out, the default.
        cut_hz: A high-pass in the loop at this corner, in Hz, that thins each
            repeat. 0 takes the filter out, the default.
        wow_hz: The rate of the delay-time wobble, in Hz. 0 is none, the
            default.
        wow_depth_ms: How far the wobble moves the delay, 0 to 1000
            milliseconds. A change ramps over ``wow_depth_ramp_ms``. The
            default is 0.
        wow_depth_ramp_ms: How long a change of ``wow_depth_ms`` takes to land,
            0 to 60000 milliseconds. The default is 20.
        wow_shape: One period of the wobble as int16 Q15 samples, a
            power-of-two count from 2 to 4096, or None for the built-in sine.
            The default is None.
        wow_am_depth: How far the wobble also dips the wet gain, 0.0 to 1.0.
            It never boosts. The default is 0.
        cross_feed: How much of each repeat goes to the other channel, 0.0 to
            1.0. The default is 0.
        loop_drive: The soft-clip in the loop, 0.0 to 1.0. The default is 0.
        input_pan: Where the input goes: -1.0 sends both channels into the left
            line, 1.0 into the right, 0.0 leaves each on its own. Hard over
            sends the average of the two channels. With ``cross_feed`` it makes
            a ping-pong. The default is 0.
        delay_slew: How fast the read head walks to a new ``delay_ms``, in
            seconds of delay per second, 0 to 64. 0 jumps, the default.
        loop_semitones: How far each pass through the loop shifts the pitch,
            -24 to 24 semitones, clamped. 0 is off, the default.
        loop_window_ms: The crossfade window of the loop's pitch shifter, 1 to
            250 milliseconds, clamped. The default is 25.

    Raises:
        ValueError: ``max_delay_ms`` is not above 0, ``channel_count`` is not
            1 or 2, or ``wow_shape`` is not a power-of-two count from 2 to
            4096 int16 samples.
        TypeError: An unknown option was given, or both ``delay_ms`` and
            ``delay_frames``.
    """

    def __init__(
        self,
        *,
        sample_rate: int = 48000,
        max_delay_ms: float = 250,
        channel_count: int = 2,
        delay_ms: float = ...,
        delay_frames: float = ...,
        feedback: float = 0.4,
        mix: float = 0.3,
        damping_hz: float = 0.0,
        cut_hz: float = 0.0,
        wow_hz: float = 0.0,
        wow_depth_ms: float = 0.0,
        wow_depth_ramp_ms: float = 20.0,
        wow_shape: bytes | bytearray | memoryview | None = None,
        wow_am_depth: float = 0.0,
        cross_feed: float = 0.0,
        loop_drive: float = 0.0,
        input_pan: float = 0.0,
        delay_slew: float = 0.0,
        loop_semitones: float = 0.0,
        loop_window_ms: float = 25.0,
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
        """Set the stream to delay, replacing the last one."""
    def set(
        self,
        *,
        delay_ms: float = ...,
        delay_frames: float = ...,
        feedback: float = ...,
        mix: float = ...,
        damping_hz: float = ...,
        cut_hz: float = ...,
        wow_hz: float = ...,
        wow_depth_ms: float = ...,
        wow_depth_ramp_ms: float = ...,
        wow_shape: bytes | bytearray | memoryview | None = ...,
        wow_am_depth: float = ...,
        cross_feed: float = ...,
        loop_drive: float = ...,
        input_pan: float = ...,
        delay_slew: float = ...,
        loop_semitones: float = ...,
        loop_window_ms: float = ...,
    ) -> None:
        """Change options while the node plays, with the meanings and ranges
        of the constructor's. All of them change together or not at all, so a
        bad keyword leaves the node as it was.

        Raises:
            ValueError: ``wow_shape`` is not a power-of-two count from 2 to
                4096 int16 samples.
            TypeError: An unknown option was given, or both ``delay_ms`` and
                ``delay_frames``. ``sample_rate``, ``max_delay_ms`` and
                ``channel_count`` are ignored here, not applied.
        """
    def clear(self) -> None:
        """Empty the line and every filter, which stops a ringing tail."""
    def deinit(self) -> None:
        """Release the node. Later calls other than `deinit` raise
        `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...
