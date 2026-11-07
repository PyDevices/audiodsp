# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Band levels for a spectrum meter: N log-spaced bands between two
frequencies, plus peak and RMS, one byte each in half-dB steps.

audiodsp's own, not a CircuitPython module. It came out of usbif's sound-card
pump, where it was the audio meter spike's engine, and the arithmetic is the
same C on every interpreter (``src/shared/audiodsp_meter.c``), so a desktop
and a board read the same bytes for the same samples.

    meter = audiometer.Meter(32, low_hz=35, high_hz=20000)
    meter.feed(pcm, 48000, 2)              # interleaved s16
    seq, levels, peak, rms = meter.levels()

A level byte is ``2 * (dB + 100)``: 0 is -100 dB or quieter, 200 is a
full-scale sine. ``seq`` counts analyses, about 60 a second of audio, so a
reader can tell fresh levels from a stopped stream.

The meter can also listen to the pump: ``attach(tap, sample_rate)`` reads an
``audiopump.Tap``, and each ``levels()`` call first feeds it everything the
pump played since the last one. On a board that reading is in C, so Python
never touches a sample; here it is a copy per call.

A desktop's pump renders in bursts (about 80 ms at a time when the output is
buffered), so here the meter takes in only as much as has played since the
last call. The bars then move with the music at the frame rate, not once a
burst.
"""

import time

import _audiodsp
import audiopump

#: Bytes taken from a tap per feed, as the C meter's chunk.
_CHUNK = 1024

#: The audiodsp this was built from (src/cp_compat/audiodsp_build.h).
__version__ = _audiodsp.__version__
__revision__ = _audiodsp.__revision__

MAX_BANDS = _audiodsp.METER_MAX_BANDS
#: attach() this to meter usbif's sound card (MicroPython on a board only).
def db_byte(power):
    """The level byte a power reads as (1.0 is a full-scale sine)."""
    return _audiodsp.meter_db_byte(power)


class Meter:
    """N log-spaced band levels between ``low_hz`` and ``high_hz``."""

    def __init__(self, bands=32, *, low_hz=35.0, high_hz=20000.0):
        self._state = _audiodsp.MeterState()
        self._tap = None
        self._tap_cursor = 0
        self._tap_rate = 0
        self._tap_channels = 2
        self._lapped = 0
        self._clock = time.monotonic
        self._drained_at = 0.0
        self.configure(bands, low_hz=low_hz, high_hz=high_hz)

    def _live(self):
        if self._state is None:
            raise ValueError("meter is deinitialized")
        return self._state

    def configure(self, bands, *, low_hz=35.0, high_hz=20000.0):
        """Change the bands; 0 turns the meter off. Takes effect at the next feed."""
        self._live().configure(bands, float(low_hz), float(high_hz))

    def feed(self, buffer, sample_rate, channel_count=2):
        """Interleaved s16 frames. Runs an analysis each sixtieth of a second of audio."""
        state = self._live()
        if self._tap is not None:
            raise ValueError("meter is attached to a source; detach() first")
        state.feed(buffer, sample_rate, channel_count)

    def attach(self, source, sample_rate=None):
        """Meter what the pump plays through ``source``, an `audiopump.Tap`.

        Only what the pump plays from now on is metered.

        Raises:
            TypeError: ``source`` isn't a tap, or ``sample_rate`` is missing.
            ValueError: ``sample_rate`` is outside 1 kHz to 384 kHz.
        """
        self._live()
        self._tap = None
        if not isinstance(source, audiopump.Tap):
            raise TypeError("attach() takes an audiopump.Tap")
        if sample_rate is None:
            raise TypeError("a tap needs its sample_rate")
        rate = int(sample_rate)
        if rate < 1000 or rate > 384000:
            raise ValueError("sample_rate 1 kHz..384 kHz")
        self._tap = source
        self._tap_rate = rate
        self._tap_channels = source._frame // 2
        self._tap_cursor = source._w
        self._drained_at = self._clock()

    def detach(self):
        """Stop metering the tap."""
        self._live()
        self._tap = None

    def _drain(self, state):
        tap = self._tap
        now = self._clock()
        frame = tap._frame
        # What has played since the last call, in whole frames. Audio a burst
        # put in the tap ahead of that waits for the next call.
        due = round((now - self._drained_at) * self._tap_rate) * frame
        taken = 0
        while taken < due:
            data, self._tap_cursor, lapped = tap._read_since(self._tap_cursor, min(_CHUNK, due - taken))
            if lapped:
                self._lapped += 1
            if not data:
                # Caught up with the pump: start counting from now.
                self._drained_at = now
                return
            taken += len(data)
            state.feed(data, self._tap_rate, self._tap_channels)
        self._drained_at += taken / frame / self._tap_rate

    def levels(self, buf=None):
        """(seq, levels, peak, rms). With ``buf``, the levels are copied into it."""
        state = self._live()
        if self._tap is not None:
            self._drain(state)
        seq, lv, pk, rms = state.read()
        if buf is not None:
            n = min(len(lv), len(buf))
            buf[:n] = lv[:n]
            lv = buf
        return seq, lv, pk, rms

    def stats(self):
        enabled, bands, analyses, feed_us, analysis_us, max_us, elapsed_us = self._live().stats()
        return {"enabled": enabled, "bands": bands, "analyses": analyses, "feed_us": feed_us,
                "analysis_us": analysis_us, "max_analysis_us": max_us, "elapsed_us": elapsed_us,
                "lapped": self._lapped, "source": "tap" if self._tap is not None else None}

    def deinit(self):
        self._state = None
        self._tap = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.deinit()
