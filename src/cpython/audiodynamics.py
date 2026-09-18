"""Envelope-follower dynamics processing: compression, limiting, downward
expansion, gating and transient shaping.

Unlike the rest of this package, `audiodynamics` is not a CircuitPython module.
It comes from micropython-vst3's `vstaudio` engine, which grew it for its
effects library, and it is provided here for MicroPython, CPython and patched
CircuitPython alike.

    node = audiodynamics.Dynamics(
        audiodynamics.DYN_COMPRESS, sample_rate=48000,
        threshold_db=-24, ratio=4, knee_db=6, attack_ms=10, release_ms=120)
    node.play(source)          # any audiosample: 16-bit signed stereo
    audio_out.play(node)

`sidechain_hz` high-passes the detector without touching the audio, which is
how the de-essers in the effects library are built. `gain_reduction_db()`
reports what the last processed frame was reduced by, for a meter.

`lookahead_ms` holds the audio back while the detector reads ahead of it, so
the gain is already down by the time the peak arrives - the difference
between a brickwall limiter that catches transients and one that overshoots
them. It is latency the whole chain pays, capped at 50 ms, and it costs a
buffer that is allocated only if asked for. `true_peak=True` adds the peak
*between* samples to what the detector sees, which is where an inter-sample
over hides. Both default off: a Dynamics built without them is the node
exactly as it was.

Twenty-one further options came with the effects program, every one of them
default-off and every one of them a fixed trait of a named circuit that
tuning the shipped knobs cannot reach. See docs/upstream-diff.md for the
measurements; briefly:

* ``detector="rms"`` with ``rms_ms`` averages a mean square instead of
  rectifying a peak, so a sine and a square of equal RMS get the same gain.
* ``feedback_detector=True`` reads the frame the node last put out rather
  than this frame's input - the side chain tapped after the gain cell, as a
  1176, an LA-2A and a Fairchild all do. ``key(sample)`` feeds the detector
  from a different stream entirely, and ``key_listen=True`` puts the
  detector's own signal on the output.
* ``true_peak=2`` replaces the half-band estimate with a 4x polyphase
  reconstruction - four phases of twelve taps, BS.1770 Annex 2's shape,
  though not its coefficients - which reads a worst-phase tone within
  0.26 dB where the estimate under-reads by 1.24 dB at a quarter of the
  sample rate.
* ``sidechain_lp_hz`` puts the top on the key band ``sidechain_hz`` opened
  the bottom of, and ``sidechain_poles=2`` cascades a second pole through
  both, taking the skirt from 6 to 12 dB/octave.
* ``depth_db`` replaces the expander's fixed -60 dB and the gate's fixed
  -80 dB floors. Positive means unset, because a depth is an attenuation.
* ``hold_ms`` (with ``hysteresis_db``) swaps the gate's memoryless gain
  computer for a closed/attack/hold/decay machine: once the attack starts it
  runs to full open whatever the key does next, and holds there.
* ``relative_threshold=True`` drives the gain computer with the side-chained
  level minus the full-band level, so a fixed spectral balance gets the same
  reduction wherever it sits in the level range.
* ``program_attack=True`` scales the attack coefficient by the overshoot, so
  20 dB over is caught about three times faster than 10 dB over.
* ``transient_fast_attack_ms``, ``transient_fast_release_ms``,
  ``transient_slow_attack_ms`` and ``transient_slow_release_ms`` expose the
  transient shaper's four detector time constants, which were literals.
  ``slow_hold_ms`` makes the slow envelope a peak-hold, and
  ``transient_dual=True`` runs a second, slower pair
  (``sustain_fast_attack_ms`` and its three siblings) for the sustain
  section so both differences apply at once instead of one being selected.
"""

from audiocore import (
    GET_BUFFER_ERROR, GET_BUFFER_MORE_DATA, _AudioSample, get_buffer,
)
import _audioif


#: The audioif this was built from, the same pair the native builds put
#: on this module (src/cp_compat/audioif_build.h). audioif#55.
__version__ = _audioif.__version__
__revision__ = _audioif.__revision__

DYN_COMPRESS = 0
DYN_LIMIT = 1
DYN_EXPAND = 2
DYN_GATE = 3
DYN_TRANSIENT = 4

#: Option name -> the native configure() slot. Kept in the order
#: shared/audioif_dynamics.h declares, which is the order the MicroPython
#: bindings list them in too.
_OPTIONS = {
    "threshold_db": 0,
    "ratio": 1,
    "knee_db": 2,
    "makeup_db": 3,
    "attack_ms": 4,
    "release_ms": 5,
    "attack_gain_db": 6,
    "sustain_gain_db": 7,
    "sidechain_hz": 8,
    "lookahead_ms": 9,
    "true_peak": 10,
    # The effects program's additions. New names go on the end; these numbers
    # are the C enum's, and the C enum only ever grows.
    "transient_fast_attack_ms": 11,
    "transient_fast_release_ms": 12,
    "transient_slow_attack_ms": 13,
    "transient_slow_release_ms": 14,
    "detector": 15,
    "rms_ms": 16,
    "feedback_detector": 17,
    "sidechain_lp_hz": 18,
    "sidechain_poles": 19,
    "key_listen": 20,
    "depth_db": 21,
    "hold_ms": 22,
    "hysteresis_db": 23,
    "relative_threshold": 24,
    "program_attack": 25,
    "transient_dual": 26,
    "sustain_fast_attack_ms": 27,
    "sustain_fast_release_ms": 28,
    "sustain_slow_attack_ms": 29,
    "sustain_slow_release_ms": 30,
    "slow_hold_ms": 31,
    "gain_smooth_ms": 32,
    "feedback_gain_corrected": 33,
}

