"""audiobiquad: the claims the parity golden cannot make on its own.

`tests/parity/filter_f32_probe.py` pins what this module renders, and prints
the two invariants as integers so a hash covers them. What it deliberately
does not do is compare the module with the ported kernels it was added
because of -- that needs decibels, and a probe whose output has to be
integer-exact on three interpreters should not be computing logarithms.

So the comparisons live here:

- the float biquad agrees with `audiofilters.Filter` mid-band, which is what
  says it is the same filter and not merely a filter;
- one all-pass stage's phase really does pass -90 degrees at `frequency`,
  where the ported phaser's lands high enough that its class has to pre-warp;
- `feedback=0.0` nulls, and the ported node's 0.1..0.9 clamp cannot get near
  it however its frequency is trimmed;
- and the frozen `synthio.Biquad` kernel has fixed points it parks on, which
  is the measurement audiodsp#23 is about, reproduced here at the kernel so
  the contrast is a fact in the tree rather than a link.

## The traits, with their bars

`docs/correctness-standard.md` is what this file implements for `audiobiquad`:
the module is ours, upstream CircuitPython has no counterpart, so it is held to
three-target agreement (`tests/parity/filter_f32_probe.py`, run by `verify_dsp`)
plus the numeric traits below. Each has an ID so an issue or an evidence pack
can cite one, and each names its bar so nobody has to read the assertion to
learn what is being promised.

| ID | Trait | Bar |
|---|---|---|
| B1 | The float kernel is the same filter as `audiofilters.Filter` mid-band | 1.5 dB across the band |
| B2 | A shelf reaches the gain it was asked for | 0.5 dB |
| B3 | `mix=0` is a bit-exact wire | exact |
| B4 | Every mode's tail reaches exact zero and stays there | exact, inside 600 blocks |
| B5 | An all-pass tail reaches exact zero at every feedback | exact |
| B6 | One all-pass stage passes −90° at `frequency` | 12° |
| B7 | `feedback=0.0` nulls, which the ported clamp cannot reach | 0 against ≥ 1 LSB |
| B8 | Silence in gives exactly zero out, every mode | exact |
| B9 | Full-scale DC passes a low-pass at unity | exact, 0 LSB |
| B10 | `clear()` leaves the node as a freshly built one, every mode | exact |
| B11 | A band-pass peaks at 0 dB at its own centre | 0.05 dB at and above 100 Hz; 0.15 dB at the 20-63 Hz corner |
| B12 | Presents as a sample; a starved node yields silence, not a short block | exact |

**B11's recorded departure is CLOSED (audiodsp#64, 2026-09-09).** It had two
causes and only one of them was the filter. The measurement ended before the
resonator had rung up: the old window reached 2.09 ring-up time constants at
20 Hz / Q 32 and read −1.1474 dB, where `20*log10(1 - exp(-2.09))` is
−1.147 dB — the whole of it. `_peak_db` now derives its window from
`Q*rate/(pi*f0)` and `test_the_short_window_reads_the_ring_up_not_the_filter`
keeps it honest. The filter's own share was real but far smaller: direct form I
differences two nearly-equal large numbers when the poles sit near the unit
circle, and over the 8×7 f0/Q grid at three probe levels it had 7 of 56 cells
outside 0.05 dB, worst +0.2034 dB. Transposed direct form II has 1 of 56, worst
−0.0785 dB, and costs nothing at run time. The one cell left is 31.5 Hz / Q 32
at −0.0899 dB, and it is level-independent, so it is the `float` coefficients
placing the pole pair slightly beside `f0` rather than round-off.

**Where a trait has a control that fails it**, the control is named in the test
rather than left implied: a trait no implementation can fail is not a
measurement. B9's control is `audiofilters.Filter`, which settles a full-scale
DC step at 28520 rather than 32767 - not a defect, but upstream's two-voice
mix-down knee, `(32767 - 28000) * 7151 >> 16 + 28000`, which this module does
not apply. B4's control is the frozen `synthio.Biquad` kernel, whose fixed
points are the subject of audiodsp#23 and are reproduced here.
"""

import math
import unittest
from array import array

import _audiodsp
import audiobiquad
import audiocore
import audiofilters
import synthio

SAMPLE_RATE = 8000

