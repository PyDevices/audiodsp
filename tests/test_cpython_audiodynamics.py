"""audiodynamics: the traits and the surface the renders cannot check.

`tests/parity/dynamics_probe.py`, `dynamics_options_probe.py` and
`dynamics_extras_probe.py` pin what this module renders across the three
targets. What is left for here is what a render cannot say: the argument forms,
the errors, the source behaviours only a Python-defined audiosample can
produce, and the numeric traits with their bars.

`docs/correctness-standard.md` is what this file implements for
`audiodynamics`: the module is ours - upstream CircuitPython has no
counterpart - so it is held to three-target agreement plus the traits below,
never to a previous version of its own output.

## The traits, with their bars

| ID | Trait | Bar |
|---|---|---|
| D1 | Presents itself as a stereo sample | exact |
| D2 | Keyword order does not change the coefficients | exact |
| D3 | A source that finishes leaves silence behind, not a short block | exact |
| D4 | Gain reduction follows the signal | sign and direction |
| D5 | `stop()` and `play()` are symmetric | exact |
| D6 | A Dynamics can feed another | renders |
| D7 | `gain_smooth_ms` defaults off and is then an identity | exact |
| D8 | Gain smoothing reduces the 50 Hz ripple, monotonically | 6.82 % to 0.41 % at 30 ms |
| D9 | A corrected feedback loop settles at the ratio asked for | 0.01 dB |
| D10 | A corrected loop is still a feedback design | > 1 dB from feed-forward |
| D11 | The corrected slope does not ring | 0.01 dB held, 0.25 dB overshoot |
| D12 | The correction is inert outside COMPRESS and without the detector | exact |
| D13 | `reset()` leaves the node as a freshly built one | exact, 0 LSB |

D8 through D12 arrived with audiodsp#61 and audiodsp#62 and were written the same
day; D1 through D6 were here before the traits had names.

**D13 is why the traits exist at all.** It is audiodsp#56: `reset()` used to keep
the side-chain filter memory, so a reset node was not a fresh one - measured, a
fresh node gated a quiet tone to 0 LSB and the same node after a loud pass and a
reset passed it at 32. The fix is in the shared C, so it moved all three targets
*together* and `verify_dsp` stayed green through it - agreement cannot see a
change that moves every target at once. This trait is what that gate cannot say.
"""

import math
import unittest
from array import array

import audiocore
import audiodynamics

SAMPLE_RATE = 48000
#: What the traits folded in from audiodsp#61 and #62 call it.
RATE = SAMPLE_RATE
CHANNELS = 2


