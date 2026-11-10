# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Speech features for a wake word model.

`Frontend` turns audio into what a microWakeWord model listens to: every
10 ms, a slice of 40 values, one per mel band from 125 Hz to 7.5 kHz. It is
TensorFlow Lite Micro's audio microfrontend with the settings those models
were trained on. Feed it audio from Python, or attach an `audiopump.Tap` and
it reads the audio in C. Each slice also carries a voice activity flag.

Audio at any rate from 16 kHz to 384 kHz is accepted; other rates are
resampled to 16 kHz first, and stereo or wider audio is mixed to mono.
"""

from typing import Self

__version__: str
"""The audiodsp release this module came from."""
__revision__: str
"""Which source this build was made from, from `git describe`: a release tag,
or the nearest one and the commit. "unknown" when the build had no git."""

FEATURE_SIZE: int
"""Values in one slice: 40."""
SAMPLE_RATE: int
"""The frontend's own rate in Hz: 16000. Audio at this rate is not resampled."""
STEP_MS: int
"""Audio that makes one slice, in milliseconds: 10."""

class Frontend:
    """The feature extractor, with a queue of finished slices.

    Slices wait in the queue until `readinto` takes them. When it is full, the
    oldest are dropped and counted in `stats`.

    Args:
        slices: How many slices the queue holds, 8 to 100000. The default is
            one second of audio.

    Raises:
        ValueError: ``slices`` is out of range.
        MemoryError: The queue does not fit.
    """

    def __init__(self, *, slices: int = 100) -> None: ...
    def feed(self, buffer: bytes | bytearray | memoryview, sample_rate: int, channel_count: int = 1, /) -> int:
        """Run audio through the frontend.

        Args:
            buffer: Interleaved signed 16-bit samples, a whole number of
                frames.
            sample_rate: The audio's rate in Hz, 16000 to 384000.
            channel_count: Channels in the buffer, 1 to 8.

        Returns:
            How many finished slices are waiting.

        Raises:
            ValueError: The rate or channel count is out of range, the buffer
                is not a whole number of frames, the frontend was
                deinitialised, or it is attached to a source (`detach` it
                first).
            MemoryError: The resampler does not fit.
        """
    def attach(self, tap: object, sample_rate: int, /) -> None:
        """Read an `audiopump.Tap` in C, from where it is now.

        The tap is drained each time `readinto` runs, on the reader's thread.
        The tap's channel count is taken from the tap; give the rate it
        runs at.

        Args:
            tap: An `audiopump.Tap`.
            sample_rate: The tap's rate in Hz, 16000 to 384000.

        Raises:
            TypeError: ``tap`` is not an `audiopump.Tap`.
            ValueError: The rate is out of range.
        """
    def detach(self) -> None:
        """Stop reading the attached tap, if there is one."""
    def readinto(self, buffer: bytearray | memoryview, voice: bytearray | memoryview | None = None, /) -> int:
        """Take finished slices as the int8 values a model takes.

        Args:
            buffer: Where to write them. It holds one slice per `FEATURE_SIZE`
                bytes.
            voice: Optional. One byte a slice is written here: 1 when the
                slice holds speech, 0 for silence or steady noise. It limits
                how many slices are taken if it is shorter than ``buffer``
                allows.

        Returns:
            How many whole slices were written.
        """
    def readinto_raw(self, buffer: bytearray | memoryview, voice: bytearray | memoryview | None = None, /) -> int:
        """Take finished slices as the frontend's own unsigned 16-bit values,
        about 0 to 670.

        Args:
            buffer: Where to write them. It holds one slice per
                ``2 * FEATURE_SIZE`` bytes.
            voice: As for `readinto`.

        Returns:
            How many whole slices were written.
        """
    def speaking(self) -> bool:
        """Whether the newest slice holds speech."""
    def reset(self) -> None:
        """Forget all audio and slices, as if just made. An attached tap is
        read from where it is now."""
    def stats(self) -> dict[str, int | bool | str | None]:
        """Counters.

        Returns:
            A dict with ``slices`` (made so far), ``dropped`` (lost to a full
            queue), ``waiting``, ``capacity``, ``rate`` (the input rate in
            Hz), ``resampling`` (whether that rate needs it), ``lapped``
            (times an attached tap overwrote audio before it was read) and
            ``source`` (``"tap"``, or None when nothing is attached).
        """
    def deinit(self) -> None:
        """Free the queue. Later calls other than `deinit` raise `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

def quantize(value: int) -> int:
    """The int8 a model takes for one raw frontend value.

    This is ``(value * 256 + 333) // 666 - 128``, which is what `readinto`
    applies to every value.

    Args:
        value: A raw value, 0 to 65535.

    Raises:
        ValueError: ``value`` is out of range.
    """
