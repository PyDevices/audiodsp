"""A delay with a filter, a soft-clip and a cross-feed inside its feedback
loop.

Not a CircuitPython module, and not from micropython-vst3's engine either --
audiodsp adds it. `audiodelays.Echo` exists upstream and its feedback path is
`echo * decay` and nothing else, so everything a delay is actually *called*
falls out of what this module puts in that path:

    tape = audioecho.FeedbackDelay(
        sample_rate=48000, max_delay_ms=600, delay_ms=340, feedback=0.45,
        mix=0.28, damping_hz=3200, cut_hz=180, wow_hz=0.7,
        wow_depth_ms=1.4, loop_drive=0.35)
    tape.play(source)
    audio_out.play(tape)

`damping_hz` and `cut_hz` are one-pole filters *in* the loop, so each repeat
loses a little more top and bottom than the last - which is what makes tape
sound like tape rather than like a delay with a tone control after it.
`loop_drive` softens each pass the same way. `wow_hz`/`wow_depth_ms` modulate
the delay per sample, so the repeats get the doppler an LFO at block rate
cannot reach. `cross_feed` sends each channel's repeats into the other
channel's line, and `input_pan` steers the *input* to one line only: hard
over with full cross-feed is a real ping-pong, where the first repeat is on
one side alone.

Four more options, every one of them off by default, so a node built the way
it was before them renders the same bytes:

`wow_shape` replaces the built-in sine with one period of your own, as int16
Q15 samples whose count is a power of two from 2 to 4096. A bucket brigade's
delay is its line length over its clock, so a triangle on the clock is a
*reciprocal* on the delay and no sine can be it - with a table the law is
Python's to bake and C only looks it up:

    from array import array
    period = 64
    clock = array("h", [(i * 65534) // period - 32767 for i in range(period)])
    small_clone = audioecho.FeedbackDelay(
        sample_rate=48000, max_delay_ms=30, delay_ms=6.5, mix=1.0,
        wow_hz=0.5, wow_depth_ms=2.5, wow_shape=clock)

`wow_shape=None` puts the sine back. The table is *borrowed*, not copied:
keep a reference to it for as long as the node is alive.

`delay_slew` walks the read head to a new `delay_ms` instead of jumping to
it, in delay-seconds per second - the same number at any sample rate. 0 (the
default) is the jump this node has always made. 1.0 is the read head standing
still while the write head runs, which is an octave up for exactly as long as
the move lasts; the pitch is constant while it travels and there is no
discontinuity at either end, which is both the varispeed an Echoplex makes
and the reason a delay-time knob can be click-free.

`wow_am_depth` (0..1) puts the wow oscillator on the wet gain as well as on
the delay. It dips to `1 - depth` and returns to unity, never boosts, and it
is on the output only - the feedback path is untouched.

`loop_semitones` (-24..+24) pitch-shifts the line read *inside* the loop, so
every pass rises again: +12 gives a repeat an octave up, then two, then
three, which is what a shimmer is. Two taps half a `loop_window_ms` window
apart are crossfaded, so the window trades grain rate against smear; it
defaults to 25 ms and is capped at a quarter of the line.

A shifted loop repeats half a window later than an unshifted one - the two
taps' gains sum to one, so their weighted mean read is `delay_ms +
loop_window_ms / 2` at every point of the cycle. Measured at 48 kHz with
`delay_ms=200`: 212.49 ms with the default window, 229.98 ms with a 60 ms
one, 200.00 ms with the shift off. So switching `loop_semitones` on or off
mid-stream steps the read by that half window, and nothing smooths it. Set
it when you build the node, or between takes.

A new module rather than arguments on `Echo`, deliberately: an argument added
to audiodsp's copy of a CircuitPython module would not exist on a stock board,
so an effect written against it would silently be a different effect there.
This either installs whole or is absent and says so on import.
"""

from audiocore import (
    GET_BUFFER_ERROR, GET_BUFFER_MORE_DATA, _AudioSample, get_buffer,
)
import _audiodsp


#: The audiodsp this was built from, the same pair the native builds put
#: on this module (src/cp_compat/audiodsp_build.h). audiodsp#55.
__version__ = _audiodsp.__version__
__revision__ = _audiodsp.__revision__

FRAMES = _audiodsp.FEEDBACK_DELAY_FRAMES

#: Option name -> the native configure() slot. Kept in the order
#: shared/audiodsp_feedback_delay.h declares, which is the order the
#: MicroPython bindings list them in too.
_OPTIONS = {
    "delay_ms": 0,
    "feedback": 1,
    "mix": 2,
    "damping_hz": 3,
    "cut_hz": 4,
    "wow_hz": 5,
    "wow_depth_ms": 6,
    "cross_feed": 7,
    "loop_drive": 8,
    "input_pan": 9,
    "delay_slew": 10,
    "wow_am_depth": 11,
    "loop_semitones": 12,
    "loop_window_ms": 13,
}


class FeedbackDelay(_AudioSample):
    def __init__(self, sample_rate=48000, max_delay_ms=250.0, **options):
        channel_count = int(options.pop("channel_count", 2))
        if channel_count not in (1, 2):
            raise ValueError("channel_count must be 1 or 2")
        options.pop("sample_rate", None)
        options.pop("max_delay_ms", None)
        self.sample_rate = int(sample_rate)
        self.bits_per_sample = 16
        self.channel_count = channel_count
        self.samples_signed = True
        self.single_buffer = False
        self.max_buffer_length = FRAMES * 2 * channel_count
        self._deinited = False
        self._source = None
        self._pending = b""
        if max_delay_ms <= 0:
            raise ValueError("max_delay_ms must be positive")
        self._state = _audiodsp.FeedbackDelayState(
            sample_rate=self.sample_rate, max_delay_ms=float(max_delay_ms),
            channel_count=channel_count)
        # The default delay is half the line rather than all of it, so a
        # caller who sizes the line and says nothing else still hears repeats.
        self._apply(options)
        self._state.finish()

    def _apply(self, options):
        for name, value in options.items():
            # wow_shape is a buffer, not a number, so it does not go through
            # configure()'s float table.
            if name == "wow_shape":
                self._state.set_wow_shape(value)
                continue
            slot = _OPTIONS.get(name)
            if slot is None:
                raise TypeError("unknown FeedbackDelay option %r" % (name,))
            self._state.configure(slot, float(value))

    def set(self, **options):
        """Change settings mid-stream. The line and the filters keep their
        contents; only what the loop does to them changes."""
        self._check()
        self._apply(options)

    def clear(self):
        """Empty the line and the loop filters."""
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
        # Unlike audiodynamics, everything goes. A delay's whole state is
        # audible: a chain restarted with the old repeats still in the line
        # plays the previous take over the new one.
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
        # finished. Note that the tail stops with the source -- the line is
        # only advanced by frames that arrive, so repeats do not ring on into
        # silence. That matches audiodelays.Echo, whose users expect it.
        if produced == 0:
            return GET_BUFFER_MORE_DATA, self._publish(
                bytes(FRAMES * 2 * self.channel_count))
        return GET_BUFFER_MORE_DATA, self._publish(output)


__all__ = ("FeedbackDelay",)
