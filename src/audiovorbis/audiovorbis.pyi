# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Play Ogg Vorbis files.

`VorbisDecoder` plays an Ogg Vorbis file the way `audiomp3.MP3Decoder` plays an
MP3, with the same properties and methods. It is an audio sample, so it plays
wherever an `audiocore.WaveFile` does. CircuitPython has no Vorbis module; this
one is audiodsp's own.

The decoder reads its file during playback, so keep the file open while the
sample plays. Decoding is integer-only, using Xiph's Tremor.
"""

from typing import BinaryIO, Self

__version__: str
"""The audiodsp release this module came from."""
__revision__: str
"""Which source this build was made from, from `git describe`: a release tag,
or the nearest one and the commit. "unknown" when the build had no git."""

class VorbisDecoder:
    """Decodes an Ogg Vorbis file as it plays.

    It hands out signed 16-bit audio, mono or stereo. A chained file plays
    through as long as every link has the first one's rate and channel count,
    and ends at the first link that does not.

    Args:
        file: A path, or a file object opened for reading in binary mode.
        buffer: A writable buffer for decoded audio, in place of the
            decoder's own. Each block handed out is half of it, in whole
            stereo frames; a buffer whose halves are under 256 bytes is not
            used. Without one, each block is 1024 frames.

    Raises:
        TypeError: ``file`` is a file opened in text mode.
        OSError: The file cannot be opened or read.
        RuntimeError: The file is not Ogg Vorbis, its headers are damaged or
            it is a version the decoder cannot read.
        ValueError: The file has more than two channels.
    """

    def __init__(self, file: str | BinaryIO, buffer: bytearray | memoryview | None = None) -> None: ...
    file: BinaryIO
    """The file being decoded. Assign a new file object, opened in binary
    mode, to play another with the same decoder."""
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
        """Open an Ogg Vorbis file by path and decode that instead.

        Args:
            path: The file to open. It is opened for reading in binary mode.

        Raises:
            OSError: The file cannot be opened or read.
            RuntimeError: The file is not a Vorbis file the decoder can read.
        """
    def deinit(self) -> None:
        """Release the decoder. Every later use raises `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...
