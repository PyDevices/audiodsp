"""A table-driven waveshaper that does its shaping above the sample rate.

Not a CircuitPython module, and not from micropython-vst3's engine either --
audioif adds it. `audiofilters.Distortion` exists upstream and runs one of
four fixed curves at the base rate; this one takes the curve as data and
oversamples:

    from array import array
    import audioshaper

    # a soft knee at +-0.6, computed once, on the desktop
    points = 2048
    curve = array("h")
    for i in range(points):
        x = -1.0 + 2.0 * i / (points - 1)
        y = x if abs(x) <= 0.6 else (x / abs(x)) * (
            0.6 + 0.4 * (1.0 - 2.718281828 ** (-3.0 * (abs(x) - 0.6) / 0.4)))
        curve.append(max(-32768, min(32767, int(round(y * 32767)))))

    drive = audioshaper.Waveshaper(
        sample_rate=48000, curve=curve, oversample=4,
        pre_gain=8.0, post_gain=0.5, mix=1.0)
    drive.play(source)
    audio_out.play(drive)

The curve is where the circuit lives: diodes in a feedback loop, diodes to
ground, a biased germanium pair are each a different shape, and none of them
is one of `Distortion`'s four. It is computed once, on CPython, and shipped
as data -- never rebuilt on the target, whose float is single-precision
where the desktop's is double, so a table built on a board would be a
different table.

`pre_gain` is the drive knob: gain into one normalised curve, rather than a
curve rebuilt on every knob move. `bias` moves the operating point. A bias
that has to move *per sample* is a second stream summed in front of this
node -- `audiomixer.Mixer` adds sample by sample -- not an argument here.

`oversample` (1, 2, 4 or 8) is how far above the sample rate the shaping
happens, between a matched pair of polyphase all-pass half-bands. It is the
whole reason this module exists: a nonlinearity makes harmonics above
Nyquist and they fold back onto the signal, and nothing else in audioif
resamples at all.

A curve that reaches the rails has a cost on the way back down, though: the
band-limited version of a full-scale clipped edge does not fit in int16.
The decimator rings about a third past the rails on an edge like that --
at the base rate, after the oversampling is already done, where no factor
of it reaches -- and `post_gain` is what decides whether that overshoot
then clips. Keep `post_gain * max(abs(curve))` at or below `CLIP_HEADROOM`
(about 0.74 of full scale) for a curve that reaches the rails, and put the
rest of the wanted level on a mixer voice after this node rather than on
this knob. Measured table: `docs/upstream-diff.md`, "`audioshaper`"
(audioif#99).

`hysteresis` is off by default and is the one thing a table cannot do: give
the curve a memory, so a slow triangle in and out traces two different paths
and encloses an area. At zero the node is a static table, sample for sample.

A new module rather than arguments on `Distortion`, deliberately: an argument
added to audioif's copy of a CircuitPython module would not exist on a stock
board, so an effect written against it would silently be a different effect
there. This either installs whole or is absent and says so on import.

`SampleHold` is the module's other node and the same argument again, one
layer down: the waveshaper quantises the value, the hold quantises the time,
and neither belongs bolted onto a module CircuitPython ships. See its
docstring.
"""

from audiocore import (
    GET_BUFFER_DONE, GET_BUFFER_ERROR, GET_BUFFER_MORE_DATA, _AudioSample,
    get_buffer, reset_buffer,
)
import _audioif


#: The audioif this was built from, the same pair the native builds put
#: on this module (src/cp_compat/audioif_build.h). audioif#55.
__version__ = _audioif.__version__
__revision__ = _audioif.__revision__

FRAMES = _audioif.SHAPER_FRAMES
MAX_OVERSAMPLE = _audioif.SHAPER_MAX_OVERSAMPLE

#: `SampleHold`'s own block size and the largest `num` a ratio may name.
HOLD_FRAMES = _audioif.SAMPLEHOLD_FRAMES
MAX_HOLD_RATIO = _audioif.SAMPLEHOLD_MAX_RATIO

#: Option name -> the native configure() slot. Kept in the order
#: shared/audioif_shaper.h declares, which is the order the MicroPython
#: bindings list them in too. Append only, never renumber.
_OPTIONS = {
    "pre_gain": 0,
    "bias": 1,
    "post_gain": 2,
    "mix": 3,
    "hysteresis": 4,
    "hysteresis_width": 5,
    "hysteresis_bias": 6,
}

#: Group delay of the whole up/shape/down chain, in samples at the base rate,
#: per oversampling factor. Measured on the built extension rather than
#: derived: a 200 Hz..5 kHz sine in, the output's phase against the input's,
#: at 48 kHz (tools/design_halfband.py holds the filter design the numbers
#: come out of). It is flat across the audio band to within a hundredth of a
#: sample, and the magnitude response over 200 Hz..18 kHz is flat to within
#: 0.001 dB. A component reporting `latency_samples` should report the entry
#: for the factor it built with -- it is small, but it is not zero.
GROUP_DELAY_SAMPLES = {1: 0.0, 2: 2.2, 4: 3.3, 8: 3.9}

