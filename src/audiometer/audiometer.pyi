# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Band levels for a spectrum meter.

`Meter` splits audio into log-spaced frequency bands and reports each band's
level, plus the peak and the RMS, as one byte each in half-dB steps: 0 is
-100 dB or quieter and 200 is a full-scale sine. Feed it audio from Python, or
attach an `audiopump.Tap` and it reads whatever the pump is playing or
recording, in C.
"""

from typing import Self

__version__: str
"""The audiodsp release this module came from."""
__revision__: str
"""Which source this build was made from, from `git describe`: a release tag,
or the nearest one and the commit. "unknown" when the build had no git."""

MAX_BANDS: int
"""The most bands a meter can have: 96."""

class Meter:
    """A spectrum meter.

    Args:
        bands: How many bands, 0 to `MAX_BANDS`. 0 turns the meter off.
        low_hz: The bottom of the lowest band in Hz. The default is 35.
        high_hz: The top of the highest band in Hz, above ``low_hz``. The
            default is 20000.

    Raises:
        ValueError: ``bands`` is out of range, or the band edges are not
            0 < ``low_hz`` < ``high_hz``.
        MemoryError: The meter does not fit.
    """

    def __init__(self, bands: int = 32, *, low_hz: float | None = None, high_hz: float | None = None) -> None: ...
    def configure(self, bands: int, *, low_hz: float | None = None, high_hz: float | None = None) -> None:
        """Change the bands, as for the constructor. ``low_hz`` and ``high_hz``
        go back to 35 and 20000 when left out.

        Raises:
            ValueError: As for the constructor.
        """
    def feed(self, buffer: bytes | bytearray | memoryview, sample_rate: int, channel_count: int = 2, /) -> None:
        """Run audio through the meter.

        Args:
            buffer: Interleaved signed 16-bit samples, a whole number of
                frames.
            sample_rate: The audio's rate in Hz, 1000 to 384000.
            channel_count: Channels in the buffer, 1 to 8.

        Raises:
            ValueError: The rate or channel count is out of range, the buffer
                is not a whole number of frames, the meter was deinitialised,
                or it is attached to a source (`detach` it first).
        """
    def attach(self, tap: object, sample_rate: int, /) -> None:
        """Read an `audiopump.Tap` in C, from where it is now.

        The tap is drained each time `levels` runs, on the reader's thread, so
        the pump pays nothing. The tap's channel count is taken from the tap;
        give the rate it runs at.

        Args:
            tap: An `audiopump.Tap`.
            sample_rate: The tap's rate in Hz, 1000 to 384000.

        Raises:
            TypeError: ``tap`` is not an `audiopump.Tap`.
            ValueError: The rate is out of range.
        """
    def detach(self) -> None:
        """Stop reading the attached tap, if there is one."""
    def levels(self, buffer: bytearray | memoryview | None = None, /) -> tuple[int, bytes | bytearray | memoryview, int, int]:
        """Take the latest reading.

        Args:
            buffer: Optional. Where to write the band levels, so nothing is
                allocated. It is filled with as many bands as fit.

        Returns:
            ``(seq, levels, peak, rms)``. ``seq`` counts analyses, at 60 a
            second of audio, so a reader can tell fresh levels from a stopped
            stream. ``levels`` is ``buffer``, or a new `bytes` holding one
            byte a band, lowest band first. ``peak`` and ``rms`` are bytes on
            the same scale.
        """
    def stats(self) -> dict[str, int | bool | str | None]:
        """Counters.

        Returns:
            A dict with ``enabled``, ``bands``, ``analyses`` (done so far),
            ``feed_us`` (microseconds spent feeding), ``analysis_us``
            (microseconds spent analysing), ``max_analysis_us`` (the longest
            single analysis), ``elapsed_us``, ``lapped`` (times an attached
            tap overwrote audio before it was read) and ``source``
            (``"tap"``, or None when nothing is attached).
        """
    def deinit(self) -> None:
        """Free the meter. Later calls other than `deinit` raise `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

def db_byte(power: float) -> int:
    """The byte a power reads as on the meter's scale.

    Args:
        power: Signal power, where 1.0 is a full-scale sine.
    """
