"""An algorithmic reverberation tank whose topology comes from Python.

Not a CircuitPython module, and not from micropython-vst3's engine either --
audiodsp adds it. `audiofreeverb.Freeverb` is the only reverberator upstream
has, and its line lengths are constants inside a CircuitPython-ported binding:
one topology, no modulated taps, no dispersive input chain, nothing to re-cut
per character. `audioverb.Tank` is Dattorro's plate network with the line
lengths and the output taps handed in::

    plate = audioverb.Tank(
        sample_rate=48000, max_predelay_ms=200, decay=0.7, diffusion=0.75,
        bandwidth_hz=9000, damping_hz=4500, mod_rate_hz=1.0,
        mod_depth_ms=0.27, mix=0.35)
    plate.play(source)
    audio_out.play(plate)

The signal path is Dattorro's: predelay, a one-pole `bandwidth_hz` band limit,
an optional `low_cut_hz` and `drive`, four Schroeder all-passes, then a tank of
two halves that feed each other. Each half is a *modulated* all-pass (a
fractional read wobbling by `mod_depth_ms` at `mod_rate_hz`, which is what
breaks a static plate's picket-fence ringing), a delay, a one-pole `damping_hz`
in the loop, the `decay` multiply, a second all-pass and another delay. The
stereo comes back out of the tap table, then `width`, `tone_db` and `mix`.

`delays` is twelve line lengths in frames, in this order::

    0..3    the four input diffusers
    4..7    tank half A: modulated all-pass, delay, all-pass, delay
    8..11   tank half B: the same four

`taps` is four values per tap -- channel, line index, offset in frames, gain --
and at most 32 of them. Both default to Dattorro's own published table, scaled
from 29761 Hz to whatever `sample_rate` says. His figures, for a class that
wants to scale or re-cut them itself:

    lines   142 107 379 277 | 672 4453 1800 3720 | 908 4217 2656 3163
    left    +.6 line9[266] +.6 line9[2974] -.6 line10[1913] +.6 line11[1996]
            -.6 line5[1990] -.6 line6[187] -.6 line7[1066]
    right   +.6 line5[353] +.6 line5[3627] -.6 line6[1228] +.6 line7[2673]
            -.6 line9[2111] -.6 line10[335] -.6 line11[121]

Every filter is off at zero -- `damping_hz`, `bandwidth_hz`, `low_cut_hz`,
`mod_rate_hz`, `mod_depth_ms`, `drive`, `tone_db`, `predelay_ms` -- so a bare
`Tank()` is that network with nothing added to it. `mix` follows
`audiodelays.Echo`'s 0..2 convention, dry at unity until 1, the same one
`audioecho.FeedbackDelay` adopted, so a rack built out of several of these
nodes has one meaning for the word.

A new module rather than arguments on `Freeverb`, deliberately: an argument
added to audiodsp's copy of a CircuitPython module would not exist on a stock
board, so an effect written against it would silently be a different effect
there. This either installs whole or is absent and says so on import.
"""

from audiocore import (
    GET_BUFFER_ERROR, GET_BUFFER_MORE_DATA, _AudioSample, get_buffer,
)
import _audiodsp


#: The audiodsp this was built from, the same pair the native builds put
#: on this module (src/cp_compat/audiodsp_build.h). audiodsp#55.
__version__ = _audiodsp.__version__
__revision__ = _audiodsp.__revision__

FRAMES = _audiodsp.TANK_FRAMES
#: Delay lines the network owns, and the length `delays` must have.
LINES = _audiodsp.TANK_LINES
#: Output taps the tap table may carry, four values each.
MAX_TAPS = _audiodsp.TANK_MAX_TAPS

#: Option name -> the native configure() slot. Kept in the order
#: shared/audiodsp_tank.h declares, which is the order the MicroPython bindings
#: list them in too.
_OPTIONS = {
    "decay": 0,
    "diffusion": 1,
    "damping_hz": 2,
    "bandwidth_hz": 3,
    "low_cut_hz": 4,
    "predelay_ms": 5,
    "mod_depth_ms": 6,
    "mod_rate_hz": 7,
    "drive": 8,
    "width": 9,
    "tone_db": 10,
    "mix": 11,
}

#: Keywords that shape the allocation rather than setting an option. They are
#: constructor-only, and `set()` refuses them rather than silently ignoring
#: them.
_SHAPE = ("sample_rate", "channel_count", "max_predelay_ms", "delays", "taps")


class Tank(_AudioSample):
    def __init__(self, sample_rate=48000, max_predelay_ms=200.0, delays=None,
                 taps=None, **options):
        channel_count = int(options.pop("channel_count", 2))
        if channel_count not in (1, 2):
            raise ValueError("channel_count must be 1 or 2")
        for name in _SHAPE:
            options.pop(name, None)
        if max_predelay_ms < 0:
            raise ValueError("max_predelay_ms must not be negative")
        self.sample_rate = int(sample_rate)
        self.bits_per_sample = 16
        self.channel_count = channel_count
        self.samples_signed = True
        self.single_buffer = False
        self.max_buffer_length = FRAMES * 2 * channel_count
        self._deinited = False
        self._source = None
        self._pending = b""
        self._state = _audiodsp.TankState(
            sample_rate=self.sample_rate,
            max_predelay_ms=float(max_predelay_ms),
            channel_count=channel_count,
            delays=None if delays is None else [float(v) for v in delays],
            taps=None if taps is None else [float(v) for v in taps])
        self._apply(options)
        self._state.finish()

    def _apply(self, options):
        for name, value in options.items():
            slot = _OPTIONS.get(name)
            if slot is None:
                raise TypeError("unknown Tank option %r" % (name,))
            self._state.configure(slot, float(value))

    def set(self, **options):
        """Change settings mid-stream. The lines keep their contents; only what
        the network does to them changes."""
        self._check()
        for name in _SHAPE:
            if name in options:
                raise TypeError("%r is fixed at construction" % (name,))
        self._apply(options)
        self._state.finish()

    def clear(self):
        """Empty every line and every filter."""
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
        # Unlike audiodynamics, everything goes. A reverberation tail is
        # entirely state: a chain restarted with the old tail still in the
        # lines plays the previous take underneath the new one.
        self._state.reset()

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        output = bytearray()
        produced = 0
        width = 2 * self.channel_count
        while produced < FRAMES:
            if not self._pending:
                if self._source is None:
                    break
                result, data = get_buffer(self._source, False, 0)
                data = bytes(data)
                if result == GET_BUFFER_ERROR or len(data) < width:
                    break
                self._pending = data[:len(data) // width * width]
            run = min(FRAMES - produced, len(self._pending) // width)
            output += self._state.process(self._pending[:run * width])
            self._pending = self._pending[run * width:]
            produced += run
        # A starved chain gets silence rather than a short block: this node
        # sits in the middle of a live graph and never reports itself
        # finished. The tail stops with the source -- the lines only advance
        # for frames that arrive -- which is `audioecho.FeedbackDelay`'s
        # behaviour and `audiodelays.Echo`'s before it. A class that wants the
        # tail rung out feeds the tank silence for as long as `tail_samples`
        # says.
        if produced == 0:
            return GET_BUFFER_MORE_DATA, self._publish(
                bytes(FRAMES * 2 * self.channel_count))
        return GET_BUFFER_MORE_DATA, self._publish(output)


__all__ = ("Tank",)