def source(frames=800, level=9000):
    values = array("h")
    for frame in range(frames):
        for channel in range(2):
            values.append(((frame * (61 + channel * 13)) % 2001 - 1000)
                          * level // 1000)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=2)


class Dry:
    """An audiosample that yields one buffer and is then finished."""

    def __init__(self, blocks=1):
        self.sample_rate = SAMPLE_RATE
        self.bits_per_sample = 16
        self.channel_count = 2
        self.samples_signed = True
        self.single_buffer = False
        self.max_buffer_length = 1024
        self._left = blocks

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        pass

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        if self._left <= 0:
            return audiocore.GET_BUFFER_DONE, memoryview(b"")
        self._left -= 1
        block = array("h", (((index * 37) % 2001) - 1000 for index in range(512)))
        return audiocore.GET_BUFFER_MORE_DATA, memoryview(block)

class DynamicsTest(unittest.TestCase):
    def test_it_presents_itself_as_a_stereo_sample(self):
        node = audiodynamics.Dynamics(audiodynamics.DYN_LIMIT,
                                      sample_rate=SAMPLE_RATE)
        self.assertEqual(node.sample_rate, SAMPLE_RATE)
        self.assertEqual(node.channel_count, 2)
        self.assertEqual(node.bits_per_sample, 16)
        self.assertTrue(node.samples_signed)
        self.assertFalse(node.single_buffer)

    def test_an_unknown_option_is_refused(self):
        # A silently ignored typo would be a patch that quietly does nothing.
        with self.assertRaises(TypeError):
            audiodynamics.Dynamics(audiodynamics.DYN_COMPRESS, thresh_db=-12)
        node = audiodynamics.Dynamics(audiodynamics.DYN_COMPRESS)
        with self.assertRaises(TypeError):
            node.set(squash=1.0)

    def test_keyword_order_does_not_change_the_coefficients(self):
        # attack_ms is converted against the sample rate, so a sample_rate
        # written after it still has to win.
        first = audiodynamics.Dynamics(audiodynamics.DYN_COMPRESS,
                                       sample_rate=8000, attack_ms=25.0)
        second = audiodynamics.Dynamics(audiodynamics.DYN_COMPRESS,
                                        attack_ms=25.0, sample_rate=8000)
        first.play(source())
        second.play(source())
        self.assertEqual(bytes(audiocore.get_buffer(first)[1]),
                         bytes(audiocore.get_buffer(second)[1]))

    def test_a_source_that_finishes_leaves_silence_behind(self):
        node = audiodynamics.Dynamics(audiodynamics.DYN_COMPRESS,
                                      sample_rate=SAMPLE_RATE)
        node.play(Dry(blocks=1))
        result, data = audiocore.get_buffer(node)
        self.assertEqual(result, audiocore.GET_BUFFER_MORE_DATA)
        self.assertEqual(len(bytes(data)), 512 * 2)   # the one block it got
        # And then it keeps answering, because the graph around it is running.
        for _ in range(2):
            result, data = audiocore.get_buffer(node)
            self.assertEqual(result, audiocore.GET_BUFFER_MORE_DATA)
            self.assertEqual(bytes(data), bytes(audiodynamics.FRAMES * 4))

    def test_gain_reduction_follows_the_signal(self):
        node = audiodynamics.Dynamics(audiodynamics.DYN_COMPRESS,
                                      sample_rate=SAMPLE_RATE,
                                      threshold_db=-40.0, ratio=8.0)
        self.assertEqual(node.gain_reduction_db(), 0.0)
        node.play(source())
        audiocore.get_buffer(node)
        self.assertLess(node.gain_reduction_db(), -1.0)

    def test_stop_and_play_are_symmetric(self):
        node = audiodynamics.Dynamics(audiodynamics.DYN_GATE)
        self.assertFalse(node.playing)
        node.play(source())
        self.assertTrue(node.playing)
        node.stop()
        self.assertFalse(node.playing)

    def test_a_dynamics_can_feed_another(self):
        first = audiodynamics.Dynamics(audiodynamics.DYN_COMPRESS,
                                       sample_rate=SAMPLE_RATE,
                                       threshold_db=-30.0)
        first.play(source())
        second = audiodynamics.Dynamics(audiodynamics.DYN_LIMIT,
                                        sample_rate=SAMPLE_RATE,
                                        threshold_db=-12.0)
        second.play(first)
        data = bytes(audiocore.get_buffer(second)[1])
        self.assertEqual(len(data), audiodynamics.FRAMES * 4)
        self.assertNotEqual(data, bytes(len(data)))


CHANNELS = 2
TONE_HZ = 50.0
SETTINGS = {"threshold_db": -30.0, "ratio": 6.0, "knee_db": 0.0,
            "attack_ms": 1.0, "release_ms": 10.0}


def tone(hz=TONE_HZ, dbfs=-6.0, cycles=25):
    period = RATE / hz
    assert period == int(period), "choose a frequency that divides the rate"
    amplitude = 32767.0 * (10.0 ** (dbfs / 20.0))
    values = array("h")
    for frame in range(int(period) * cycles):
        sample = int(amplitude * math.sin(2.0 * math.pi * hz * frame / RATE))
        values.append(sample)
        values.append(sample)
    return audiocore.RawSample(values, sample_rate=RATE,
                               channel_count=CHANNELS)


def render(blocks=40, hz=TONE_HZ, **extra):
    node = audiodynamics.Dynamics(audiodynamics.DYN_COMPRESS, sample_rate=RATE,
                                  channel_count=CHANNELS)
    node.set(**SETTINGS)
    if extra:
        node.set(**extra)
    node.play(tone(hz))
    return b"".join(bytes(audiocore.get_buffer(node)[1])
                    for _ in range(blocks))


def left_channel(pcm):
    out = []
    for index in range(0, len(pcm) - 3, 4):
        value = pcm[index] | (pcm[index + 1] << 8)
        out.append(value - 65536 if value >= 32768 else value)
    return out


def thd_percent(samples, hz=TONE_HZ):
    """Total harmonic distortion, harmonics 2..8, on a Hann window."""
    count = len(samples)
    window = [0.5 - 0.5 * math.cos(2.0 * math.pi * i / (count - 1))
              for i in range(count)]
    shaped = [samples[i] * window[i] for i in range(count)]

    def magnitude(frequency):
        real = sum(shaped[i] * math.cos(2.0 * math.pi * frequency * i / RATE)
                   for i in range(count))
        imaginary = sum(shaped[i] * math.sin(2.0 * math.pi * frequency * i
                                             / RATE) for i in range(count))
        return math.hypot(real, imaginary)

    fundamental = magnitude(hz)
    if not fundamental:
        return 0.0
    harmonics = math.sqrt(sum(magnitude(hz * k) ** 2 for k in range(2, 9)))
    return 100.0 * harmonics / fundamental


def ripple(hz=TONE_HZ, **extra):
    """THD of the second half of the render, so the attack is excluded.

    `hz` is passed to **both** the source and the analysis. Not doing that was
    the second bug in this file's first draft: a 1 kHz render measured for
    50 Hz harmonics read 300 %, which looks like a catastrophic node and is a
    catastrophic test.
    """
    samples = left_channel(render(hz=hz, **extra))
    return thd_percent(samples[len(samples) // 2:], hz)


class TheDefaultIsAnIdentity(unittest.TestCase):

    def test_the_material_itself_is_clean(self):
        """The control the first draft of this file lacked. Without it, a
        distorted source would make every reading below meaningless."""
        source = tone()
        pcm = b"".join(bytes(audiocore.get_buffer(source)[1])
                       for _ in range(40))
        samples = left_channel(pcm)
        self.assertLess(thd_percent(samples[len(samples) // 2:]), 0.01)

    def test_omitting_the_keyword_and_passing_zero_render_the_same_bytes(self):
        self.assertEqual(render(), render(gain_smooth_ms=0.0))

    def test_a_negative_time_is_off_too(self):
        self.assertEqual(render(), render(gain_smooth_ms=-5.0))


class TheSmootherReducesTheRipple(unittest.TestCase):

    def test_a_short_release_on_a_low_tone_ripples_without_it(self):
        """The defect, restated as a number so the test below has a baseline
        it cannot silently lose."""
        self.assertGreater(ripple(), 5.0)

    def test_it_falls_monotonically_with_the_setting(self):
        readings = [ripple(gain_smooth_ms=ms)
                    for ms in (0.0, 1.0, 10.0, 30.0)]
        for earlier, later in zip(readings, readings[1:]):
            self.assertLess(later, earlier,
                            "not monotonic: %s" % (readings,))
        # And it reaches the bar the class's trait sets, which is the point.
        self.assertLess(readings[-1], 0.5)

    def test_it_helps_at_1_kHz_as_well_but_matters_less(self):
        """The ripple is a low-frequency problem: at 1 kHz the un-smoothed
        node is already inside the bar, so the option is an improvement rather
        than a rescue. Stated so nobody reads the 50 Hz figures as general."""
        without = ripple(hz=1000.0)
        with_it = ripple(hz=1000.0, gain_smooth_ms=10.0)
        self.assertLess(without, 0.5)
        self.assertLess(with_it, without)


if __name__ == "__main__":
    unittest.main()


CHANNELS = 2
#: 20 dB of overshoot, which is what makes the settled reduction easy to read
#: against the algebra.
THRESHOLD_DB = -40.0
INPUT_DBFS = -20.0
BASE = {"threshold_db": THRESHOLD_DB, "knee_db": 0.0, "attack_ms": 5.0,
        "release_ms": 80.0}


def steady(dbfs=INPUT_DBFS, frames=4096):
    value = int(32767.0 * (10.0 ** (dbfs / 20.0)))
    return audiocore.RawSample(array("h", [value] * (frames * CHANNELS)),
                               sample_rate=RATE, channel_count=CHANNELS)


def settled_reduction(ratio, blocks=40, **extra):
    node = audiodynamics.Dynamics(audiodynamics.DYN_COMPRESS, sample_rate=RATE,
                                  channel_count=CHANNELS)
    node.set(ratio=ratio, **BASE)
    if extra:
        node.set(**extra)
    node.play(steady())
    for _ in range(blocks):
        audiocore.get_buffer(node)
    return -node.gain_reduction_db()


def wanted(ratio, over=20.0):
    """What the ratio asks for: the overshoot times the feed-forward slope."""
    return over * (1.0 - 1.0 / ratio)


def step_trace(ratio=8.0, blocks=24, loud_frames=3000, loud_dbfs=-20.0,
               **extra):
    """A quiet lead, a held loud passage, then quiet again.

    `loud_frames` matters and is why it is a parameter: the node renders 256
    frames per block, so 3000 loud frames is about eleven blocks and the
    passage is over by block fourteen. The ringing test below reads blocks 8-16
    as "while the input is held", which is only true with a longer passage -
    reading them against the short one measured the *release* and called it a
    wobble."""
    values = array("h")
    for dbfs, count in ((-40.0, 600), (loud_dbfs, loud_frames), (-40.0, 3000)):
        sample = int(32767.0 * (10.0 ** (dbfs / 20.0)))
        for _ in range(count):
            values.append(sample)
            values.append(sample)
    node = audiodynamics.Dynamics(audiodynamics.DYN_COMPRESS, sample_rate=RATE,
                                  channel_count=CHANNELS)
    node.set(ratio=ratio, **BASE)
    if extra:
        node.set(**extra)
    node.play(audiocore.RawSample(values, sample_rate=RATE,
                                  channel_count=CHANNELS))
    trace = []
    for _ in range(blocks):
        audiocore.get_buffer(node)
        trace.append(-node.gain_reduction_db())
    return trace


RATIOS = (4.0, 8.0, 12.0, 20.0)


class TheUncorrectedLoopMissesTheRatio(unittest.TestCase):
    """The defect, as a number, so the fix below has a baseline it cannot
    quietly lose."""

    def test_every_ratio_lands_near_two_to_one(self):
        for ratio in RATIOS:
            with self.subTest(ratio=ratio):
                reading = settled_reduction(ratio, feedback_detector=1)
                self.assertLess(reading, 10.0)
                self.assertLess(reading, wanted(ratio) - 4.0)


class TheCorrectedLoopLandsOnTheRatio(unittest.TestCase):

    def test_it_settles_where_the_algebra_says(self):
        for ratio in RATIOS:
            with self.subTest(ratio=ratio):
                self.assertAlmostEqual(
                    settled_reduction(ratio, feedback_detector=1,
                                      feedback_gain_corrected=1),
                    wanted(ratio), delta=0.01)

    def test_it_is_still_a_feedback_design(self):
        """The guard on the cancelling implementation. Same steady state as
        feed-forward, different path to it - if this ever falls to a fraction
        of a dB, the loop has been cancelled rather than inverted."""
        forward = step_trace()
        corrected = step_trace(feedback_detector=1,
                               feedback_gain_corrected=1)
        apart = max(abs(a - b) for a, b in zip(forward, corrected))
        self.assertGreater(apart, 1.0, "corrected feedback is tracking "
                           "feed-forward to %.4f dB, which means the loop was "
                           "cancelled, not inverted" % apart)

    def test_it_does_not_ring(self):
        """A slope of R-1 is steep. Overshoot past the settled value, or a
        wobble while the input is held, is what instability would look like."""
        for ratio in (4.0, 20.0, 100.0):
            with self.subTest(ratio=ratio):
                trace = step_trace(ratio=ratio, blocks=30,
                                   loud_frames=6000, loud_dbfs=-10.0,
                                   feedback_detector=1,
                                   feedback_gain_corrected=1)
                hold = trace[8:16]
                self.assertLess(max(hold) - min(hold), 0.01)
                self.assertLess(max(trace[:16]) - trace[14], 0.25)


class ItsScopeIsNarrow(unittest.TestCase):
    """Three claims the fixtures capture but cannot compare, asserted here."""

    def _render(self, mode, blocks=4, **options):
        node = audiodynamics.Dynamics(mode, sample_rate=RATE,
                                      channel_count=CHANNELS)
        node.set(**options)
        node.play(steady(dbfs=-12.0))
        return b"".join(bytes(audiocore.get_buffer(node)[1])
                        for _ in range(blocks))

    def test_it_is_inert_without_the_feedback_detector(self):
        self.assertEqual(
            self._render(audiodynamics.DYN_COMPRESS, ratio=8.0, **BASE),
            self._render(audiodynamics.DYN_COMPRESS, ratio=8.0,
                         feedback_gain_corrected=1, **BASE))

    def test_it_changes_nothing_outside_compress(self):
        for name, mode, extra in (
                ("limit", audiodynamics.DYN_LIMIT, {"threshold_db": -26.0}),
                ("expand", audiodynamics.DYN_EXPAND,
                 {"threshold_db": -20.0, "ratio": 3.0}),
                ("gate", audiodynamics.DYN_GATE, {"threshold_db": -22.0}),
                ("transient", audiodynamics.DYN_TRANSIENT,
                 {"attack_gain_db": 9.0, "sustain_gain_db": -7.0})):
            with self.subTest(mode=name):
                self.assertEqual(
                    self._render(mode, feedback_detector=1, **extra),
                    self._render(mode, feedback_detector=1,
                                 feedback_gain_corrected=1, **extra))

    def test_it_does_change_compress_with_the_detector_on(self):
        """The control for the two above: they would all pass if the option
        did nothing at all."""
        self.assertNotEqual(
            self._render(audiodynamics.DYN_COMPRESS, ratio=8.0,
                         feedback_detector=1, **BASE),
            self._render(audiodynamics.DYN_COMPRESS, ratio=8.0,
                         feedback_detector=1, feedback_gain_corrected=1,
                         **BASE))


if __name__ == "__main__":
    unittest.main()


class ResetTest(unittest.TestCase):
    """D13 - audiodsp#56. A reset that leaves anything behind is a reset that
    does not mean what it says."""

    THRESHOLD_DB = -39.7
    QUIET_DBFS = -60.0

    def _gate(self):
        node = audiodynamics.Dynamics(audiodynamics.DYN_GATE,
                                      sample_rate=SAMPLE_RATE,
                                      channel_count=CHANNELS)
        node.set(threshold_db=self.THRESHOLD_DB, attack_ms=0.01,
                 release_ms=100.0, depth_db=-80.0, sidechain_hz=120.0)
        return node

    def _tone(self, dbfs, frames=1024):
        amplitude = 32767.0 * (10.0 ** (dbfs / 20.0))
        values = array("h")
        for frame in range(frames):
            sample = int(amplitude * math.sin(2.0 * math.pi * 440.0 * frame
                                              / SAMPLE_RATE))
            values.append(sample)
            values.append(sample)
        return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                                   channel_count=CHANNELS)

    def _first_peak(self, node):
        data = bytes(audiocore.get_buffer(node)[1])
        top = 0
        for position in range(0, len(data), 2):
            word = data[position] | (data[position + 1] << 8)
            if word >= 32768:
                word -= 65536
            top = max(top, abs(word))
        return top

    def test_a_reset_node_gates_a_quiet_tone_like_a_fresh_one(self):
        """D13. The gate's own condition: a tone 20 dB under the threshold
        should be shut out. It was, on a fresh node, and was passed at full
        level on a reset one - the stale high-pass state kept the detector's
        level up and held the gate open."""
        fresh = self._gate()
        fresh.play(self._tone(self.QUIET_DBFS))
        self.assertEqual(self._first_peak(fresh), 0)

        used = self._gate()
        used.play(self._tone(0.0))
        for _block in range(8):
            audiocore.get_buffer(used)
        audiocore.reset_buffer(used)
        used.play(self._tone(self.QUIET_DBFS))
        self.assertEqual(self._first_peak(used), 0)

    def test_D13_reads_a_gate_that_can_open(self):
        """D13's control. "The output was zero" is also true of a gate that
        never opens for anything, so the same node on a loud tone must pass
        it."""
        node = self._gate()
        node.play(self._tone(0.0))
        self.assertGreater(self._first_peak(node), 1000)

    def test_the_reported_gain_reduction_resets_too(self):
        """D13's other half: a fresh node reports no reduction, so a reset one
        must not still be reporting the last one it made."""
        node = audiodynamics.Dynamics(audiodynamics.DYN_COMPRESS,
                                      sample_rate=SAMPLE_RATE,
                                      channel_count=CHANNELS)
        node.set(threshold_db=-40.0, ratio=8.0, knee_db=0.0, attack_ms=1.0,
                 release_ms=50.0)
        node.play(self._tone(-6.0))
        for _block in range(8):
            audiocore.get_buffer(node)
        self.assertLess(node.gain_reduction_db(), -1.0)
        audiocore.reset_buffer(node)
        self.assertEqual(node.gain_reduction_db(), 0.0)


class UniversalTraitTest(unittest.TestCase):
    """D14 and D15 - the traits every audiodsp-own node carries."""

    def _alternating(self, frames=4096, level=32767):
        values = array("h")
        for frame in range(frames):
            for _channel in range(CHANNELS):
                values.append(level if frame % 2 else -level)
        return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                                   channel_count=CHANNELS)

    def _alternating_words(self, count, level=32767):
        return [level if (index // CHANNELS) % 2 else -level
                for index in range(count)]

    def _words(self, node, blocks):
        out = []
        for _block in range(blocks):
            data = bytes(audiocore.get_buffer(node)[1])
            for position in range(0, len(data), 2):
                word = data[position] | (data[position + 1] << 8)
                out.append(word - 65536 if word >= 32768 else word)
        return out

    def test_a_threshold_above_the_signal_is_unity_gain(self):
        """D14, this module's form of the identity trait.

        Nothing crosses the threshold, so the gain computer returns 0 dB and the
        node must hand back exactly what it was given - through the detector,
        the envelope and the multiply, at +/-32767, which is where an
        arithmetic width error shows and a range check does not.
        """
        for mode in (audiodynamics.DYN_COMPRESS, audiodynamics.DYN_LIMIT):
            with self.subTest(mode=mode):
                node = audiodynamics.Dynamics(mode, sample_rate=SAMPLE_RATE,
                                              channel_count=CHANNELS)
                node.set(threshold_db=0.0, ratio=4.0, knee_db=0.0,
                         attack_ms=0.01, release_ms=1.0)
                node.play(self._alternating())
                rendered = self._words(node, 6)
                self.assertEqual(rendered,
                                 self._alternating_words(len(rendered)))

    def test_D14_discriminates(self):
        """D14's control: a threshold *under* the signal must not satisfy it."""
        node = audiodynamics.Dynamics(audiodynamics.DYN_COMPRESS,
                                      sample_rate=SAMPLE_RATE,
                                      channel_count=CHANNELS)
        node.set(threshold_db=-40.0, ratio=8.0, knee_db=0.0, attack_ms=0.01,
                 release_ms=1.0)
        node.play(self._alternating())
        rendered = self._words(node, 6)
        self.assertNotEqual(rendered, self._alternating_words(len(rendered)))

    def test_silence_in_is_exactly_zero_out_in_every_mode(self):
        """D15. A gate at its floor still outputs zero on zero, and a
        transient shaper has no transient to find in silence."""
        for mode in (audiodynamics.DYN_COMPRESS, audiodynamics.DYN_LIMIT,
                     audiodynamics.DYN_EXPAND, audiodynamics.DYN_GATE,
                     audiodynamics.DYN_TRANSIENT):
            with self.subTest(mode=mode):
                node = audiodynamics.Dynamics(mode, sample_rate=SAMPLE_RATE,
                                              channel_count=CHANNELS)
                node.play(audiocore.RawSample(
                    array("h", bytes(4096 * CHANNELS * 2)),
                    sample_rate=SAMPLE_RATE, channel_count=CHANNELS))
                for _block in range(6):
                    data = bytes(audiocore.get_buffer(node)[1])
                    self.assertEqual(data, bytes(len(data)))
