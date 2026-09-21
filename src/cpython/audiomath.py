"""Arithmetic on audio streams: one multiplied by another, and one divided
down in frequency.

Unlike most of this package, `audiomath` is not a CircuitPython module and did
not come from micropython-vst3's engine either - audiodsp adds it. Nothing else
in the palette multiplies two *streams*: `synthio` rings a note against an
oscillator, which cannot reach a microphone, a sampler, or the output of
another effect, and an LFO-driven parameter updates once per block (about
187 Hz at 48 kHz) where a ring modulator wants hundreds of hertz.

    carrier = audiocore.RawSample(one_cycle_of_sine, sample_rate=48000,
                                  channel_count=2)
    node = audiomath.Multiply(source, carrier, mix=1.0)
    audio_out.play(node)

The two inputs fail in opposite directions, deliberately. A source that runs
dry gives silence, as every other effect here does; a modulator that is absent
or has stopped lets the signal through untouched, because multiplying by
nothing and multiplying by zero must not mean the same thing.

`SubOctave` is the other kind of arithmetic on one stream: a comparator into a
chain of flip-flops, which is the Boss OC-2's front end and every analog
divider's. It inverts every other cycle of the signal, so the result has twice
the period, the input's own timbre, and no latency at all:

    node = audiomath.SubOctave(source, order=1, mix=1.0)
    audio_out.play(node)

`audiodelays.PitchShift` also goes down an octave, but granularly - windowed
grains, a comb, and a buffer per instance. A divider is none of that, and does
not sound like it.

`remix_s16` is not a graph node. It is the buffer primitive a push path needs
when the producer is stereo and the wire is mono (or the reverse): interleaved
native-endian s16, 1↔2, optional writable destination so a caller can reuse
scratch. `audiomixer.Mixer` cannot do this — its sources must already match
`channel_count`.
"""

from audiocore import (
    GET_BUFFER_ERROR, GET_BUFFER_MORE_DATA, _AudioSample, get_buffer,
)
import _audiodsp


#: The audiodsp this was built from, the same pair the native builds put
#: on this module (src/cp_compat/audiodsp_build.h). audiodsp#55.
__version__ = _audiodsp.__version__
__revision__ = _audiodsp.__revision__

FRAMES = _audiodsp.MULTIPLY_FRAMES
SUBOCTAVE_FRAMES = _audiodsp.SUBOCTAVE_FRAMES


def remix_s16(source, src_channels, dst_channels, dest=None):
    """Convert interleaved s16 native-endian PCM between 1 and 2 channels.

    Stereo→mono is (L+R)/2. Mono→stereo duplicates the sample. Same channel
    counts copy. Pass a writable ``dest`` to fill in place; otherwise a new
    ``bytes`` is returned.
    """
    return _audiodsp.remix_s16(source, int(src_channels), int(dst_channels),
                              dest)

#: Option name -> the native configure() slot, in the order
#: shared/audiodsp_suboctave.h declares them, which is the order the
#: MicroPython bindings list them in too.
_SUBOCTAVE_OPTIONS = {
    "order": 0,
    "mix": 1,
    "threshold": 2,
    "hold_ms": 3,
}


