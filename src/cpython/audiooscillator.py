"""A band-limited oscillator whose pitch, FM, pulse width and sync are audio
streams, read sample by sample.

Not a CircuitPython module -- audiodsp adds it. synthio's oscillator is a
wavetable whose pitch moves once per block, with no band limiting, no hard sync
and no linear FM. Everything here acts inside the block:

    osc = audiooscillator.Oscillator(
        frequency=110.0, waveform=audiooscillator.SAW, fm_depth=200.0)
    osc.modulate(fm=lfo, sync=master)
    audio_out.play(osc)

`waveform` is any sum of `SAW`, `PULSE`, `TRIANGLE` and `SINE`; the waveforms
it names are added. A two-channel oscillator puts `waveform_right` on the right
channel from the same phase, so one node hands out two waveforms that stay in
step. `level` is the output for a waveform's full swing, 1.0 being full scale.

The four inputs are any audio sources; only the first channel of each is read,
as a fraction `v` of full scale (-1..1):

* `pitch`: the frequency is `frequency * 2 ** (v * pitch_octaves)`.
* `fm`: plus `v * fm_depth` Hz, linear. It does not go through zero: a
  frequency below 0.1 Hz is held there.
* `pwm`: the pulse width is `pulse_width + v * pwm_depth`, kept in 0.05..0.95.
* `sync`: each rising zero crossing resets the phase. The crossing is placed
  between the two samples it fell between, so a sync is as clean as an edge.

An input that is not connected, or runs dry, counts as zero. Steps are
band-limited with PolyBLEP and the triangle's corners with PolyBLAMP, which
puts the output one sample behind the phase: that is what lets a sync edge,
which nothing can see coming, still correct the sample before it.

`buffer_size` is the size in bytes of each block handed out, as on the
CircuitPython effects, so a host running small blocks gets them without
latency. It defaults to 256 frames, which is also the most it can be.
"""

from audiocore import (
    GET_BUFFER_ERROR, _AudioSample, get_buffer, GET_BUFFER_MORE_DATA,
)
import _audiodsp


#: The audiodsp this was built from, the same pair the native builds put
#: on this module (src/cp_compat/audiodsp_build.h). audiodsp#55.
__version__ = _audiodsp.__version__
__revision__ = _audiodsp.__revision__

FRAMES = _audiodsp.OSCILLATOR_FRAMES

SAW = 1
PULSE = 2
TRIANGLE = 4
SINE = 8

#: Option name -> the native configure() slot, in the order
#: shared/audiodsp_oscillator.h declares them, which is the order the
#: MicroPython bindings list them in too.
_OPTIONS = {
    "frequency": 0,
    "waveform": 1,
    "waveform_right": 2,
    "pulse_width": 3,
    "level": 4,
    "pitch_octaves": 5,
    "fm_depth": 6,
    "pwm_depth": 7,
}

#: The inputs, in the order the native process() takes them.
_INPUTS = ("pitch", "fm", "pwm", "sync")


def _frames_for(buffer_size, channel_count):
    if buffer_size is None:
        return FRAMES
    width = 2 * channel_count
    buffer_size = int(buffer_size)
    if buffer_size < width or buffer_size % width or \
            buffer_size > FRAMES * width:
        raise ValueError(
            "buffer_size must be whole frames, from one to %d" % FRAMES)
    return buffer_size // width


class Oscillator(_AudioSample):
    def __init__(self, sample_rate=48000, channel_count=1, buffer_size=None,
                 pitch=None, fm=None, pwm=None, sync=None, **options):
        channel_count = int(channel_count)
        if channel_count not in (1, 2):
            raise ValueError("channel_count must be 1 or 2")
        self.sample_rate = int(sample_rate)
        self.bits_per_sample = 16
        self.channel_count = channel_count
        self.samples_signed = True
        self.single_buffer = False
        self._frames = _frames_for(buffer_size, channel_count)
        self.max_buffer_length = self._frames * 2 * channel_count
        self._deinited = False
        self._inputs = [None] * 4
        self._channels = [1] * 4
        self._pending = [b""] * 4
        self._state = _audiodsp.OscillatorState(
            sample_rate=self.sample_rate, channel_count=channel_count)
        self._apply(options)
        self._state.finish()
        self._connect({"pitch": pitch, "fm": fm, "pwm": pwm, "sync": sync})

    def _apply(self, options):
        for name, value in options.items():
            slot = _OPTIONS.get(name)
            if slot is None:
                raise TypeError("unknown Oscillator option %r" % (name,))
            self._state.configure(slot, float(value))

    def _connect(self, inputs):
        for name, sample in inputs.items():
            index = _INPUTS.index(name)
            channels = 1
            if sample is not None:
                channels = int(getattr(sample, "channel_count", 1))
                if channels not in (1, 2):
                    raise ValueError("an input must be mono or stereo")
            self._inputs[index] = sample
            self._channels[index] = channels
            self._pending[index] = b""

    def set(self, **options):
        """Change settings mid-stream. The phase carries on; the change
        lands at the next block."""
        self._check()
        self._apply(options)
        self._state.finish()

    def modulate(self, **inputs):
        """Connect inputs by name: `pitch`, `fm`, `pwm`, `sync`. None
        disconnects one; an input not named is left as it is."""
        self._check()
        for name in inputs:
            if name not in _INPUTS:
                raise TypeError("unknown Oscillator input %r" % (name,))
        self._connect(inputs)

    def clear(self):
        """Phase back to zero, with nothing left of the last edge."""
        self._check()
        self._state.reset()

    def _release(self):
        self._inputs = [None] * 4
        self._pending = [b""] * 4

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        # A host reset starts the waveform again from phase zero. The input
        # frames already taken are kept, the rule CircuitPython's effects
        # follow: the inputs were not reset, so dropping them would shift
        # them against each other.
        self._state.reset()

    def _take(self, index):
        result, data = get_buffer(self._inputs[index], False, 0)
        width = 2 * self._channels[index]
        data = bytes(data)
        if result == GET_BUFFER_ERROR or len(data) < width:
            return b""
        return data[:len(data) // width * width]

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        output = bytearray()
        produced = 0
        frames = self._frames
        while produced < frames:
            run = frames - produced
            live = [None] * 4
            for index in range(4):
                if self._inputs[index] is None:
                    continue
                if not self._pending[index]:
                    self._pending[index] = self._take(index)
                    if not self._pending[index]:
                        continue
                width = 2 * self._channels[index]
                run = min(run, len(self._pending[index]) // width)
                live[index] = width
            inputs = []
            for index in range(4):
                width = live[index]
                if width is None:
                    inputs.append(None)
                    continue
                inputs.append((self._pending[index][:run * width],
                               self._channels[index]))
                self._pending[index] = self._pending[index][run * width:]
            output += self._state.process(run, tuple(inputs))
            produced += run
        return GET_BUFFER_MORE_DATA, self._publish(output)


__all__ = ("Oscillator", "SAW", "PULSE", "TRIANGLE", "SINE")
