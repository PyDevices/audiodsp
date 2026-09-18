"""A biquad and a first-order all-pass cascade that reach exact zero.

Not a CircuitPython module, and not from micropython-vst3's engine either --
audioif adds it. `audiofilters.Filter` (over `synthio.Biquad`) and
`audiofilters.Phaser` already do these two jobs, and both are ported
CircuitPython whose arithmetic is integer:

- `synthio.Biquad`'s recursion keeps its output memory in Q12 sample units
  and rounds to nearest, with no dither and no leak, so it can land on a
  state that reproduces itself. Fed silence after a burst, a `LowPass` at
  100 Hz holds +1 LSB, a `LowPass` at 40 Hz q=8 holds -4 LSB and the
  `Phaser` defaults hold -4 LSB -- measured to 3000 blocks, 32 seconds of
  audio, and they never decay (audioif#23).
- `audiofilters.Phaser` has the same defect in its own kernel: its all-pass
  memory is `int16_t` in plain sample units, so it settles on a non-zero
  word and holds it. It also clamps `feedback` to 0.1..0.9, so the
  feedback-free topology every script phaser uses is not reachable and the
  notches stop about 17 dB short of a null.

Neither is modified -- audioif's rule for ported code is extend, never
modify, because an argument on this port's copy of a CircuitPython module
would not exist on a stock board. This module is the extension:

    eq = audiobiquad.Biquad(mode=audiobiquad.PEAKING_EQ, frequency=3200,
                            Q=1.2, gain_db=-4.0, sample_rate=48000)
    eq.play(source)

    phase = audiobiquad.AllPass(stages=4, frequency=sweep, feedback=0.0,
                                mix=0.5, sample_rate=48000)
    phase.play(eq)

`frequency`, `Q`, `gain_db`, `feedback` and `mix` all accept a synthio
`BlockInput` -- an `LFO`, a `Math`, or a plain number -- read once per
processed chunk, exactly as `audiofilters.Filter` reads its own.

Two things this does that the ported nodes cannot:

- **Silence in, silence out.** State is `float`, and any state word below
  1e-20 is written as exact zero, so a tail decays to zero and stays there.
- **`feedback=0.0` is zero**, and negative feedback is allowed. At zero an
  `AllPass` cascade at `mix=0.5` is the script phaser's equal sum, and its
  notches are true nulls.

And one convenience: `AllPass`'s `frequency` really is the frequency at
which one stage's phase passes -90 degrees, because the coefficient is the
bilinear `(tan(pi f / fs) - 1) / (tan(pi f / fs) + 1)`.
`audiofilters.Phaser` uses the small-angle form of that, which lands the
break high enough to matter, so its class has to pre-warp in Python.
"""

from audiocore import (
    GET_BUFFER_ERROR, GET_BUFFER_MORE_DATA, _AudioSample, get_buffer,
)
import _audioif


#: The audioif this was built from, the same pair the native builds put
#: on this module (src/cp_compat/audioif_build.h). audioif#55.
__version__ = _audioif.__version__
__revision__ = _audioif.__revision__

FRAMES = _audioif.FILTER_F32_FRAMES
MAX_STAGES = _audioif.FILTER_F32_MAX_STAGES

#: Filter shapes, numbered exactly as `synthio.FilterMode` numbers them, so
#: `mode=synthio.FilterMode.NOTCH.value` and `mode=audiobiquad.NOTCH` are the
#: same request.
LOW_PASS = 0
HIGH_PASS = 1
BAND_PASS = 2
NOTCH = 3
PEAKING_EQ = 4
LOW_SHELF = 5
HIGH_SHELF = 6

MODES = (LOW_PASS, HIGH_PASS, BAND_PASS, NOTCH, PEAKING_EQ, LOW_SHELF,
         HIGH_SHELF)

#: Option name -> the native configure() slot, in the order
#: shared/audioif_filter_f32.h declares them, which is the order the
#: MicroPython bindings list them in too.
_BIQUAD_OPTIONS = ("mode", "frequency", "Q", "gain_db", "mix")
_ALLPASS_OPTIONS = ("frequency", "feedback", "mix")


def _value(value):
    """Resolve a synthio BlockInput, or pass a plain number through.

    The same helper audiofilters.py uses, and for the same reason: the
    MicroPython bindings call synthio_block_slot_get_limited() at exactly
    this point in the loop, so both targets read a block once per chunk and
    neither evaluates one inside the sample loop.
    """
    evaluator = getattr(value, "_evaluate", None)
    if evaluator is not None:
        from synthio import _BLOCK_TICK
        return float(evaluator(_BLOCK_TICK))
    return float(value.value if hasattr(value, "value") else value)


