# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""A transistor ladder filter.

`Ladder` is a low-pass filter that behaves like the circuit. `audiofilters.Filter`
is a better *resonant low-pass* than this one: a cascade of biquads tracks
the analytic response to a fraction of a decibel. It is also linear, so it
never sustains a tone of its own, has no harmonics to speak of and sounds the
same however hard you hit it. A ladder does those three things, and that is
what this module is for.

::

    import audioladder

    ladder = audioladder.Ladder(
        sample_rate=48000,      # and channel_count=1 or 2
        cutoff_hz=800.0,        # where the filter turns over
        resonance=3.6,          # 0 to 4.2; at 4 it sustains a tone at the cutoff
        drive=2.5,              # linear gain into the loop, 1 being unity
        poles=4,                # tap stage 1 to 4: 6, 12, 18 or 24 dB an octave
        passband_comp=0.0,      # 0 to 1, how much of the passband to give back
        oversample=2,           # 1 or 2; 2 folds back fewer of its harmonics
        mix=1.0,                # 0 a wire, 1 the filter
        cutoff_octaves=10.0,    # octaves for a full-scale cutoff input
    )
    ladder.play(source)
    ladder.set(cutoff_hz=1200.0)   # any option, mid-stream, once a block
    ladder.modulate(cutoff=lfo)    # the cutoff read from a stream, every sample
    ladder.clear()                 # empty the integrators; stops a sustained tone
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

class Ladder:
    """A four-pole transistor ladder low-pass filter, played like any audio
    sample.

    It hands out 256 frames at a time, or fewer with ``buffer_size``, and
    renders 16-bit audio. Every option is a keyword. ``sample_rate``,
    ``channel_count`` and ``buffer_size`` can only be given here; the rest can
    be given here or to `set`.

    Args:
        sample_rate: Frames per second, 1 or more. The default is 48000.
        channel_count: 1 or 2. The default is 2.
        buffer_size: Bytes in each block, a whole number of frames from one
            frame to 256. None, the default, is 256 frames.
        cutoff_hz: Where the filter turns over, in Hz. The default is 1000.
            It is held between 1 Hz and just under half the sample rate.
        resonance: Feedback around the loop, 0.0 to 4.2, clamped. The default
            is 0. At 4 the loop sustains a tone at the cutoff; a little past
            it is where a ladder is usually played.
        drive: A linear gain into the loop, 0.0 to 64.0, clamped, where 1 is
            unity. The default is 1.
        poles: Which stage the output is tapped from, 1 to 4, which is 6,
            12, 18 or 24 dB an octave above the cutoff. The default is 4.
            The feedback comes from the fourth stage at every tap.
        passband_comp: How much of the passband the feedback takes away to
            give back, 0.0 to 1.0, clamped. At a resonance of 4 the whole
            band below the cutoff is 14 dB down; 1 gives all of it back.
            The default is 0, which leaves the filter as the circuit behaves.
        oversample: 1 or 2. At 2, the default, the filter runs at twice the
            rate, so fewer of the harmonics it makes fold back above Nyquist.
        mix: A crossfade, 0.0 to 1.0, clamped. 0 is a wire and 1 is the
            filter. This is not the 0 to 2 scale of `audiodelays.Echo`. The
            default is 1.
        cutoff_octaves: How many octaves a full-scale `modulate` input moves
            the cutoff, of either sign. The default is 10.
        cutoff: A stream to read the cutoff from, as for `modulate`.

    Raises:
        ValueError: ``sample_rate`` is less than 1, ``channel_count`` is not
            1 or 2, or ``buffer_size`` is not whole frames up to 256.
        TypeError: An unknown option was given.
    """

    def __init__(
        self,
        *,
        sample_rate: int = 48000,
        channel_count: int = 2,
        buffer_size: int | None = None,
        cutoff_hz: float = 1000.0,
        resonance: float = 0.0,
        drive: float = 1.0,
        poles: int = 4,
        passband_comp: float = 0.0,
        oversample: int = 2,
        mix: float = 1.0,
        cutoff_octaves: float = 10.0,
        cutoff: _AudioSample | None = None,
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
        """Set the stream to filter, replacing the last one."""
    def set(
        self,
        *,
        cutoff_hz: float = ...,
        resonance: float = ...,
        drive: float = ...,
        poles: int = ...,
        passband_comp: float = ...,
        oversample: int = ...,
        mix: float = ...,
        cutoff_octaves: float = ...,
    ) -> None:
        """Change options while the filter plays.

        The cutoff moves once a block, which is plenty for an envelope or a
        knob. Swept by an audio-rate signal that way, it is heard as a
        staircase, a buzz at the block rate; use `modulate` for that. All the
        options given change together or not at all.

        Args:
            cutoff_hz: As for the constructor.
            resonance: As for the constructor.
            drive: As for the constructor.
            poles: As for the constructor.
            passband_comp: As for the constructor.
            oversample: As for the constructor.
            mix: As for the constructor.
            cutoff_octaves: As for the constructor.

        Raises:
            TypeError: An unknown option was given.
        """
    def modulate(self, *, cutoff: _AudioSample | None) -> None:
        """Read the cutoff from a stream, every sample.

        For an input at ``v`` of full scale the cutoff is
        ``cutoff_hz * 2 ** (v * cutoff_octaves)``, so filter FM sounds like
        filter FM. Only the first channel of a stereo input is read. An input
        of zero, or none at all, leaves the filter exactly as ``cutoff_hz``
        sets it.

        Args:
            cutoff: The stream, mono or stereo, or None to disconnect it.

        Raises:
            TypeError: An input other than ``cutoff`` was named.
            ValueError: The stream is not mono or stereo.
        """
    def clear(self) -> None:
        """Empty the integrators. This stops a tone the loop is sustaining."""
    def deinit(self) -> None:
        """Release the filter and let go of its source. Every later use
        raises `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...
