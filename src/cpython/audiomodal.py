"""A bank of resonators, which is what a struck object is.

Hit a drum head, a marimba bar, a bell or a wine glass and it rings as a sum
of decaying sinusoids at frequencies that are not harmonics of anything. This
node is that sum: N two-pole resonators fed the same excitation, summed in
float, and quantised once on the way out.

    bank = audiomodal.Bank(modes=9, sample_rate=48000, channel_count=1)
    bank.set_modes(((58.0, 0.55, 1.00), (92.4, 0.24, 0.38),
                    (123.9, 0.15, 0.24), (2200.0, 0.010, 0.30)))
    bank.play(stick)        # a short noise burst -- the stick or beater

`set_mode(index, frequency, decay, gain)` takes the decay as a **60 dB time
in seconds** rather than a Q, and `gain` is the peak of that mode's impulse
response, so a modal table read out of a paper goes in as published without
solving for a filter gain first.

Not a mode on `audiobiquad.Biquad`, and the reason is not the usual one --
`audiobiquad` is already audiodsp's own, so "an argument here would not exist
on a stock board" does not apply. Three things make it a different node:

- **It sums before the quantiser.** Built instead as N band-passes in
  parallel, every one of them quantises to int16 on the way out, so a mode 40
  dB down is carried in about five bits. Measured on a six-mode 58 Hz kick at
  48 kHz with identical excitation: spectral centroid **4113 Hz** out of
  `Biquad` nodes against **73.8 Hz** summed in float. The first is not a
  duller kick, it is noise wearing a kick's envelope.
- **It is parameterised by decay, not Q.** `audiobiquad` clamps Q to 60 and
  says why -- an RBJ section's pole radius goes to 1 as Q rises. A 3 kHz
  cymbal partial ringing for three seconds is Q = 4093, sixty-eight times
  that cap.
- **It skips finished modes.** Every state word flushes to *exact* zero, so
  "this mode has stopped" is a test that cannot be wrong by a fraction of an
  LSB, and a kit holding ten drums resident pays only for the ones sounding.

`audioroute.Splitter` also stops at four taps, so the parallel construction
is not expressible past four modes without a tree of splitters and mixers.
"""

from audiocore import (
    GET_BUFFER_ERROR, GET_BUFFER_MORE_DATA, _AudioSample, get_buffer,
)
import _audiodsp


#: The audiodsp this was built from, the same pair the native builds put
#: on this module (src/cp_compat/audiodsp_build.h). audiodsp#55.
__version__ = _audiodsp.__version__
__revision__ = _audiodsp.__revision__

FRAMES = _audiodsp.MODAL_FRAMES
MAX_MODES = _audiodsp.MODAL_MAX_MODES

#: Decay bounds in MILLISECONDS, as integers, matching the names the
#: MicroPython module exports. `set_mode` takes seconds as a float; these are
#: exact integer milliseconds, so nothing rounds.
MIN_DECAY_MS = 1
MAX_DECAY_MS = 30000

#: Option name -> the native configure() slot, in the order
#: shared/audiodsp_modal.h declares them, which is the order the MicroPython
#: bindings list them in too.
_OPTIONS = {
    "mix": 0,
    "gain": 1,
}


class Bank(_AudioSample):
    def __init__(self, sample_rate=48000, modes=8, **options):
        channel_count = int(options.pop("channel_count", 2))
        if channel_count not in (1, 2):
            raise ValueError("channel_count must be 1 or 2")
        options.pop("sample_rate", None)
        options.pop("modes", None)
        modes = int(modes)
        if modes < 1 or modes > MAX_MODES:
            raise ValueError("modes must be 1 to %d" % (MAX_MODES,))
        self.sample_rate = int(sample_rate)
        self.bits_per_sample = 16
        self.channel_count = channel_count
        self.samples_signed = True
        self.single_buffer = False
        self.max_buffer_length = FRAMES * 2 * channel_count
        self._deinited = False
        self._source = None
        self._pending = b""
        self._modes = modes
        self._state = _audiodsp.ModalState(
            sample_rate=self.sample_rate, channel_count=channel_count,
            modes=modes)
        self._apply(options)
        self._state.finish()

    def _apply(self, options):
        for name, value in options.items():
            slot = _OPTIONS.get(name)
            if slot is None:
                raise TypeError("unknown Bank option %r" % (name,))
            self._state.configure(slot, float(value))

    def set(self, **options):
        """Change settings mid-stream. Modes that are ringing keep ringing."""
        self._check()
        self._apply(options)

    def set_mode(self, index, frequency, decay, gain):
        """One mode, positionally, because that is the order a modal table is
        published in and the order someone reading one will type."""
        self._check()
        index = int(index)
        if index < 0 or index >= self._modes:
            raise IndexError("mode index must be 0 to %d" % (self._modes - 1,))
        self._state.set_mode(index, float(frequency), float(decay),
                             float(gain))

    def set_modes(self, table):
        """The whole table in one call. An instrument changing kit mid-bar
        cannot afford a call per mode."""
        self._check()
        rows = list(table)
        if len(rows) > self._modes:
            raise ValueError("table has %d rows, bank holds %d"
                             % (len(rows), self._modes))
        for index, row in enumerate(rows):
            frequency, decay, gain = row
            self._state.set_mode(index, float(frequency), float(decay),
                                 float(gain))
        # Rows the table did not reach are silenced rather than left holding
        # the last kit's partials, so a shorter table is a smaller drum and
        # not a chord of two.
        for index in range(len(rows), self._modes):
            self._state.set_mode(index, 0.0, 0.001, 0.0)

    def clear(self):
        """Stop every mode ringing at once."""
        self._check()
        self._state.reset()

    @property
    def modes(self):
        return self._modes

    @property
    def ringing(self):
        """True while any mode still holds energy. Exact rather than a
        threshold, because a finished mode is flushed to exact zero."""
        self._check()
        return not self._state.silent()

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
        # Everything goes, for the delay's reason: a bank restarted with the
        # last take's partials still ringing plays the previous hit over the
        # new one.
        self._state.reset()

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        self._state.finish()
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
            output += self._state.process(self._pending[:run * width])
            self._pending = self._pending[run * width:]
            produced += run
        # A starved chain gets silence *through the bank* rather than a short
        # block. Unlike the delay beside it, the tail does keep ringing when
        # the source stops: that is the whole behaviour of a struck object,
        # and a drum whose decay ended the instant the stick left would be the
        # one thing this node exists not to be. Feeding zeros is what rings it
        # out, so the silent frames are pushed through the recursion rather
        # than written over the top of it.
        if produced < FRAMES:
            output += self._state.process(
                bytes((FRAMES - produced) * width))
        return GET_BUFFER_MORE_DATA, self._publish(output)


__all__ = ("Bank",)
