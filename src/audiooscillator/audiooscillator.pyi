# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""A band-limited oscillator that other audio streams can steer.

`Oscillator` reads its pitch, linear FM, pulse width and hard sync from audio
streams every sample, so one oscillator can sync to another or be
frequency-modulated by it. `synthio` moves its pitch once per block and reads
a wavetable with no band limiting; this oscillator does neither. Steps are
band-limited with PolyBLEP and the triangle's corners with PolyBLAMP.
"""

from typing import Protocol, Self

__version__: str
"""The audiodsp release this module came from."""
__revision__: str
"""Which source this build was made from, from `git describe`: a release tag,
or the nearest one and the commit. "unknown" when the build had no git."""

SAW: int
"""A waveform: a rising sawtooth. Waveforms add: pass ``SAW + PULSE`` to mix
the two from the same phase."""
PULSE: int
"""A waveform: a pulse wave whose width is ``pulse_width``."""
TRIANGLE: int
"""A waveform: a triangle."""
SINE: int
"""A waveform: a sine."""

class _AudioSample(Protocol):
    @property
    def sample_rate(self) -> int: ...
    @property
    def bits_per_sample(self) -> int: ...
    @property
    def channel_count(self) -> int: ...

class Oscillator:
    """A band-limited oscillator, played like any audio sample.

    The output trails the phase by one sample, which lets a sync edge, which
    nothing can see coming, still correct the sample before it. The first
    block holds the waveform's value at phase zero.

    Every argument is a keyword. The inputs are audio samples, mono or
    stereo, read one frame per output frame; a stereo input is read from its
    first channel. An input that is left out or None does nothing.

    Args:
        sample_rate: Frames per second, 1 or more.
        channel_count: 1 or 2.
        buffer_size: Bytes in one block, a whole number of frames, from one
            frame to 256 frames. The default is 256 frames.
        frequency: The pitch in Hz before the ``pitch`` input moves it. The
            default is 440.0. The phase step is limited to between a tenth
            of a sample and just under half the rate.
        waveform: A sum of `SAW`, `PULSE`, `TRIANGLE` and `SINE`. The default
            is `SAW`.
        waveform_right: The right channel's waveform, from the same phase.
            The default is `SAW`. Used only when ``channel_count`` is 2.
        pulse_width: The pulse's width as a fraction of the cycle, 0.05 to
            0.95. The default is 0.5.
        level: The output for a waveform's full swing, where 1.0 is full
            scale. The default is 1.0.
        pitch_octaves: Octaves the ``pitch`` input moves the pitch for a
            full-scale signal. The default is 10.0.
        fm_depth: Hz the ``fm`` input moves the frequency for a full-scale
            signal, linearly. The default is 0.0.
        pwm_depth: Pulse width the ``pwm`` input adds for a full-scale
            signal. The default is 0.5.
        pitch: An audio sample that moves the pitch exponentially.
        fm: An audio sample that frequency-modulates the oscillator.
        pwm: An audio sample that modulates the pulse width.
        sync: An audio sample that resets the phase when it rises through
            zero.

    Raises:
        TypeError: An unknown keyword was given, or an input is not an audio
            sample.
        ValueError: ``sample_rate`` is below 1, ``channel_count`` is not 1 or
            2, ``buffer_size`` is not valid, or an input has more than two
            channels.
    """

    def __init__(
        self,
        *,
        sample_rate: int = 48000,
        channel_count: int = 1,
        buffer_size: int | None = None,
        frequency: float = ...,
        waveform: int = ...,
        waveform_right: int = ...,
        pulse_width: float = ...,
        level: float = ...,
        pitch_octaves: float = ...,
        fm_depth: float = ...,
        pwm_depth: float = ...,
        pitch: _AudioSample | None = None,
        fm: _AudioSample | None = None,
        pwm: _AudioSample | None = None,
        sync: _AudioSample | None = None,
    ) -> None: ...
    sample_rate: int
    """Frames per second. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: always 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    def set(
        self,
        *,
        frequency: float = ...,
        waveform: int = ...,
        waveform_right: int = ...,
        pulse_width: float = ...,
        level: float = ...,
        pitch_octaves: float = ...,
        fm_depth: float = ...,
        pwm_depth: float = ...,
    ) -> None:
        """Change options while the oscillator plays. The options are those
        of the constructor, and all of them change together or not at all.

        Raises:
            TypeError: An unknown option was given.
        """
    def modulate(
        self,
        *,
        pitch: _AudioSample | None = ...,
        fm: _AudioSample | None = ...,
        pwm: _AudioSample | None = ...,
        sync: _AudioSample | None = ...,
    ) -> None:
        """Connect or disconnect input streams while the oscillator plays.
        Inputs you do not name stay as they are; pass None to disconnect one.

        Raises:
            TypeError: An unknown input was named, or one is not an audio
                sample.
            ValueError: An input has more than two channels.
        """
    def clear(self) -> None:
        """Set the phase back to zero."""
    def deinit(self) -> None:
        """Release the oscillator and let go of its inputs. Every later use
        raises `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...
