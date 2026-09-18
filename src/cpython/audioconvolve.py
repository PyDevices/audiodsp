"""Convolution: an impulse response applied properly, rather than imitated.

Not a CircuitPython module, and not from micropython-vst3's engine either --
audioif adds it. It is the one effect on the catalogue the others cannot
approximate. A plate emulation is a network of delays that *sounds like* a
plate; a convolution of a plate's recorded impulse *is* that plate. The same
node is a hall, a guitar cabinet, a spring tank or a telephone, depending only
on which impulse it was handed::

    cab = audioconvolve.Convolver(impulse=taps, max_taps=2048, mix=1.0)
    cab.play(source)
    audio_out.play(cab)

or, with no file to load::

    hall = audioconvolve.Convolver(max_taps=48000, ir_channels=2)
    hall.synthesize(decay=2.4, damping_hz=4000, predelay_ms=25,
                    diffusion_ms=12, seed=4)
    hall.set(mix=0.35)

`impulse` is int16 frames, interleaved if `impulse_channels` is 2. `max_taps`
sizes the convolver and cannot grow afterwards -- see the module docstring in
shared/audioif_convolve.h for what a partition costs, because on a
microcontroller that number decides whether an impulse fits at all.

**The output lags the input by `latency` frames** (one partition, 5.3 ms at
48 kHz). A block cannot be transformed before it is complete, and removing
that would mean a non-uniform partitioning scheme worth roughly triple the
code -- which matters only when monitoring a live player. A convolver with no
impulse loaded passes its input through with no latency at all: an impulse
that has not arrived is a missing setting, not a null room.
"""

from audiocore import (
    GET_BUFFER_ERROR, GET_BUFFER_MORE_DATA, _AudioSample, get_buffer,
)
import _audioif


#: The audioif this was built from, the same pair the native builds put
#: on this module (src/cp_compat/audioif_build.h). audioif#55.
__version__ = _audioif.__version__
__revision__ = _audioif.__revision__

FRAMES = _audioif.CONVOLVE_FRAMES
MAX_PARTITIONS = _audioif.CONVOLVE_MAX_PARTITIONS

_OPTIONS = {"mix": 0}


def _frames_of(impulse, channels):
    data = bytes(impulse)
    if len(data) % (2 * channels):
        raise ValueError("impulse length must be whole int16 frames")
    return data, len(data) // (2 * channels)


