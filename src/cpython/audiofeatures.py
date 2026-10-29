"""Speech features for a wake word model, from PCM at any rate.

audiodsp's own, not a CircuitPython module. ``Frontend`` is TensorFlow Lite
Micro's audio microfrontend with the settings microWakeWord's models were
trained on (ESPHome's): a 30 ms window every 10 ms, 40 mel channels from
125 Hz to 7.5 kHz, noise reduction, PCAN gain control and a log scale. The
arithmetic is the same C on every interpreter (``src/shared/audiodsp_features.c``),
so a desktop and a board make the same bytes from the same samples.

    fe = audiofeatures.Frontend()
    fe.feed(pcm, 48000, 2)                 # interleaved s16, any rate from 16 kHz
    buf = bytearray(40 * 10)
    n = fe.readinto(buf)                   # n whole slices of 40 int8

Audio that isn't 16 kHz is resampled to 16 kHz first, through a low-pass that
is flat to 7.5 kHz; 16 kHz audio goes to the frontend untouched. Each 10 ms
makes a slice, which waits in a queue of ``slices`` until it is read. When
the queue is full the oldest slice is dropped, and ``stats()`` counts it.

On a board the frontend can also listen without Python touching a sample:
``attach(tap, sample_rate)`` reads an ``audiopump.Tap`` in C. There is no pump
to attach to on CPython, so ``attach()`` raises there.
"""

import _audiodsp

#: The audiodsp this was built from (src/cp_compat/audiodsp_build.h).
__version__ = _audiodsp.__version__
__revision__ = _audiodsp.__revision__

#: Values in a slice.
FEATURE_SIZE = _audiodsp.FEATURES_SIZE
#: The rate the frontend itself runs at.
SAMPLE_RATE = _audiodsp.FEATURES_RATE
#: Audio per slice, in milliseconds.
STEP_MS = _audiodsp.FEATURES_STEP_MS


def quantize(value):
    """The int8 a model takes for one raw value: value * 256 / 666 - 128, rounded."""
    if not 0 <= value <= 65535:
        raise ValueError("value 0..65535")
    v = (value * 256 + 333) // 666 - 128
    return max(-128, min(127, v))


class Frontend:
    """Feature slices from PCM, ten a second, held until read."""

    def __init__(self, *, slices=100):
        self._state = _audiodsp.FeaturesState(slices)

    def _live(self):
        if self._state is None:
            raise ValueError("frontend is deinitialized")
        return self._state

    def feed(self, buffer, sample_rate, channel_count=1):
        """Interleaved s16 frames at any rate from 16 to 384 kHz. Returns the slices waiting."""
        return self._live().feed(buffer, sample_rate, channel_count)

    def attach(self, source, sample_rate=None):
        self._live()
        raise NotImplementedError("attach() needs a board's pump: on CPython, feed() the samples")

    def detach(self):
        self._live()

    def readinto(self, buf):
        """Whole slices of 40 int8 into ``buf``, oldest first. Returns how many."""
        return self._live().read(buf, False)

    def readinto_raw(self, buf):
        """Whole slices of 40 uint16 (the frontend's own values) into ``buf``. Returns how many."""
        return self._live().read(buf, True)

    def reset(self):
        """Forget the audio so far and every queued slice."""
        self._live().reset()

    def stats(self):
        slices, dropped, waiting, capacity, rate, resampling = self._live().stats()
        return {"slices": slices, "dropped": dropped, "waiting": waiting, "capacity": capacity,
                "rate": rate, "resampling": resampling, "lapped": 0, "source": None}

    def deinit(self):
        self._state = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.deinit()
