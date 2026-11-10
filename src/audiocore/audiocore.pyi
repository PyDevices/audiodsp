# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Audio sample sources: raw PCM in memory, WAV files, and the calls that read
a source's buffers directly.

`RawSample` plays a buffer you already hold and `WaveFile` streams a `.wav`
file. Both are audio samples, the objects every other audiodsp module reads
from and writes to: each has `sample_rate`, `bits_per_sample` and
`channel_count`, and hands its audio to whatever plays it. The module has
CircuitPython's own `audiocore` API, so one script runs on both.

`get_buffer`, `reset_buffer` and `get_structure` pull audio out of any audio
sample by hand, with no output device, which is how a test reads what a chain
of nodes produces.
"""

from typing import Self

__version__: str
"""The audiodsp release this module came from."""
__revision__: str
"""Which source this build was made from, from `git describe`: a release tag,
or the nearest one and the commit. "unknown" when the build had no git."""

class RawSample:
    """Plays a buffer of raw PCM samples that you provide.

    The buffer is read in place and never copied, so keep it unchanged while
    the sample plays. Samples are 8 or 16 bits, signed or unsigned, taken
    from the buffer's type.

    Args:
        buffer: A `bytearray` or an `array.array` of type ``'b'``, ``'B'``,
            ``'h'`` or ``'H'``. A ``'b'`` or ``'h'`` array holds signed
            samples; the others hold unsigned samples.
        channel_count: 1 for mono, 2 for interleaved stereo.
        sample_rate: Frames per second.
        single_buffer: True to play the buffer once through as one block.
            False splits it into two halves handed out alternately, so the
            buffer's length must be a multiple of twice the size of one frame.

    Raises:
        ValueError: The buffer is not one of the types above, or
            ``single_buffer`` is False and its length is not a multiple of
            ``channel_count`` times the sample size times two.
    """

    def __init__(
        self,
        buffer: bytes | bytearray | memoryview,
        *,
        channel_count: int = 1,
        sample_rate: int = 8000,
        single_buffer: bool = True,
    ) -> None: ...
    sample_rate: int
    """Frames per second. You can change it while the sample plays, to 1 or
    more, to play it faster or slower."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: 8 or 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    def deinit(self) -> None:
        """Release the sample. Every later use raises `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class WaveFile:
    """Streams audio from a ``.wav`` file.

    The file stays open and is read a block at a time while the sample
    plays. Uncompressed PCM of 16 bits or fewer, mono or stereo, is
    supported: 8-bit samples are unsigned and 16-bit samples are signed.

    Args:
        file: A path, or a binary file object opened for reading.
        buffer: A writable buffer, 8 to 1024 bytes and a multiple of 8, to
            read blocks into. Leave it out to let the sample allocate its
            own.

    Raises:
        ValueError: The file is not a supported WAV file, or the buffer is
            the wrong size.
        OSError: The file cannot be opened or read.
    """

    def __init__(self, file: str | object, buffer: bytearray | memoryview | None = None) -> None: ...
    sample_rate: int
    """Frames per second. You can change it while the sample plays, to 1 or
    more, to play it faster or slower."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: 8 or 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    def deinit(self) -> None:
        """Release the sample and close its file. Every later use raises
        `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...


def get_buffer(sample: RawSample | WaveFile | object) -> tuple[int, memoryview | None]:
    """Read the next block of audio from a sample.

    Args:
        sample: Any audio sample: a `RawSample`, a `WaveFile`, or a node from
            another audiodsp module.

    Returns:
        ``(status, block)``. ``status`` is 0 when this was the last block, 1
        when more follow and 2 on an error; ``block`` is a copy of the audio
        as a byte `memoryview`, and None when the status is 2. Its length is
        in bytes, whatever the sample width.

    Raises:
        TypeError: ``sample`` is not an audio sample.
        ValueError: The sample was deinitialised.
    """

def reset_buffer(sample: RawSample | WaveFile | object) -> None:
    """Rewind a sample so the next `get_buffer` starts from its beginning.

    Raises:
        TypeError: ``sample`` is not an audio sample.
        ValueError: The sample was deinitialised.
    """

def get_structure(sample: RawSample | WaveFile | object) -> tuple[int, int, int, int]:
    """Describe how a sample hands out its audio.

    Returns:
        ``(single_buffer, samples_signed, max_buffer_length, spacing)``:
        whether one block holds the whole sample, whether samples are signed,
        the most bytes one block can hold, and the bytes between one
        channel's samples.

    Raises:
        TypeError: ``sample`` is not an audio sample.
        ValueError: The sample was deinitialised.
    """
