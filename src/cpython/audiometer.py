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

On a board the meter can also listen without Python touching a sample:
``attach(tap, sample_rate)`` reads an ``audiopump.Tap`` in C, and
``attach(audiometer.UAC)`` is fed by usbif's sound card in its own pump. Neither
exists on CPython, where there is no pump to attach to, so ``attach()`` raises.
"""

import _audiodsp

#: The audiodsp this was built from (src/cp_compat/audiodsp_build.h).
__version__ = _audiodsp.__version__
__revision__ = _audiodsp.__revision__

MAX_BANDS = _audiodsp.METER_MAX_BANDS
#: attach() this to meter usbif's sound card (MicroPython on a board only).
UAC = 1


def db_byte(power):
    """The level byte a power reads as (1.0 is a full-scale sine)."""
    return _audiodsp.meter_db_byte(power)


class Meter:
    """N log-spaced band levels between ``low_hz`` and ``high_hz``."""

    def __init__(self, bands=32, *, low_hz=35.0, high_hz=20000.0):
        self._state = _audiodsp.MeterState()
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
        self._live().feed(buffer, sample_rate, channel_count)

    def attach(self, source, sample_rate=None):
        self._live()
        raise NotImplementedError("attach() needs a board's pump: on CPython, feed() the samples")

    def detach(self):
        self._live()

    def levels(self, buf=None):
        """(seq, levels, peak, rms). With ``buf``, the levels are copied into it."""
        seq, lv, pk, rms = self._live().read()
        if buf is not None:
            n = min(len(lv), len(buf))
            buf[:n] = lv[:n]
            lv = buf
        return seq, lv, pk, rms

    def stats(self):
        enabled, bands, analyses, feed_us, analysis_us, max_us, elapsed_us = self._live().stats()
        return {"enabled": enabled, "bands": bands, "analyses": analyses, "feed_us": feed_us,
                "analysis_us": analysis_us, "max_analysis_us": max_us, "elapsed_us": elapsed_us,
                "lapped": 0, "source": None}

    def deinit(self):
        self._state = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.deinit()