#: Above roughly this fraction of full scale, `post_gain` on a curve that
#: reaches the rails re-clips the decimator's own overshoot at the base
#: rate rather than anything the oversampling can still fix -- see the
#: docstring above and docs/upstream-diff.md's `audioshaper` section for
#: the measured table (audioif#99). Documentation only, the same as
#: `GROUP_DELAY_SAMPLES` above it: neither the MicroPython usermod's module
#: globals (`src/audioshaper/module.c`) nor the CircuitPython spike's
#: (`shared-bindings/audioshaper/__init__.c`) export anything past
#: `__version__`/`__revision__` and the two types, so there is no second
#: target yet for this figure to agree with.
CLIP_HEADROOM = 0.74


class Waveshaper(_AudioSample):
    def __init__(self, sample_rate=48000, curve=None, oversample=4,
                 **options):
        channel_count = int(options.pop("channel_count", 2))
        if channel_count not in (1, 2):
            raise ValueError("channel_count must be 1 or 2")
        # Powers of two only, and no higher than 8: the state is a fixed
        # number of half-band stages, and rounding a stray 3 down to 2 would
        # be a different effect than the caller asked for, quietly.
        oversample = int(options.pop("oversample", oversample))
        if oversample not in (1, 2, 4, 8):
            raise ValueError("oversample must be 1, 2, 4 or 8")
        curve = options.pop("curve", curve)
        if curve is None:
            raise ValueError("curve is required")
        options.pop("sample_rate", None)
        self.sample_rate = int(sample_rate)
        self.bits_per_sample = 16
        self.channel_count = channel_count
        self.samples_signed = True
        self.single_buffer = False
        self.max_buffer_length = FRAMES * 2 * channel_count
        self.oversample = oversample
        self._deinited = False
        self._source = None
        self._pending = b""
        self._state = _audioif.WaveshaperState(
            sample_rate=self.sample_rate, oversample=oversample,
            channel_count=channel_count)
        self._state.load_curve(bytes(memoryview(curve).cast("B")))
        self._apply(options)

    def _apply(self, options):
        for name, value in options.items():
            if name == "curve":
                self._state.load_curve(bytes(memoryview(value).cast("B")))
                continue
            slot = _OPTIONS.get(name)
            if slot is None:
                raise TypeError("unknown Waveshaper option %r" % (name,))
            self._state.configure(slot, float(value))
        self._state.finish()

    def set(self, **options):
        """Change settings mid-stream. The half-band memories and the play
        position keep their contents; only what the node does to them
        changes."""
        self._check()
        self._apply(options)

    def clear(self):
        """Empty the half-band memories and the play position. That is the
        whole of this node's state: it has no delay line."""
        self._check()
        self._state.reset()

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
        # A starved chain gets silence rather than a short block: this node
        # sits in the middle of a live graph and never reports itself
        # finished. It has no tail of its own -- no delay line, no
        # reverberation -- so silence in really is silence out, once the
        # half-bands have rung down.
        if produced == 0:
            return GET_BUFFER_MORE_DATA, memoryview(
                bytes(FRAMES * 2 * self.channel_count))
        return GET_BUFFER_MORE_DATA, memoryview(bytes(output))


