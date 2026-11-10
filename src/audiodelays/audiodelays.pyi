# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Delay-based effects: echo, chorus, flanger, pitch shifters and a multi-tap delay.

These are CircuitPython's `audiodelays` effects, with the same names and
arguments, so code written for a CircuitPython board runs unchanged. Each one
is an audio source that reads another audio source: give it a source with
`play`, hand the effect to an output or a mixer, and it processes the audio
a block at a time.

The effects take a ``buffer_size`` in bytes for each block, the sample format
(``sample_rate``, ``bits_per_sample``, ``samples_signed``, ``channel_count``)
the source has to match, and then their own options. Every argument is a
keyword. Options that can change while the effect plays are block inputs: a
number, or a ``synthio.LFO`` or ``synthio.Math`` read once a block. A level
moved on a playing node walks to its new value over 20 ms instead of
stepping, so turning a knob does not click.

`audioecho.FeedbackDelay` is a separate delay with a filter, a soft-clip and a
cross-feed in its feedback loop; reach for it when `Echo` is not enough.
"""

from typing import Iterable, Protocol, Self, Union

# A number, or a synthio.LFO or synthio.Math read once a block.
_BlockInput = Union[float, object, None]

class _AudioSample(Protocol):
    @property
    def sample_rate(self) -> int: ...
    @property
    def bits_per_sample(self) -> int: ...
    @property
    def channel_count(self) -> int: ...

class Echo:
    """A single-tap echo with feedback.

    The delay line repeats the source, each repeat ``decay`` times as loud as the
    one before. ``filter`` is applied to the signal inside the feedback loop,
    so each repeat is filtered again. With ``freq_shift`` on, a change to
    ``delay_ms`` while playing shifts the pitch of the repeats, as moving a tape
    head does; with it off the repeats do not shift.

    Args:
        max_delay_ms: The longest delay the effect can be set to, 1 to 4000.
            It sizes the delay line, so it can only be given here. The default
            is 500.
        delay_ms: The delay in milliseconds, up to ``max_delay_ms``. The
            default is 250.
        decay: How much of each repeat feeds back, 0.0 to 1.0. The default is
            0.7.
        filter: A filter, or a sequence of them, applied inside the feedback
            loop, or None for none.
        mix: The balance of the effect, 0.0 for the dry signal only, 1.0 for
            the echo only. The default is 0.25.
        freq_shift: Whether a change to ``delay_ms`` shifts the pitch of the
            repeats. The default is True.
        buffer_size: How many bytes the effect hands out in each block. The
            default is 512.
        sample_rate: Frames per second, 1 or more. The default is 8000.
        bits_per_sample: 8 or 16. The default is 16.
        samples_signed: Whether samples are signed. The default is True.
        channel_count: 1 or 2. The default is 1.

    Raises:
        ValueError: ``max_delay_ms`` is not 1 to 4000.
        ValueError: ``channel_count`` is not 1 or 2, ``sample_rate`` is below
            1, or ``bits_per_sample`` is not 8 or 16.
    """

    def __init__(
        self,
        *,
        max_delay_ms: int = 500,
        delay_ms: _BlockInput = ...,
        decay: _BlockInput = ...,
        filter: object | None = None,
        mix: _BlockInput = ...,
        buffer_size: int = 512,
        sample_rate: int = 8000,
        bits_per_sample: int = 16,
        samples_signed: bool = True,
        channel_count: int = 1,
        freq_shift: bool = True,
    ) -> None: ...
    delay_ms: _BlockInput
    """The delay in milliseconds."""
    decay: _BlockInput
    """How much of each repeat feeds back, 0.0 to 1.0."""
    filter: object | None
    """The filter, or sequence of filters, in the feedback loop."""
    mix: _BlockInput
    """The balance from the dry signal (0.0) to the echo (1.0)."""
    freq_shift: bool
    """Whether a change to ``delay_ms`` shifts the repeats' pitch."""
    sample_rate: int
    """Frames per second. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: 8 or 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    @property
    def playing(self) -> bool:
        """Whether a source is playing."""
    def play(self, sample: _AudioSample, *, loop: bool = False) -> Self:
        """Start reading a source.

        Args:
            sample: The audio to process. Its sample format has to match the
                effect's.
            loop: Whether to start the source again when it ends.

        Returns:
            The effect itself.
        """
    def stop(self) -> None:
        """Stop reading the source. The tail is not played out."""
    def deinit(self) -> None:
        """Release the effect. Later calls other than `deinit` raise
        `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class Chorus:
    """A chorus: the source mixed with slightly detuned, delayed copies of itself.

    ``voices`` sets how many copies, and may be fractional, so a chorus can
    fade from one voice to two.

    Args:
        max_delay_ms: The longest delay the effect can be set to, 1 to 4000.
            It sizes the delay line, so it can only be given here. The default
            is 50.
        delay_ms: The centre delay of the copies in milliseconds. The default
            is 50.
        voices: How many copies are mixed in. The default is 1.0.
        mix: The balance from the dry signal (0.0) to the copies only (1.0).
            The default is 0.5.
        buffer_size: How many bytes the effect hands out in each block. The
            default is 512.
        sample_rate: Frames per second, 1 or more. The default is 8000.
        bits_per_sample: 8 or 16. The default is 16.
        samples_signed: Whether samples are signed. The default is True.
        channel_count: 1 or 2. The default is 1.

    Raises:
        ValueError: ``max_delay_ms`` is not 1 to 4000.
        ValueError: ``channel_count`` is not 1 or 2, ``sample_rate`` is below
            1, or ``bits_per_sample`` is not 8 or 16.
    """

    def __init__(
        self,
        *,
        max_delay_ms: int = 50,
        delay_ms: _BlockInput = ...,
        voices: _BlockInput = ...,
        mix: _BlockInput = ...,
        buffer_size: int = 512,
        sample_rate: int = 8000,
        bits_per_sample: int = 16,
        samples_signed: bool = True,
        channel_count: int = 1,
    ) -> None: ...
    delay_ms: _BlockInput
    """The centre delay of the copies in milliseconds."""
    voices: _BlockInput
    """How many copies are mixed in."""
    mix: _BlockInput
    """The balance from the dry signal (0.0) to the copies (1.0)."""
    sample_rate: int
    """Frames per second. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: 8 or 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    @property
    def playing(self) -> bool:
        """Whether a source is playing."""
    def play(self, sample: _AudioSample, *, loop: bool = False) -> Self:
        """Start reading a source.

        Args:
            sample: The audio to process. Its sample format has to match the
                effect's.
            loop: Whether to start the source again when it ends.

        Returns:
            The effect itself.
        """
    def stop(self) -> None:
        """Stop reading the source. The tail is not played out."""
    def deinit(self) -> None:
        """Release the effect. Later calls other than `deinit` raise
        `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class Flanger:
    """A flanger: a short, swept delay mixed with the source, with feedback.

    A low-frequency oscillator sweeps the delay from ``min_delay_ms`` up
    towards ``max_delay_ms``, by as much of the span as ``depth`` says.

    Args:
        max_delay_ms: The longest delay the sweep can reach, 1 to 100. It
            sizes the delay line, so it can only be given here. The default
            is 10.
        min_delay_ms: The shortest delay of the sweep in milliseconds. The
            default is 1.0.
        rate: How fast the sweep runs, in Hz. The default is 0.5.
        depth: How much of the span between the shortest and longest delay the
            sweep covers. The default is 0.5.
        feedback: How much of the delayed signal feeds back. The default is
            0.5.
        mix: The balance from the dry signal (0.0) to the delayed one (1.0).
            The default is 0.5.
        invert: Whether the delayed signal is inverted, which moves the notches
            from the harmonics to between them. The default is False.
        buffer_size: How many bytes the effect hands out in each block. The
            default is 512.
        sample_rate: Frames per second, 1 or more. The default is 8000.
        bits_per_sample: 8 or 16. The default is 16.
        samples_signed: Whether samples are signed. The default is True.
        channel_count: 1 or 2. The default is 1.

    Raises:
        ValueError: ``max_delay_ms`` is not 1 to 100.
        ValueError: ``channel_count`` is not 1 or 2, ``sample_rate`` is below
            1, or ``bits_per_sample`` is not 8 or 16.
    """

    def __init__(
        self,
        *,
        max_delay_ms: int = 10,
        min_delay_ms: _BlockInput = ...,
        rate: _BlockInput = ...,
        depth: _BlockInput = ...,
        feedback: _BlockInput = ...,
        mix: _BlockInput = ...,
        invert: bool = False,
        buffer_size: int = 512,
        sample_rate: int = 8000,
        bits_per_sample: int = 16,
        samples_signed: bool = True,
        channel_count: int = 1,
    ) -> None: ...
    min_delay_ms: _BlockInput
    """The shortest delay of the sweep in milliseconds."""
    rate: _BlockInput
    """How fast the sweep runs, in Hz."""
    depth: _BlockInput
    """How much of the delay span the sweep covers."""
    feedback: _BlockInput
    """How much of the delayed signal feeds back."""
    mix: _BlockInput
    """The balance from the dry signal (0.0) to the delayed one (1.0)."""
    invert: bool
    """Whether the delayed signal is inverted."""
    sample_rate: int
    """Frames per second. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: 8 or 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    @property
    def lfo_value(self) -> float:
        """Where the sweep is now, as the oscillator's triangle from 0.0 to 1.0."""
    @property
    def playing(self) -> bool:
        """Whether a source is playing."""
    def play(self, sample: _AudioSample, *, loop: bool = False) -> Self:
        """Start reading a source.

        Args:
            sample: The audio to process. Its sample format has to match the
                effect's.
            loop: Whether to start the source again when it ends.

        Returns:
            The effect itself.
        """
    def stop(self) -> None:
        """Stop reading the source. The tail is not played out."""
    def deinit(self) -> None:
        """Release the effect. Later calls other than `deinit` raise
        `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class GranularPitchShift:
    """A pitch shifter that plays overlapping grains of the source.

    Grains are short slices of the recent input, replayed at a different
    speed. It keeps the tempo and shifts the pitch.

    Args:
        semitones: How far to shift the pitch, in semitones. Negative is
            down. The default is 0.
        mix: The balance from the dry signal (0.0) to the shifted one (1.0).
            The default is 1.
        grain_size: How many frames in a grain, 2 or more. The default is
            1024.
        density: How many grains play at once, 1 to 8. The default is 2.
        spread: How much each grain's start is randomised, 0.0 to 1.0. The
            default is 0.
        buffer_size: How many bytes the effect hands out in each block. The
            default is 512.
        sample_rate: Frames per second, 1 or more. The default is 8000.
        bits_per_sample: 8 or 16. The default is 16.
        samples_signed: Whether samples are signed. The default is True.
        channel_count: 1 or 2. The default is 1.

    Raises:
        ValueError: ``grain_size`` is below 2 or ``density`` is not 1 to 8.
        ValueError: ``channel_count`` is not 1 or 2, ``sample_rate`` is below
            1, or ``bits_per_sample`` is not 8 or 16.
    """

    def __init__(
        self,
        *,
        semitones: _BlockInput = 0,
        mix: _BlockInput = 1,
        grain_size: int = 1024,
        density: int = 2,
        spread: _BlockInput = 0,
        buffer_size: int = 512,
        sample_rate: int = 8000,
        bits_per_sample: int = 16,
        samples_signed: bool = True,
        channel_count: int = 1,
    ) -> None: ...
    semitones: _BlockInput
    """How far to shift the pitch, in semitones."""
    mix: _BlockInput
    """The balance from the dry signal (0.0) to the shifted one (1.0)."""
    spread: _BlockInput
    """How much each grain's start is randomised."""
    sample_rate: int
    """Frames per second. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: 8 or 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    @property
    def playing(self) -> bool:
        """Whether a source is playing."""
    def play(self, sample: _AudioSample, *, loop: bool = False) -> Self:
        """Start reading a source.

        Args:
            sample: The audio to process. Its sample format has to match the
                effect's.
            loop: Whether to start the source again when it ends.

        Returns:
            The effect itself.
        """
    def stop(self) -> None:
        """Stop reading the source. The tail is not played out."""
    def deinit(self) -> None:
        """Release the effect. Later calls other than `deinit` raise
        `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class PitchShift:
    """A pitch shifter that reads the source through an overlapping window.

    Where `GranularPitchShift` plays many grains, this plays one window at a
    time, which costs less. ``freeze`` holds the window so a note sustains.

    Args:
        semitones: How far to shift the pitch, in semitones. Negative is
            down. The default is 0.
        mix: The balance from the dry signal (0.0) to the shifted one (1.0).
            The default is 1.
        window: The size of the window in bytes, at least one frame. The
            default is 1024.
        overlap: How many bytes the windows overlap by. The default is 128.
        buffer_size: How many bytes the effect hands out in each block. The
            default is 512.
        sample_rate: Frames per second, 1 or more. The default is 8000.
        bits_per_sample: 8 or 16. The default is 16.
        samples_signed: Whether samples are signed. The default is True.
        channel_count: 1 or 2. The default is 1.

    Raises:
        ValueError: ``window`` is shorter than one frame.
        ValueError: ``channel_count`` is not 1 or 2, ``sample_rate`` is below
            1, or ``bits_per_sample`` is not 8 or 16.
    """

    def __init__(
        self,
        *,
        semitones: _BlockInput = 0,
        mix: _BlockInput = 1,
        window: int = 1024,
        overlap: int = 128,
        buffer_size: int = 512,
        sample_rate: int = 8000,
        bits_per_sample: int = 16,
        samples_signed: bool = True,
        channel_count: int = 1,
    ) -> None: ...
    semitones: _BlockInput
    """How far to shift the pitch, in semitones."""
    mix: _BlockInput
    """The balance from the dry signal (0.0) to the shifted one (1.0)."""
    freeze: bool
    """Whether the window is held, so the sound sustains."""
    sample_rate: int
    """Frames per second. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: 8 or 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    @property
    def playing(self) -> bool:
        """Whether a source is playing."""
    def play(self, sample: _AudioSample, *, loop: bool = False) -> Self:
        """Start reading a source.

        Args:
            sample: The audio to process. Its sample format has to match the
                effect's.
            loop: Whether to start the source again when it ends.

        Returns:
            The effect itself.
        """
    def stop(self) -> None:
        """Stop reading the source. The tail is not played out."""
    def deinit(self) -> None:
        """Release the effect. Later calls other than `deinit` raise
        `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class MultiTapDelay:
    """A delay with several taps, each at its own position and level.

    A tap is read from the delay line at a fraction of ``delay_ms``, and its
    level scales what is heard there.

    Args:
        max_delay_ms: The longest delay the effect can be set to, 1 to 4000.
            It sizes the delay line, so it can only be given here. The default
            is 500.
        delay_ms: The delay in milliseconds, up to ``max_delay_ms``. The
            default is 250.
        decay: How much of the output feeds back, 0.0 to 1.0. The default is
            0.7.
        mix: The balance from the dry signal (0.0) to the taps (1.0). The
            default is 0.25.
        taps: The taps: a sequence of positions, or of ``(position, level)``
            pairs, each from 0.0 to 1.0, where a position is a fraction of
            ``delay_ms``. A bare position has a level of 1. None, the default,
            is no taps.
        buffer_size: How many bytes the effect hands out in each block. The
            default is 512.
        sample_rate: Frames per second, 1 or more. The default is 8000.
        bits_per_sample: 8 or 16. The default is 16.
        samples_signed: Whether samples are signed. The default is True.
        channel_count: 1 or 2. The default is 1.

    Raises:
        ValueError: ``max_delay_ms`` is not 1 to 4000, ``taps`` is empty, or a
            pair is not two values.
        TypeError: ``taps`` is not iterable.
        ValueError: ``channel_count`` is not 1 or 2, ``sample_rate`` is below
            1, or ``bits_per_sample`` is not 8 or 16.
    """

    def __init__(
        self,
        *,
        max_delay_ms: int = 500,
        delay_ms: _BlockInput = 250,
        decay: _BlockInput = ...,
        mix: _BlockInput = ...,
        taps: Iterable[float | tuple[float, float]] | None = None,
        buffer_size: int = 512,
        sample_rate: int = 8000,
        bits_per_sample: int = 16,
        samples_signed: bool = True,
        channel_count: int = 1,
    ) -> None: ...
    delay_ms: _BlockInput
    """The delay in milliseconds."""
    decay: _BlockInput
    """How much of the output feeds back, 0.0 to 1.0."""
    mix: _BlockInput
    """The balance from the dry signal (0.0) to the taps (1.0)."""
    taps: Iterable[float | tuple[float, float]] | None
    """The taps. Reading gives ``(position, level)`` pairs, or None for no taps; any iterable of positions or pairs can be set."""
    sample_rate: int
    """Frames per second. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: 8 or 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    @property
    def playing(self) -> bool:
        """Whether a source is playing."""
    def play(self, sample: _AudioSample, *, loop: bool = False) -> Self:
        """Start reading a source.

        Args:
            sample: The audio to process. Its sample format has to match the
                effect's.
            loop: Whether to start the source again when it ends.

        Returns:
            The effect itself.
        """
    def stop(self) -> None:
        """Stop reading the source. The tail is not played out."""
    def deinit(self) -> None:
        """Release the effect. Later calls other than `deinit` raise
        `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...
