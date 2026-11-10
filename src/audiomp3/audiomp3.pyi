# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Play MP3 files.

`MP3Decoder` reads an MP3 file while it plays and hands out signed 16-bit
audio. It is an audio sample, so it plays wherever a `audiocore.WaveFile`
does. The module has CircuitPython's own `audiomp3` API, so one script runs on
both.

The decoder reads its file during playback, so keep the file open while the
sample plays.
"""

from typing import BinaryIO, Self

class MP3Decoder:
    """Decodes an MP3 file as it plays.

    Args:
        file: A path, or a file object opened for reading in binary mode.
        buffer: A writable buffer to use for the decoder's input and output
            instead of allocating one. Leave it out to let the decoder
            allocate its own.

    Raises:
        TypeError: ``file`` is a file opened in text mode.
        OSError: The file cannot be opened or read.
        RuntimeError: The file has no MP3 frames the decoder can read.
        MemoryError: The decoder's buffers do not fit.
    """

    def __init__(self, file: str | BinaryIO, buffer: bytearray | memoryview | None = None) -> None: ...
    file: BinaryIO
    """The file being decoded. Assign a new file object, opened in binary
    mode, to play another MP3 with the same decoder."""
    sample_rate: int
    """Frames per second, from the file. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: always 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2, from the file."""
    @property
    def rms_level(self) -> float:
        """How loud the most recently decoded block was, in 16-bit sample
        units: the square root of the sum of its squared samples, divided by
        the number of samples. Louder audio reads higher; it is not a true
        RMS, so compare readings with each other rather than with a
        decibel scale."""
    @property
    def samples_decoded(self) -> int:
        """How many frames have been decoded so far."""
    def open(self, path: str, /) -> None:
        """Open an MP3 file by path and decode that instead.

        Args:
            path: The file to open. It is opened for reading in binary mode.

        Raises:
            OSError: The file cannot be opened or read.
        """
    def deinit(self) -> None:
        """Release the decoder and close nothing else. Every later use raises
        `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...