class SampleHold(_AudioSample):
    """A zero-order hold at an exact rational ratio: `num` frames carry
    `den` new values.

        hold = audioshaper.SampleHold(source, num=48000, den=26040)

    One source frame in, one frame out, at the source's own sample rate,
    channel count and bit depth -- so this is a rate *reducer*, not a
    resampler, and the block after it is the length the block before it was.
    The value changes only when the accumulator wraps, and the accumulator is
    the exact remainder of `n * den` modulo `num`, so 26040 Hz at 48 kHz
    (reduced, 217/400) refreshes 217 times in every 400 frames forever.

    **Why it exists.** The palette's sample-and-hold was a pair of
    `audiospeed.SpeedChanger` nodes, down by the hold ratio and up by its
    reciprocal, and the pair cannot be made reciprocal: that rate is 16.16
    fixed point, so it inverts exactly only at powers of two. Measured on the
    shipped 26 040 Hz hold, the product of the two rates was
    0.9999947184696794 at 48 kHz -- one sample late per 189 339 frames -- and
    1.0000107865780592 at 44.1 kHz, one sample early per 92 708. At Mix 0.5 a
    steady 12 kHz tone swung 10.74 dB over a twelve-second render: a slow
    flange on a setting nobody was touching (audioif#97). Counting cannot
    drift, so this counts.

    **`num`/`den` rather than a rate in hertz**, because the rounding has to
    be the caller's. A class with a `rate_hz` knob decides how to land it on a
    pair -- `num=sample_rate, den=round(rate_hz)` is exact and is what the
    node then reports back, reduced -- and it is the class that should say
    which hold rate it actually got.

    **Latency is 0.** A refresh latches the frame it is looking at and emits
    it in the same frame, so `num == den` is a wire byte for byte. What a hold
    displaces is an event landing on a frame it drops: that arrives on the
    next kept frame, up to `ceil(num/den) - 1` frames later, which is the
    effect rather than a delay of this node.

    `set(num, den)` moves the ratio mid-stream, re-arming the accumulator when
    the reduced pair actually changes and doing nothing at all when it does
    not. `play(sample)` re-sources, in the format fixed at construction.
    `clear()` arms the accumulator and forgets the held frame, which is the
    whole of this node's state.
    """

    def __init__(self, source, num=1, den=1):
        if source is None:
            raise ValueError("source is required")
        num = int(num)
        den = int(den)
        if num < 1 or den < 1 or den > num or num > MAX_HOLD_RATIO:
            raise ValueError("num and den must be whole, den <= num (a hold "
                             "cannot invent frames)")
        self.sample_rate = int(source.sample_rate)
        self.bits_per_sample = int(source.bits_per_sample)
        self.channel_count = int(source.channel_count)
        self.samples_signed = bool(source.samples_signed)
        self.single_buffer = False
        frame_bytes = self.bits_per_sample // 8 * self.channel_count
        if frame_bytes < 1 or frame_bytes > 4:
            raise ValueError("source frames must be 1 or 2 channels of 8- or "
                             "16-bit audio")
        self._frame_bytes = frame_bytes
        self.max_buffer_length = HOLD_FRAMES * frame_bytes
        self._deinited = False
        self._source = source
        self._pending = b""
        self._source_done = False
        self._exhausted = False
        self._state = _audioif.SampleHoldState(num=num, den=den)
        self._num, self._den = self._state.ratio()

    @property
    def num(self):
        self._check()
        return self._num

    @property
    def den(self):
        self._check()
        return self._den

    @property
    def latency(self):
        """0, at every ratio. See the class docstring for what a hold
        displaces instead, and whose job it is to report that."""
        self._check()
        return 0

    @property
    def playing(self):
        return self._source is not None

    def set(self, num, den):
        """Change the ratio mid-stream. The pair moves together or not at
        all: half a new ratio is exactly the transient state the exact
        accumulator exists to rule out."""
        self._check()
        num = int(num)
        den = int(den)
        if num < 1 or den < 1 or den > num or num > MAX_HOLD_RATIO:
            raise ValueError("num and den must be whole, den <= num (a hold "
                             "cannot invent frames)")
        self._state.configure(num, den)
        self._num, self._den = self._state.ratio()

    def clear(self):
        """Arm the accumulator and forget the held frame. That is the whole
        of this node's state: it has no delay line and no filter."""
        self._check()
        self._state.reset()

    def play(self, sample, *, loop=False):
        self._check()
        if (int(sample.bits_per_sample) != self.bits_per_sample or
                int(sample.channel_count) != self.channel_count):
            raise ValueError("source format does not match the one this node "
                             "was built with")
        self._source = sample
        self._pending = b""
        self._source_done = False
        self._exhausted = False
        self._state.reset()

    def stop(self):
        self._source = None
        self._pending = b""
        self._exhausted = True

    def _release(self):
        self.stop()
        self._state.reset()

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        if self._source is not None:
            reset_buffer(self._source, False, 0)
        self._pending = b""
        self._source_done = False
        self._exhausted = False
        self._state.reset()

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        width = self._frame_bytes
        output = bytearray()
        produced = 0
        while produced < HOLD_FRAMES:
            if not self._pending:
                if (self._source is None or self._exhausted or
                        self._source_done):
                    self._exhausted = True
                    break
                result, data = get_buffer(self._source, False, 0)
                data = bytes(data)
                if result == GET_BUFFER_ERROR or len(data) < width:
                    self._exhausted = True
                    break
                self._pending = data[:len(data) // width * width]
                self._source_done = result == GET_BUFFER_DONE
            run = min(HOLD_FRAMES - produced, len(self._pending) // width)
            output += self._state.process(self._pending[:run * width], width)
            self._pending = self._pending[run * width:]
            produced += run
        # One frame in, one frame out, and the source's own ending. A node
        # that manufactured silence here would move where a chain ends, and
        # the pair of `SpeedChanger`s this replaces did not.
        if produced == 0 or self._exhausted:
            return GET_BUFFER_DONE, memoryview(bytes(output))
        return GET_BUFFER_MORE_DATA, memoryview(bytes(output))


__all__ = ("SampleHold", "Waveshaper")
