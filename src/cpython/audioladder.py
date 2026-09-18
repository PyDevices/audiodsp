"""A transistor ladder filter: four one-pole stages round a feedback loop
with an odd saturator inside the loop.

Not a CircuitPython module, and not from micropython-vst3's engine either --
audioif adds it. `audiofilters.Filter` reproduces a resonant low-pass, and
this is not a better one: it is the thing a cascade of biquads cannot be,
because a biquad cascade is linear and a ladder's character is what its
feedback loop's nonlinearity does.

    ladder = audioladder.Ladder(
        sample_rate=48000, cutoff_hz=800, resonance=3.6, drive=2.5)
    ladder.play(source)
    audio_out.play(ladder)

`resonance` is the classic 0..4 feedback: at 4 the loop sustains a tone of
its own, at the cutoff, from silence, and the saturator inside the loop is
what stops that tone growing without bound. Below 4 it is a resonant
low-pass whose peak sharpens as the number rises. `drive` is a linear gain
into the loop, so how hard the saturator is hit -- and therefore how much of
the growl there is -- follows the input level, the way it does in the
circuit. `poles` taps the output from the first, second, third or fourth
stage (6, 12, 18 or 24 dB an octave); the feedback is always the fourth, so
the resonance is the same filter's at every tap. `passband_comp` gives back
the passband the feedback subtracts, 0 for none (the default, which is what
the circuit does) and 1 for all of it. `oversample` runs the loop at twice
the rate so the harmonics the saturator makes fold back less; it is on by
default, because half the cost is the wrong saving on the one node whose job
is to distort. `mix` is a plain crossfade, 0 a wire and 1 the filter.

A module of its own rather than arguments on `audiofilters.Filter`,
deliberately: an argument added to audioif's copy of a CircuitPython module
would not exist on a stock board, so an effect written against it would
silently be a different effect there. This either installs whole or is absent
and says so on import.
"""

from audiocore import (
    GET_BUFFER_ERROR, GET_BUFFER_MORE_DATA, _AudioSample, get_buffer,
)
import _audioif


#: The audioif this was built from, the same pair the native builds put
#: on this module (src/cp_compat/audioif_build.h). audioif#55.
__version__ = _audioif.__version__
__revision__ = _audioif.__revision__

FRAMES = _audioif.LADDER_FRAMES

#: Option name -> the native configure() slot. Kept in the order
#: shared/audioif_ladder.h declares, which is the order the MicroPython
#: bindings list them in too.
_OPTIONS = {
    "cutoff_hz": 0,
    "resonance": 1,
    "drive": 2,
    "poles": 3,
    "passband_comp": 4,
    "oversample": 5,
    "mix": 6,
}


class Ladder(_AudioSample):
    def __init__(self, sample_rate=48000, **options):
        channel_count = int(options.pop("channel_count", 2))
        if channel_count not in (1, 2):
            raise ValueError("channel_count must be 1 or 2")
        options.pop("sample_rate", None)
        self.sample_rate = int(sample_rate)
        self.bits_per_sample = 16
        self.channel_count = channel_count
        self.samples_signed = True
        self.single_buffer = False
        self.max_buffer_length = FRAMES * 2 * channel_count
        self._deinited = False
        self._source = None
        self._pending = b""
        self._state = _audioif.LadderState(
            sample_rate=self.sample_rate, channel_count=channel_count)
        self._apply(options)
        self._state.finish()

    def _apply(self, options):
        for name, value in options.items():
            slot = _OPTIONS.get(name)
            if slot is None:
                raise TypeError("unknown Ladder option %r" % (name,))
            self._state.configure(slot, float(value))

    def set(self, **options):
        """Change settings mid-stream. The integrators keep their charge;
        only what the loop does to them changes. This is the call a macro
        rides on, once a block."""
        self._check()
        self._apply(options)

    def clear(self):
        """Empty the integrators and the solver's history, which also stops a
        self-oscillation."""
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
        # Like audioecho and unlike audiodynamics, everything goes: a
        # self-oscillating filter restarted with its integrators still
        # charged carries the previous take's tone into the new one.
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
        # finished. A self-oscillation stops with the source, for the same
        # reason audioecho's repeats do -- the loop is only advanced by
        # frames that arrive.
        if produced == 0:
            return GET_BUFFER_MORE_DATA, self._publish(
                bytes(FRAMES * 2 * self.channel_count))
        return GET_BUFFER_MORE_DATA, self._publish(output)


__all__ = ("Ladder",)
