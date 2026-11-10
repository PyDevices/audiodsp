# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Mix several audio sources into one stream.

`Mixer` is an audio source with a fixed number of voices. Each voice plays
one source at that source's own rate, with its own level and panning, and the
mixer adds the voices up into one stream of the format you gave it. Hand the
mixer to an output, then `Mixer.play` sources into its voices. It follows
CircuitPython's `audiomixer`, so code written for a board runs unchanged,
and adds `Mixer.level_ramp`.

Nothing in the module makes a voice on its own: you reach one through
`Mixer.voice`.
"""

from typing import Protocol, Self

class _AudioSample(Protocol):
    @property
    def sample_rate(self) -> int: ...
    @property
    def bits_per_sample(self) -> int: ...
    @property
    def channel_count(self) -> int: ...

class _MixerVoice:
    """One voice of a `Mixer`.

    Get voices from `Mixer.voice`; the module has no name for the type and
    you cannot make one on its own.
    """

    @property
    def playing(self) -> bool:
        """Whether the voice is playing a source. A source that has ended, or
        a voice that was stopped, is not playing."""
    level: float | object
    """How loud the voice is: 0.0 is silent, 1.0 is as recorded. A number, or
    a `synthio.LFO` or `synthio.Math` that moves it. A new voice starts at 1.0.

    With `Mixer.level_ramp` at 0 a change lands at the voice's next zero
    crossing, so it cannot step a waveform mid-cycle. With a ramp set it walks
    to the new level over that many frames."""
    panning: float | object
    """Where the voice sits between the channels of a stereo mixer: -1.0 is
    all left, 1.0 is all right, 0.0 is the centre and the default. A number,
    or a `synthio.LFO` or `synthio.Math`. A mono mixer ignores it. A change
    follows the rules of `level`."""
    loop: bool
    """Whether the voice starts its source again when it ends."""
    def play(self, sample: _AudioSample, /, *, loop: bool = False) -> None:
        """Play a source on this voice, from its start, replacing what the
        voice was playing.

        Args:
            sample: An audio source. Its channel count, bit depth and
                signedness must match the mixer's.
            loop: Start the source again whenever it ends.

        Raises:
            ValueError: The source's format does not match the mixer's.
        """
    def stop(self, *, voice: int = 0) -> None:
        """Stop the voice at once. ``voice`` is accepted and ignored, as in
        CircuitPython."""
    def end(self) -> None:
        """Let the source play to its end and then stop, by turning looping
        off."""

class Mixer:
    """An audio source that mixes several others.

    Every argument is a keyword. The format you give is the format of the
    stream the mixer produces, and every source played into it must have the
    same channel count, bit depth and signedness; it may have any sample rate,
    and plays at its own.

    Args:
        voice_count: How many voices, 1 to 255. The default is 2.
        buffer_size: Bytes in each of the mixer's two block buffers. The
            default is 1024.
        channel_count: 1 or 2.
        bits_per_sample: 8 or 16.
        samples_signed: Whether samples are signed. The default is True.
        sample_rate: The mixer's rate in Hz, 1 or more. The default is 8000.

    Raises:
        ValueError: An argument is out of range.
    """

    def __init__(
        self,
        *,
        voice_count: int = 2,
        buffer_size: int = 1024,
        channel_count: int = 2,
        bits_per_sample: int = 16,
        samples_signed: bool = True,
        sample_rate: int = 8000,
    ) -> None: ...
    @property
    def playing(self) -> bool:
        """Whether any voice is playing."""
    @property
    def voice(self) -> tuple[_MixerVoice, ...]:
        """The voices, as a tuple of ``voice_count`` objects, so
        ``mixer.voice[0].level = 0.5`` sets the first one's level."""
    level_ramp: int
    """Frames a voice's level or pan move takes, 0 to 16777216 (2 ** 24). At
    0, the default, a move behaves as it does in CircuitPython. Above 0, a
    move walks from the old value to the new one in a straight line over that
    many frames, so turning a level in one call does not click. Set it to the
    number of frames in 20 ms (960 at 48 kHz) for the usual crossfade.

    A voice's first level lands at once; the walk starts with the next move.
    A move made during a walk starts a new walk from where the level stands.
    A level driven by a `synthio.LFO` starts a walk each time it is read, so
    it follows the LFO a walk behind."""
    sample_rate: int
    """The mixer's rate in Hz. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: 8 or 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    def play(self, sample: _AudioSample, /, *, voice: int = 0, loop: bool = False) -> Self:
        """Play a source on one voice, from its start.

        Args:
            sample: An audio source in the mixer's format.
            voice: The voice, 0 up to ``voice_count - 1``.
            loop: Start the source again whenever it ends.

        Returns:
            The mixer.

        Raises:
            ValueError: ``voice`` is out of range, or the source's format
                does not match the mixer's.
        """
    def stop_voice(self, voice: int = 0) -> None:
        """Stop one voice at once.

        Raises:
            ValueError: ``voice`` is out of range.
        """
    def deinit(self) -> None:
        """Free the mixer. Later calls other than `deinit` raise
        `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...