class _Node(_AudioSample):
    """The pull loop both classes share.

    Deliberately the same shape as `audioecho.FeedbackDelay`'s: fetch a
    source block, process it in runs of at most FRAMES frames, and hand a
    starved chain silence rather than a short block, because this sits in
    the middle of a live graph and never reports itself finished.
    """

    _options = ()

    def _init_format(self, sample_rate, channel_count):
        channel_count = int(channel_count)
        if channel_count not in (1, 2):
            raise ValueError("channel_count must be 1 or 2")
        if int(sample_rate) < 1:
            raise ValueError("sample_rate must be at least 1")
        self.sample_rate = int(sample_rate)
        self.bits_per_sample = 16
        self.channel_count = channel_count
        self.samples_signed = True
        self.single_buffer = False
        self.max_buffer_length = FRAMES * 2 * channel_count
        self._deinited = False
        self._source = None
        self._pending = b""

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

    def clear(self):
        """Zero the recursion. A filter's state is audible: a chain restarted
        with the previous take still in it plays that take's tail over the
        new one."""
        self._check()
        self._state.reset()

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        self._pending = b""
        self._state.reset()

    def _refresh(self):
        """Read every block input once and hand the values to the kernel.

        The kernel clamps, not this: doing it in one place is what keeps the
        four targets identical.
        """
        for slot, name in enumerate(self._options):
            self._state.configure(slot, _value(getattr(self, name)))
        self._state.finish()

    def _apply_blocks(self, frames):
        """Advance the block layer by one chunk, then read it.

        Same point in the loop `audiofilters/Filter.c` reads its own, and the
        MicroPython twin ticks and reads in exactly this order.
        """
        from synthio import _advance_blocks
        _advance_blocks(self.sample_rate, frames)
        self._refresh()

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        width = 2 * self.channel_count
        output = bytearray()
        produced = 0
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
            self._apply_blocks(run)
            output += self._state.process(self._pending[:run * width])
            self._pending = self._pending[run * width:]
            produced += run
        if produced == 0:
            return GET_BUFFER_MORE_DATA, self._publish(bytes(FRAMES * width))
        return GET_BUFFER_MORE_DATA, self._publish(output)


class Biquad(_Node):
    """One RBJ section, float state, exact-zero tail."""

    _options = _BIQUAD_OPTIONS

    def __init__(self, mode=LOW_PASS, frequency=1000.0, Q=0.7071067811865475,
                 gain_db=0.0, mix=1.0, sample_rate=48000, channel_count=2):
        self._init_format(sample_rate, channel_count)
        self.mode = mode
        self.frequency = frequency
        self.Q = Q
        self.gain_db = gain_db
        self.mix = mix
        self._state = _audioif.BiquadF32State(
            sample_rate=self.sample_rate, channel_count=self.channel_count)

    @property
    def mode(self):
        return self._mode

    @mode.setter
    def mode(self, value):
        # Not a BlockInput: it selects the algebra, not a value in it, and a
        # mode that could arrive mid-block would mean recomputing the whole
        # coefficient set from a number nobody chose.
        value = int(value)
        if value not in MODES:
            raise ValueError("mode must be one of audiobiquad.MODES")
        self._mode = value

    @property
    def coefficients(self):
        """`(b0, b1, b2, a1, a2)`, normalized, at the settings in force.

        Reads the block inputs but does not advance them, so looking at this
        does not move an LFO along.
        """
        self._refresh()
        return self._state.coefficients()


class AllPass(_Node):
    """A first-order all-pass cascade with unclamped feedback.

    `stages` is fixed at construction, because it sizes the state. `mix` is a
    crossfade: 0 is a wire, 0.5 is the equal sum of dry and cascade that the
    script phasers make -- and where the notches are deepest -- and 1 is the
    bare cascade, which is magnitude-flat and so sounds like nothing.
    """

    _options = _ALLPASS_OPTIONS

    def __init__(self, stages=4, frequency=1000.0, feedback=0.0, mix=1.0,
                 sample_rate=48000, channel_count=2):
        stages = int(stages)
        if stages < 1 or stages > MAX_STAGES:
            raise ValueError("stages must be between 1 and %d" % MAX_STAGES)
        self._init_format(sample_rate, channel_count)
        self._stages = stages
        self.frequency = frequency
        self.feedback = feedback
        self.mix = mix
        self._state = _audioif.AllPassF32State(
            sample_rate=self.sample_rate, channel_count=self.channel_count,
            stages=stages)

    @property
    def stages(self):
        return self._stages

    @property
    def coefficient(self):
        """The all-pass coefficient at the frequency in force.

        Reads the block inputs but does not advance them, so looking at this
        does not move an LFO along.
        """
        self._refresh()
        return self._state.coefficient()


__all__ = ("AllPass", "Biquad", "LOW_PASS", "HIGH_PASS", "BAND_PASS", "NOTCH",
           "PEAKING_EQ", "LOW_SHELF", "HIGH_SHELF", "MODES")