#: `detector=` reads better as a word than as a number, and the C option is a
#: float like every other, so the word is mapped here.
_DETECTORS = {"peak": 0.0, "rms": 1.0}

FRAMES = _audioif.DYNAMICS_FRAMES


class Dynamics(_AudioSample):
    def __init__(self, mode=DYN_COMPRESS, **options):
        sample_rate = int(options.pop("sample_rate", 48000))
        channel_count = int(options.pop("channel_count", 2))
        if channel_count not in (1, 2):
            raise ValueError("channel_count must be 1 or 2")
        self.sample_rate = sample_rate
        self.bits_per_sample = 16
        self.channel_count = channel_count
        self.samples_signed = True
        self.single_buffer = False
        self.max_buffer_length = FRAMES * 2 * channel_count
        self._deinited = False
        self._source = None
        self._pending = b""
        self._key_source = None
        self._key_pending = b""
        self._state = _audioif.DynamicsState(mode=int(mode),
                                             sample_rate=sample_rate,
                                             channel_count=channel_count)
        self._apply(options)
        # Only now do the unset attack/release times fall back to their
        # defaults - see shared/audioif_dynamics.h for why that is a separate
        # step rather than part of the initial state.
        self._state.finish()

    def _apply(self, options):
        rate = options.pop("sample_rate", None)
        if rate is not None:
            # First, whatever order the caller wrote them: the millisecond
            # options are converted against it.
            self.sample_rate = int(rate)
            self._state.set_sample_rate(self.sample_rate)
        for name, value in options.items():
            slot = _OPTIONS.get(name)
            if slot is None:
                raise TypeError("unknown Dynamics option %r" % (name,))
            if name == "detector" and isinstance(value, str):
                if value not in _DETECTORS:
                    raise ValueError("detector must be 'peak' or 'rms', not "
                                     "%r" % (value,))
                value = _DETECTORS[value]
            # `true_peak` used to be forced through bool() here. It is a level
            # now (0, 1 or 2), and True still lands on 1.
            self._state.configure(slot, float(value))

    def set(self, **options):
        """Change settings mid-stream. The detector keeps its memory."""
        self._check()
        self._apply(options)

    def gain_reduction_db(self):
        """Gain applied to the most recent frame, in dB (negative = cut)."""
        return self._state.gain_reduction_db

    @property
    def playing(self):
        return self._source is not None

    def play(self, sample, *, loop=False):
        self._check()
        self._source = sample
        self._pending = b""

    def key(self, sample):
        """Feed the detector from `sample` instead of from the audio.

        The gain still lands on whatever `play()` is playing. Pass None to go
        back to reading the audio. A key that runs dry starves the node the
        same way an absent source does: silence, never a short block.
        """
        self._check()
        self._key_source = sample
        self._key_pending = b""

    def stop(self):
        self._source = None
        self._pending = b""
        self._key_pending = b""

    def _release(self):
        self.stop()

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        self._pending = b""
        self._key_pending = b""
        # `_state.reset()` is `audioif_dynamics_reset` in the shared C, so
        # this target and the two native ones drop the same things: every
        # thing the detector remembers, including the side-chain filter
        # memory and the reported gain reduction, which used to survive
        # (audioif#56). What is kept is configuration.
        self._state.reset()

    def _take_key(self, wanted):
        """As many key bytes as are on hand, pulling more if the source has
        them. Short means the key ran dry."""
        while len(self._key_pending) < wanted:
            result, data = get_buffer(self._key_source, False, 0)
            data = bytes(data)
            if result == GET_BUFFER_ERROR or not data:
                break
            self._key_pending += data
        taken = self._key_pending[:wanted]
        self._key_pending = self._key_pending[len(taken):]
        return taken

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
            key = b""
            if self._key_source is not None:
                key = self._take_key(run * width)
                run = len(key) // width
                if run == 0:
                    break
                key = key[:run * width]
            block = self._pending[:run * width]
            output += (self._state.process(block, key) if key
                       else self._state.process(block))
            self._pending = self._pending[run * width:]
            produced += run
        # A starved chain gets silence rather than a short block: this node
        # sits in the middle of a live graph and never reports itself finished.
        if produced == 0:
            return GET_BUFFER_MORE_DATA, self._publish(
                bytes(FRAMES * 2 * self.channel_count))
        return GET_BUFFER_MORE_DATA, self._publish(output)


__all__ = ("Dynamics", "DYN_COMPRESS", "DYN_LIMIT", "DYN_EXPAND", "DYN_GATE",
           "DYN_TRANSIENT")