class Multiply(_AudioSample):
    def __init__(self, source=None, modulator=None, mix=1.0,
                 sample_rate=48000, channel_count=2):
        channel_count = int(channel_count)
        if channel_count not in (1, 2):
            raise ValueError("channel_count must be 1 or 2")
        self.sample_rate = int(sample_rate)
        self.bits_per_sample = 16
        self.channel_count = channel_count
        self.samples_signed = True
        self.single_buffer = False
        self.max_buffer_length = FRAMES * 2 * channel_count
        self._deinited = False
        self._source = source
        self._modulator = modulator
        self._mix = 1.0
        self._pending_source = b""
        self._pending_modulator = b""
        self._apply({"mix": mix})

    def _apply(self, options):
        for name, value in options.items():
            if name != "mix":
                raise TypeError("unknown Multiply option %r" % (name,))
            self._mix = min(1.0, max(0.0, float(value)))

    def set(self, **options):
        """Change settings mid-stream."""
        self._check()
        self._apply(options)

    @property
    def playing(self):
        return self._source is not None

    def play(self, sample, *, loop=False):
        """Set the signal - the input that gets multiplied."""
        self._check()
        self._source = sample
        self._pending_source = b""

    def modulate(self, sample):
        """Set what to multiply it by."""
        self._check()
        self._modulator = sample
        self._pending_modulator = b""

    def stop(self):
        self._source = None
        self._pending_source = b""

    def _release(self):
        self.stop()

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        self._pending_source = b""
        # The modulator's cursor goes too, so a chain restarted mid-cycle
        # begins at the top of the table rather than wherever it stopped.
        self._pending_modulator = b""

    def _pull(self, sample):
        result, data = get_buffer(sample, False, 0)
        data = bytes(data)
        width = 2 * self.channel_count
        if result == GET_BUFFER_ERROR or len(data) < width:
            return b""
        return data[:len(data) // width * width]

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        output = bytearray()
        produced = 0
        while produced < FRAMES:
            if not self._pending_source:
                if self._source is None:
                    break
                self._pending_source = self._pull(self._source)
                if not self._pending_source:
                    break
            if not self._pending_modulator and self._modulator is not None:
                # A modulator is normally a short looping table, which hands
                # back its whole length every time it is asked; one that has
                # genuinely stopped leaves the signal alone rather than
                # muting it.
                self._pending_modulator = self._pull(self._modulator)
            width = 2 * self.channel_count
            run = min(FRAMES - produced, len(self._pending_source) // width)
            if self._pending_modulator:
                run = min(run, len(self._pending_modulator) // width)
                output += _audiodsp.multiply_s16(
                    self._pending_source[:run * width],
                    self._pending_modulator[:run * width], self._mix,
                    self.channel_count)
                self._pending_modulator = self._pending_modulator[run * width:]
            else:
                output += self._pending_source[:run * width]
            self._pending_source = self._pending_source[run * width:]
            produced += run
        # A starved chain gets silence rather than a short block: this node
        # sits in the middle of a live graph and never reports itself finished.
        if produced == 0:
            return GET_BUFFER_MORE_DATA, self._publish(
                bytes(FRAMES * 2 * self.channel_count))
        return GET_BUFFER_MORE_DATA, self._publish(output)


class SubOctave(_AudioSample):
    # The four options default to None rather than to their values, and an
    # option that is None is simply not configured. The defaults live in one
    # place -- shared/audiodsp_suboctave.c's config_init -- for all four
    # targets, so they cannot drift apart between the runtimes the way a
    # number repeated in each binding would. They are order 1, mix 1.0,
    # threshold 0.01 and hold_ms 1.0.
    def __init__(self, source=None, order=None, mix=None, threshold=None,
                 hold_ms=None, sample_rate=48000, channel_count=2):
        channel_count = int(channel_count)
        if channel_count not in (1, 2):
            raise ValueError("channel_count must be 1 or 2")
        self.sample_rate = int(sample_rate)
        self.bits_per_sample = 16
        self.channel_count = channel_count
        self.samples_signed = True
        self.single_buffer = False
        self.max_buffer_length = SUBOCTAVE_FRAMES * 2 * channel_count
        self._deinited = False
        self._source = source
        self._pending = b""
        self._state = _audiodsp.SubOctaveState(
            sample_rate=self.sample_rate, channel_count=channel_count)
        asked = {"order": order, "mix": mix, "threshold": threshold,
                 "hold_ms": hold_ms}
        self._apply({name: value for name, value in asked.items()
                     if value is not None})
        self._state.finish()

    def _apply(self, options):
        for name, value in options.items():
            slot = _SUBOCTAVE_OPTIONS.get(name)
            if slot is None:
                raise TypeError("unknown SubOctave option %r" % (name,))
            self._state.configure(slot, float(value))

    def set(self, **options):
        """Change settings mid-stream. The count keeps running: changing the
        mix must not flip the output's polarity under the player."""
        self._check()
        self._apply(options)

    def clear(self):
        """Back to the pass-through polarity, comparator and lockout clear."""
        self._check()
        self._state.reset()

    @property
    def playing(self):
        return self._source is not None

    def play(self, sample, *, loop=False):
        """Set the signal to divide."""
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
        # The divider holds no audio, so unlike a delay this drops nothing
        # anybody can hear; what it stops is a restarted chain beginning on
        # the inverted half of the count.
        self._state.reset()

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        output = bytearray()
        produced = 0
        width = 2 * self.channel_count
        while produced < SUBOCTAVE_FRAMES:
            if not self._pending:
                if self._source is None:
                    break
                result, data = get_buffer(self._source, False, 0)
                data = bytes(data)
                if result == GET_BUFFER_ERROR or len(data) < width:
                    break
                self._pending = data[:len(data) // width * width]
            run = min(SUBOCTAVE_FRAMES - produced, len(self._pending) // width)
            output += self._state.process(self._pending[:run * width])
            self._pending = self._pending[run * width:]
            produced += run
        # A starved chain gets silence rather than a short block: this node
        # sits in the middle of a live graph and never reports itself
        # finished.
        if produced == 0:
            return GET_BUFFER_MORE_DATA, self._publish(
                bytes(SUBOCTAVE_FRAMES * 2 * self.channel_count))
        return GET_BUFFER_MORE_DATA, self._publish(output)


__all__ = ("Multiply", "SubOctave")