class Convolver(_AudioSample):
    def __init__(self, impulse=None, impulse_channels=1, sample_rate=48000,
                 max_taps=0, ir_channels=0, gain=1.0, mix=None,
                 channel_count=2):
        channel_count = int(channel_count)
        if channel_count not in (1, 2):
            raise ValueError("channel_count must be 1 or 2")
        if impulse_channels not in (1, 2):
            raise ValueError("impulse_channels must be 1 or 2")
        taps = b""
        tap_frames = 0
        if impulse is not None:
            taps, tap_frames = _frames_of(impulse, impulse_channels)

        # Capacity comes from `max_taps` when given, from the impulse when
        # not, and otherwise from a default that is a cabinet rather than a
        # hall: a convolver sized for a second of reverb costs a megabyte, and
        # nobody should get that by leaving an argument out.
        if not max_taps:
            max_taps = tap_frames or 4096
        max_taps = max(max_taps, tap_frames)
        partitions = (max_taps + FRAMES - 1) // FRAMES
        if partitions > MAX_PARTITIONS:
            raise ValueError("impulse is too long")
        if not ir_channels:
            ir_channels = impulse_channels
        if ir_channels not in (1, 2):
            raise ValueError("ir_channels must be 1 or 2")

        self.sample_rate = int(sample_rate)
        self.bits_per_sample = 16
        self.channel_count = channel_count
        self.samples_signed = True
        self.single_buffer = False
        self.max_buffer_length = FRAMES * 2 * channel_count
        self._deinited = False
        self._source = None
        self._pending = b""
        self._max_taps = partitions * FRAMES
        self._state = _audioif.ConvolverState(
            sample_rate=self.sample_rate, partitions=partitions,
            ir_channels=ir_channels, channel_count=channel_count)
        if mix is not None:
            self._state.configure(0, float(mix))
        if tap_frames:
            self._state.load(taps, impulse_channels, float(gain))

    def _apply(self, options):
        for name, value in options.items():
            slot = _OPTIONS.get(name)
            if slot is None:
                raise TypeError("unknown Convolver option %r" % (name,))
            self._state.configure(slot, float(value))

    def set(self, **options):
        """Change settings mid-stream. Only `mix`: everything else about a
        convolver is the impulse, which `load` and `synthesize` replace."""
        self._check()
        self._apply(options)

    def load(self, impulse, channels=1, gain=1.0):
        """Replace the impulse. Deliberately does not grow the convolver to
        fit -- the capacity was chosen at construction and something
        downstream may already be pulling, so an impulse longer than the room
        made for it is a sizing mistake worth hearing about."""
        self._check()
        if channels not in (1, 2):
            raise ValueError("channels must be 1 or 2")
        taps, frames = _frames_of(impulse, channels)
        if frames > self._max_taps:
            raise ValueError("impulse is longer than max_taps")
        self._state.load(taps, channels, float(gain))

    def synthesize(self, decay=2.0, damping_hz=0.0, predelay_ms=0.0,
                   diffusion_ms=0.0, seed=1):
        """Build a decaying-noise impulse in place, filling the whole
        capacity, so a reverb is available without a file to load.

        `decay` is the -60 dB time in seconds; `damping_hz` rolls the tail's
        top off (0 leaves it bright); `predelay_ms` is silence before anything
        arrives; `diffusion_ms` fades the tail in rather than starting it at
        full amplitude, which is what stops a synthetic impulse reading as a
        burst of noise. `seed` picks the room -- two seeds are two different
        halls of the same size, and with `ir_channels=2` the two channels get
        different noise under one envelope, which is one room in stereo rather
        than two rooms."""
        self._check()
        self._state.synthesize(float(decay), float(damping_hz),
                               float(predelay_ms), float(diffusion_ms),
                               int(seed) & 0xFFFFFFFF)

    def clear(self):
        """Drop the history and the block in flight. The impulse stays: that
        is a setting, not audio."""
        self._check()
        self._state.reset()

    @property
    def taps(self):
        """Frames of impulse currently loaded, rounded up to a partition.
        Zero means the convolver is passing its input through."""
        return self._state.taps()

    @property
    def latency(self):
        """Frames the output trails the input by: one partition once an
        impulse is loaded, and **zero when none is**, because an unloaded
        convolver passes its input through. It used to answer one partition
        either way, which told a class that compensates to compensate for a
        delay that was not there (audioif#44)."""
        return FRAMES if self._state.taps() else 0

    @property
    def playing(self):
        return self._source is not None

    def play(self, sample, *, loop=False):
        self._check()
        self._source = sample
        self._pending = b""

    def stop(self):
        self._source = None
        self._pending = b""

    def _release(self):
        self.stop()

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        self._pending = b""
        # The history goes; the impulse stays. One is audio in flight and the
        # other is a setting -- reloading a room because playback restarted
        # would be both wrong and expensive.
        self._state.reset()

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        output = bytearray()
        produced = 0
        while produced < FRAMES:
            if not self._pending:
                if self._source is None:
                    break
                result, data = get_buffer(self._source, False, 0)
                data = bytes(data)
                if result == GET_BUFFER_ERROR or len(data) < 2 * self.channel_count:
                    break
                width = 2 * self.channel_count
                self._pending = data[:len(data) // width * width]
            width = 2 * self.channel_count
            run = min(FRAMES - produced, len(self._pending) // width)
            output += self._state.process(self._pending[:run * width])
            self._pending = self._pending[run * width:]
            produced += run
        # A starved chain gets silence rather than a short block, and the tail
        # stops with the source: only frames that arrive advance the
        # convolution, so a reverb does not ring on into silence after its
        # input ends. Same rule as audioecho and audiodelays, and for the same
        # reason -- a node in the middle of a live graph never reports itself
        # finished.
        if produced == 0:
            return GET_BUFFER_MORE_DATA, self._publish(
                bytes(FRAMES * 2 * self.channel_count))
        return GET_BUFFER_MORE_DATA, self._publish(output)


__all__ = ("Convolver",)