#: One period of a sine at 28000 peak. Stepping by 1/2/4/8/16 gives
#: 125/250/500/1000/2000 Hz at 8 kHz with no rounding anywhere.
SINE64 = tuple(int(round(28000 * math.sin(2 * math.pi * k / 64)))
               for k in range(64))


def tone(step, frames=4096, channels=1, level=28000):
    values = array("h")
    index = 0
    for _frame in range(frames):
        for _channel in range(channels):
            values.append(SINE64[index % 64] * level // 28000)
        index += step
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def silence(frames=2048, channels=1):
    return audiocore.RawSample(array("h", bytes(frames * channels * 2)),
                               sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def peak(node, blocks=16, skip=8):
    """Settled peak absolute sample, the first `skip` blocks discarded."""
    top = 0
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        if index < skip:
            continue
        for position in range(0, len(data), 2):
            word = data[position] | (data[position + 1] << 8)
            if word >= 32768:
                word -= 65536
            top = max(top, abs(word))
    return top


def db(value, reference=28000.0):
    return 20.0 * math.log10(max(value, 1) / reference)


class BiquadLevelTest(unittest.TestCase):
    """The float kernel is the same filter the frozen one is."""

    def _level(self, node):
        return peak(node)

    def test_it_matches_the_ported_filter_across_the_band(self):
        """B1."""
        for step, hertz in ((1, 125.0), (2, 250.0), (4, 500.0),
                            (8, 1000.0), (16, 2000.0)):
            for cutoff in (300.0, 900.0, 2500.0):
                mine = audiobiquad.Biquad(mode=audiobiquad.LOW_PASS,
                                          frequency=cutoff, Q=0.7071067811865475,
                                          sample_rate=SAMPLE_RATE,
                                          channel_count=1)
                mine.play(tone(step))
                theirs = audiofilters.Filter(
                    filter=synthio.Biquad(synthio.FilterMode.LOW_PASS,
                                          frequency=cutoff,
                                          Q=0.7071067811865475),
                    buffer_size=512, sample_rate=SAMPLE_RATE,
                    channel_count=1)
                theirs.play(tone(step))
                difference = db(self._level(mine)) - db(self._level(theirs))
                self.assertLess(
                    abs(difference), 0.5,
                    "%g Hz through a %g Hz low-pass: float %+0.2f dB against "
                    "the ported kernel" % (hertz, cutoff, difference))

    def test_a_shelf_reaches_its_asked_for_gain(self):
        """B2."""
        # 6000 rather than full scale: +9 dB on a 28000 peak is 78,000, and
        # what that would measure is the clamp, not the shelf.
        flat = audiobiquad.Biquad(mode=audiobiquad.LOW_SHELF, frequency=1000.0,
                                  gain_db=0.0, sample_rate=SAMPLE_RATE,
                                  channel_count=1)
        flat.play(tone(1, level=6000))
        lifted = audiobiquad.Biquad(mode=audiobiquad.LOW_SHELF,
                                    frequency=1000.0, gain_db=9.0,
                                    sample_rate=SAMPLE_RATE, channel_count=1)
        lifted.play(tone(1, level=6000))
        gain = db(peak(lifted)) - db(peak(flat))
        self.assertAlmostEqual(gain, 9.0, delta=0.5)

    def test_mix_zero_is_a_wire(self):
        """B3."""
        node = audiobiquad.Biquad(mode=audiobiquad.LOW_PASS, frequency=100.0,
                                  mix=0.0, sample_rate=SAMPLE_RATE,
                                  channel_count=1)
        node.play(tone(8))
        self.assertEqual(peak(node), 28000)


class TailTest(unittest.TestCase):
    """Tier 1: a decaying tail reaches exact zero, and stays there."""

    def _tail(self, node):
        node.play(tone(4, frames=512))
        audiocore.get_buffer(node)
        node.play(silence())
        for index in range(600):
            data = bytes(audiocore.get_buffer(node)[1])
            if data == bytes(len(data)):
                # And it must stay zero, not cross zero on the way past.
                for _again in range(40):
                    more = bytes(audiocore.get_buffer(node)[1])
                    self.assertEqual(more, bytes(len(more)))
                return index
        return None

    def test_every_biquad_mode_reaches_exact_zero(self):
        """B4."""
        for mode in audiobiquad.MODES:
            for frequency, q in ((31.25, 0.7071067811865475), (62.5, 8.0),
                                 (900.0, 2.0)):
                node = audiobiquad.Biquad(mode=mode, frequency=frequency, Q=q,
                                          gain_db=12.0,
                                          sample_rate=SAMPLE_RATE,
                                          channel_count=1)
                self.assertIsNotNone(
                    self._tail(node),
                    "mode %d at %g Hz Q %g never reached zero" % (
                        mode, frequency, q))

    def test_the_all_pass_reaches_exact_zero_at_every_feedback(self):
        """B5."""
        for feedback in (0.0, 0.5, -0.5, 0.95, -0.95):
            node = audiobiquad.AllPass(stages=4, frequency=1139.6,
                                       feedback=feedback, mix=0.5,
                                       sample_rate=SAMPLE_RATE,
                                       channel_count=1)
            self.assertIsNotNone(
                self._tail(node),
                "feedback %g never reached zero" % (feedback,))

    def test_the_frozen_kernel_has_fixed_points_this_one_does_not(self):
        """audiodsp#23, reproduced at the kernel rather than cited.

        Not every trajectory lands on one -- which is why the defect went
        unnoticed -- but a plain DC burst into a low-pass does, and the state
        it lands on reproduces itself for as long as anyone cares to look.

        **The numbers here were 1 and 4 until 2026-09-09, and that was this
        test measuring the wrong kernel.** `BiquadState.process_s16` ran
        audiodsp's widened fixed point at the time, which does park but only on
        a negligible value; audiodsp#77 pointed it at CircuitPython's actual Q15
        arithmetic, which is what the name always claimed. The defect is far
        larger than 1 LSB:

            40 Hz   parks at 16143      100 Hz  parks at 2631
            400 Hz  parks at 160       1000 Hz  parks at 22

        16143 is half of full scale, held forever, from a filter asked for a
        40 Hz low-pass. Measured identically on the CPython extension, desktop
        MicroPython and CircuitPython 10.3.0 (2026-09-09), so it is a fact
        about CircuitPython's kernel and not about one binding. `audiobiquad`,
        whose float state is the point of it, reaches exact zero at every one of
        these -- that is B4 above.

        This is the measurement `docs/upstream-reports/biquad-band-edges.md`
        exists to carry, and it is the cost of holding `synthio.Biquad` to
        CircuitPython's bytes.
        """
        parked = []
        for frequency, expected in ((100.0, 2631), (40.0, 16143)):
            state = _audiodsp.BiquadState()
            state.process_s16(array("h", [30000] * 256).tobytes(), 0,
                              frequency, 0.7071067811865475, 1.0, 48000, 1.0,
                              1)
            zeros = bytes(512)
            for _block in range(3000):
                out = state.process_s16(zeros, 0, frequency,
                                        0.7071067811865475, 1.0, 48000, 1.0, 1)
            word = int.from_bytes(out[0:2], "little")
            if word >= 32768:
                word -= 65536
            parked.append((frequency, word))
            self.assertEqual(abs(word), expected)
        self.assertEqual(len(parked), 2)


class AllPassTest(unittest.TestCase):
    def test_the_break_frequency_is_the_break_frequency(self):
        """B6 - one stage's phase passes -90 degrees at `frequency`.

        `audiofilters.Phaser`'s does not: its coefficient is the small-angle
        form, so its class pre-warps in Python before handing the number over
        (audiocomponents Phaser seed section 4).
        """
        for hertz in (500.0, 1000.0, 2000.0):
            node = audiobiquad.AllPass(stages=1, frequency=hertz,
                                       sample_rate=SAMPLE_RATE,
                                       channel_count=1)
            c = node.coefficient
            omega = 2 * math.pi * hertz / SAMPLE_RATE
            numerator = complex(c + math.cos(omega), -math.sin(omega))
            denominator = complex(1 + c * math.cos(omega),
                                  -c * math.sin(omega))
            phase = math.degrees(math.atan2(
                (numerator / denominator).imag,
                (numerator / denominator).real))
            self.assertAlmostEqual(phase, -90.0, delta=0.2)

    def test_zero_feedback_nulls_and_the_ported_clamp_cannot(self):
        """B7."""
        mine = audiobiquad.AllPass(stages=4, frequency=1139.6, feedback=0.0,
                                   mix=0.5, sample_rate=SAMPLE_RATE,
                                   channel_count=1)
        mine.play(tone(4))
        mine_db = db(peak(mine))
        self.assertLess(mine_db, -40.0)

        # The ported node cannot be asked for zero, so give it every chance:
        # sweep its frequency and take its own best null at its floor.
        best = 1 << 20
        for index in range(60, 160):
            frequency = 1920.0 * index / 100.0
            theirs = audiofilters.Phaser(frequency=frequency, feedback=0.1,
                                         mix=1.0, stages=4, buffer_size=512,
                                         sample_rate=SAMPLE_RATE,
                                         channel_count=1)
            theirs.play(tone(4))
            best = min(best, peak(theirs))
        self.assertGreater(db(best), -30.0)
        self.assertGreater(db(best) - mine_db, 25.0)

    def test_zero_and_the_ported_floor_are_different_settings(self):
        """0.0 and 0.1 render identically on the ported node; not here."""
        levels = []
        for feedback in (0.0, 0.1):
            node = audiobiquad.AllPass(stages=4, frequency=1139.6,
                                       feedback=feedback, mix=0.5,
                                       sample_rate=SAMPLE_RATE,
                                       channel_count=1)
            node.play(tone(4))
            levels.append(peak(node))
        self.assertNotEqual(levels[0], levels[1])

        ported = []
        for feedback in (0.0, 0.1):
            node = audiofilters.Phaser(frequency=1920.0, feedback=feedback,
                                       mix=1.0, stages=4, buffer_size=512,
                                       sample_rate=SAMPLE_RATE,
                                       channel_count=1)
            node.play(tone(4))
            ported.append(peak(node))
        self.assertEqual(ported[0], ported[1])


class SurfaceTest(unittest.TestCase):
    def test_it_presents_itself_as_a_sample(self):
        """B12."""
        node = audiobiquad.Biquad(sample_rate=SAMPLE_RATE)
        self.assertEqual(node.sample_rate, SAMPLE_RATE)
        self.assertEqual(node.channel_count, 2)
        self.assertEqual(node.bits_per_sample, 16)
        self.assertTrue(node.samples_signed)
        self.assertFalse(node.playing)

    def test_a_starved_node_hands_out_silence_not_a_short_block(self):
        """B12."""
        for node in (audiobiquad.Biquad(sample_rate=SAMPLE_RATE),
                     audiobiquad.AllPass(sample_rate=SAMPLE_RATE)):
            result, data = audiocore.get_buffer(node)
            self.assertEqual(result, audiocore.GET_BUFFER_MORE_DATA)
            self.assertEqual(len(bytes(data)), audiobiquad.FRAMES * 4)
            self.assertEqual(bytes(data), bytes(len(bytes(data))))

    def test_bad_arguments_are_refused(self):
        with self.assertRaises(ValueError):
            audiobiquad.Biquad(mode=99, sample_rate=SAMPLE_RATE)
        with self.assertRaises(ValueError):
            audiobiquad.Biquad(channel_count=3, sample_rate=SAMPLE_RATE)
        with self.assertRaises(ValueError):
            audiobiquad.AllPass(stages=0, sample_rate=SAMPLE_RATE)
        with self.assertRaises(ValueError):
            audiobiquad.AllPass(stages=audiobiquad.MAX_STAGES + 1,
                                sample_rate=SAMPLE_RATE)

    def test_a_block_input_is_read_once_per_chunk(self):
        sweep = synthio.LFO(rate=3.0, scale=400.0, offset=900.0)
        node = audiobiquad.AllPass(stages=4, frequency=sweep, feedback=0.2,
                                   mix=0.5, sample_rate=SAMPLE_RATE,
                                   channel_count=1)
        node.play(tone(4))
        seen = set()
        for _block in range(12):
            audiocore.get_buffer(node)
            seen.add(round(node.coefficient, 6))
        self.assertGreater(len(seen), 3)


if __name__ == "__main__":
    unittest.main()


def dc(level, frames=4096, channels=1):
    return audiocore.RawSample(array("h", [level] * (frames * channels)),
                               sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def settled_words(node, blocks=16, skip=10):
    """Every sample of the last `blocks - skip` blocks, signed."""
    out = []
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        if index < skip:
            continue
        for position in range(0, len(data), 2):
            word = data[position] | (data[position + 1] << 8)
            out.append(word - 65536 if word >= 32768 else word)
    return out


class UniversalTraitTest(unittest.TestCase):
    """The traits every node of ours carries, on this one.

    B8, B9 and B10 are the shape `docs/correctness-standard.md` asks of every
    audiodsp-own module. They are here first because `audiobiquad` is what ten
    of the Phase 2 `audioeffects` classes are built on, so a defect here is a
    defect in all of them.
    """

    def _low_pass(self, frequency=200.0, q=0.7071067811865475, **extra):
        return audiobiquad.Biquad(mode=audiobiquad.LOW_PASS,
                                  frequency=frequency, Q=q, mix=1.0,
                                  sample_rate=SAMPLE_RATE, channel_count=1,
                                  **extra)

    def test_silence_in_is_exactly_zero_out_in_every_mode(self):
        """B8. Not the same claim as B4: that one is about a tail draining,
        this one is about a node that was never given anything."""
        for mode in audiobiquad.MODES:
            with self.subTest(mode=mode):
                node = audiobiquad.Biquad(mode=mode, frequency=100.0, Q=8.0,
                                          gain_db=12.0,
                                          sample_rate=SAMPLE_RATE,
                                          channel_count=1)
                node.play(silence())
                for _block in range(8):
                    data = bytes(audiocore.get_buffer(node)[1])
                    self.assertEqual(data, bytes(len(data)))

    def test_B8_reads_real_output_and_not_an_empty_one(self):
        """B8's control. "Every block was zero" is also what a probe that
        pulled nothing would report, so the same read on a signal must not be
        zero. Absence reading as agreement is the failure mode this exists
        against."""
        node = audiobiquad.Biquad(mode=audiobiquad.LOW_PASS, frequency=2000.0,
                                  Q=0.7071067811865475, mix=1.0,
                                  sample_rate=SAMPLE_RATE, channel_count=1)
        node.play(tone(8))
        heard = False
        for _block in range(8):
            data = bytes(audiocore.get_buffer(node)[1])
            self.assertTrue(len(data) > 0)
            heard = heard or data != bytes(len(data))
        self.assertTrue(heard, "the same read that B8 calls silence reports "
                        "silence on a tone as well, so it measures nothing")

    def test_full_scale_dc_passes_a_low_pass_at_unity(self):
        """B9, and it is the trait that catches an overflow.

        A low-pass has unity gain at DC, so a full-scale step must come back
        as itself - exactly, at both rails. "Stays inside int16" would not do
        as a check: an overflowed value wraps to something *in* range, which
        is how the MicroPython flanger's int32 product went unnoticed by a
        matching golden. This is analytic, and any wrap in the recursion
        destroys it by thousands of LSB.
        """
        for level in (32767, -32768, 20000):
            with self.subTest(level=level):
                node = self._low_pass()
                node.play(dc(level))
                words = settled_words(node)
                self.assertEqual(max(abs(word - level) for word in words), 0)

    def test_the_ported_kernel_cannot_hold_B9(self):
        """B9's control: a trait no implementation can fail is not a
        measurement.

        `audiofilters.Filter` settles the same step at 28520, not 32767 -
        4247 LSB out. That is not a defect in it: it applies synthio's
        two-voice mix-down, whose knee is at +/-28000, and
        `(32767 - 28000) * 7151 >> 16 + 28000` is exactly 28520. This module
        does not apply that knee, which is the difference B9 pins.
        """
        ported = audiofilters.Filter(
            filter=synthio.Biquad(synthio.FilterMode.LOW_PASS, 200.0,
                                  Q=0.7071067811865475),
            sample_rate=SAMPLE_RATE, channel_count=1, buffer_size=512)
        ported.play(dc(32767))
        words = settled_words(ported)
        self.assertEqual(words[-1], 28520)
        self.assertGreater(max(abs(word - 32767) for word in words), 4000)

    def test_clear_leaves_the_node_as_a_freshly_built_one(self):
        """B10. A reset that leaves anything behind is a reset that does not
        mean what it says - audiodsp#56 is that defect in `audiodynamics`, and
        this is the trait that would have caught it there."""
        for mode in audiobiquad.MODES:
            with self.subTest(mode=mode):
                used = audiobiquad.Biquad(mode=mode, frequency=100.0, Q=8.0,
                                          sample_rate=SAMPLE_RATE,
                                          channel_count=1)
                used.play(dc(30000))
                for _block in range(6):
                    audiocore.get_buffer(used)
                used.clear()
                used.play(silence())

                fresh = audiobiquad.Biquad(mode=mode, frequency=100.0, Q=8.0,
                                           sample_rate=SAMPLE_RATE,
                                           channel_count=1)
                fresh.play(silence())
                for _block in range(6):
                    self.assertEqual(
                        bytes(audiocore.get_buffer(used)[1]),
                        bytes(audiocore.get_buffer(fresh)[1]))


    def test_B10_discriminates_a_node_that_was_not_cleared(self):
        """B10's control: without `clear()` the two must differ, or the
        comparison would pass for a node that keeps everything."""
        used = audiobiquad.Biquad(mode=audiobiquad.LOW_PASS, frequency=100.0,
                                  Q=8.0, sample_rate=SAMPLE_RATE,
                                  channel_count=1)
        used.play(dc(30000))
        for _block in range(6):
            audiocore.get_buffer(used)
        used.play(silence())          # deliberately NOT cleared

        fresh = audiobiquad.Biquad(mode=audiobiquad.LOW_PASS, frequency=100.0,
                                   Q=8.0, sample_rate=SAMPLE_RATE,
                                   channel_count=1)
        fresh.play(silence())
        differed = any(bytes(audiocore.get_buffer(used)[1])
                       != bytes(audiocore.get_buffer(fresh)[1])
                       for _block in range(6))
        self.assertTrue(differed, "an uncleared node already matches a fresh "
                        "one, so B10 cannot fail and measures nothing")


class BandPassPeakTest(unittest.TestCase):
    """B11, and the departure audiodsp#64 records."""

    #: RBJ's constant-0 dB-peak band-pass has unity gain at its own centre at
    #: every Q. Anything else is the arithmetic, not the design.
    WANTED_DB = 0.0

    #: How many ring-up time constants to discard before measuring. A
    #: resonator approaches its final amplitude as `1 - exp(-t/tau)` with
    #: `tau = Q*rate/(pi*f0)`, so a window that ends early reads the shortfall
    #: and not the filter. 12 leaves 20*log10(1 - exp(-12)) = -0.00005 dB of it,
    #: which is two orders below the tightest bar here.
    SETTLE_TAUS = 12

    @staticmethod
    def _tau(rate, hz, q):
        """Ring-up time constant in samples."""
        return q * rate / (math.pi * hz)

    def _peak_db(self, rate, hz, q, level=20000, settle_taus=None):
        """Peak output at `hz`, in dB relative to the input, once settled.

        **The window is derived from the filter, not fixed.** It used to be a
        flat "render 400 periods, skip the first 120 blocks of 256 and read the
        next 80", and at a low centre with a high Q that ends before the
        resonator has rung up -- 2.09 time constants at 20 Hz / Q 32. The
        reading there was -1.1474 dB, and `20*log10(1 - exp(-2.09))` is
        **-1.147 dB**: the whole of it was incomplete settling, recorded as an
        arithmetic defect (audiodsp#64) for as long as that window stood. The
        same window is 3.30 tau at 31.5 Hz, 6.60 at 63 Hz and 20.9 at 100 Hz,
        and the readings fell away exactly as that suggests.

        The material is generated whole-block and whole-period so the node's
        256-frame chunking cannot clip the window, and `top` is asserted
        non-zero so an exhausted source reads as a failure rather than as a
        filter with no output.
        """
        taus = self.SETTLE_TAUS if settle_taus is None else settle_taus
        frames_per_block = 256
        period = rate / hz
        skip = int(self._tau(rate, hz, q) * taus) + 1
        # At least four periods of peaks, and at least one whole block.
        measure = max(int(period * 4) + 1, frames_per_block)
        blocks_skip = -(-skip // frames_per_block)
        blocks_measure = -(-measure // frames_per_block)
        total = (blocks_skip + blocks_measure) * frames_per_block

        node = audiobiquad.Biquad(mode=audiobiquad.BAND_PASS, frequency=hz,
                                  Q=q, mix=1.0, sample_rate=rate,
                                  channel_count=1)
        values = array("h", (int(level * math.sin(2 * math.pi * hz * frame
                                                  / rate))
                             for frame in range(total)))
        node.play(audiocore.RawSample(values, sample_rate=rate,
                                      channel_count=1))
        top = 0
        for index in range(blocks_skip + blocks_measure):
            data = bytes(audiocore.get_buffer(node)[1])
            if index < blocks_skip:
                continue
            for position in range(0, len(data), 2):
                word = data[position] | (data[position + 1] << 8)
                if word >= 32768:
                    word -= 65536
                top = max(top, abs(word))
        self.assertGreater(top, 0, "no output at %g Hz Q %g -- the source ran "
                           "out before the window, so this measures nothing"
                           % (hz, q))
        return 20.0 * math.log10(top / level)

    def test_it_peaks_at_its_own_centre_from_100_hz_up(self):
        """B11 where the property holds, and the bar is tight because it does:
        measured +0.0009 dB at 100 Hz / Q 16 and -0.0004 dB at 1 kHz / Q 2."""
        for rate, hz, q in ((48000, 100.0, 16.0), (48000, 1000.0, 2.0),
                            (SAMPLE_RATE, 1000.0, 2.0)):
            with self.subTest(rate=rate, hz=hz, q=q):
                self.assertAlmostEqual(self._peak_db(rate, hz, q),
                                       self.WANTED_DB, delta=0.05)

    def test_the_low_corner_holds_too(self):
        """B11 at the corner audiodsp#64 was about. It holds now.

        Two things were wrong there and only one of them was the filter:

        * the measurement ended before the resonator had rung up -- see
          `_peak_db`, and the test below, which pins it;
        * `audiodsp_biquad_f32_process_s16()` was direct form I, whose
          `b0*x0 + b1*x1 + b2*x2 - a1*y1 - a2*y2` is the difference of two
          nearly-equal large numbers when the poles are close to the unit
          circle. Transposed direct form II is not, and costs nothing.

        Measured through the kernel over the 8x7 f0/Q grid at three probe
        levels, worst case over levels: direct form I had **7 of 56** cells
        outside 0.05 dB and a worst of +0.2034 dB; transposed direct form II has
        **1 of 56** and a worst of -0.0785 dB.

        That one cell is 31.5 Hz / Q 32, and it is a different animal:
        -0.0752/-0.0794/-0.0785 dB at levels 23197/5825/2000, i.e. **independent
        of level**, where round-off moves with it. It is the `float`
        coefficients placing the pole pair slightly off, so the true peak sits
        just beside `f0` and reading exactly at `f0` comes in low. No recursion
        form fixes that one; `double` coefficients would. Through the node it
        reads -0.0899 dB and it is the worst cell on the grid, so the bar here
        is 0.15 dB -- loose enough not to be flaky about it, tight enough that
        anything approaching direct form I's +0.2034 dB fails.
        """
        for hz, q in ((20.0, 32.0), (20.0, 16.0), (25.0, 32.0), (31.5, 32.0),
                      (40.0, 32.0), (63.0, 32.0)):
            with self.subTest(hz=hz, q=q):
                self.assertAlmostEqual(self._peak_db(48000, hz, q),
                                       self.WANTED_DB, delta=0.15)

    def test_the_short_window_reads_the_ring_up_not_the_filter(self):
        """The control for `_peak_db`'s window, and the record of audiodsp#64.

        A resonator approaches its final amplitude as `1 - exp(-t/tau)`, so a
        window that ends early reports the shortfall as if it were the filter.
        That is what the old flat window did at 20 Hz / Q 32 -- it ended at 2.09
        time constants, read -1.1474 dB, and `20*log10(1 - exp(-2.09))` is
        -1.147 dB.

        Asserting the SHAPE rather than the exact model, because the measurement
        spans a whole block past the skip point and is therefore settled a
        little more than the nominal figure: short is materially short, longer is
        better, and 12 taus is at unity. Any of those failing means `_peak_db`
        has stopped settling -- which no bar above would notice, because they all
        read through it.
        """
        readings = [self._peak_db(48000, 20.0, 32.0, settle_taus=taus)
                    for taus in (2.0, 3.0, 6.0, 12.0)]
        self.assertLess(readings[0], -0.3,
                        "two time constants no longer reads short, so this "
                        "control proves nothing about the window")
        for earlier, later in zip(readings, readings[1:]):
            self.assertGreater(later, earlier,
                               "a longer window did not read closer to unity: "
                               "%r" % (readings,))
        self.assertAlmostEqual(readings[-1], self.WANTED_DB, delta=0.1)


if __name__ == "__main__":
    unittest.main()
