"""audioshaper: the claims the parity goldens cannot carry.

tests/parity/waveshaper_probe.py and samplehold_probe.py pin what these two
nodes *render*, byte for byte, on every interpreter. What they cannot say is
whether the bytes are the right ones. These are the measurements the module
was asked for -- does oversampling actually lower the alias floor, does the
hysteresis option actually enclose an area and does that area grow with
drive, is the hold's ratio exact over a distance where a fixed-point one has
visibly walked, does a hard-clipping curve's own headroom knee sit where
`CLIP_HEADROOM` says it does -- each with the control that would go red if
the mechanism were absent.
"""

import math
import unittest
from array import array

import audiocore
import audioshaper

SAMPLE_RATE = 48000
POINTS = 1024
Q15 = 32768


def clamp15(value):
    return max(-32768, min(32767, value))


def positions():
    last = POINTS - 1
    return [(2 * index - last) * Q15 // last for index in range(POINTS)]


def cubic_curve():
    """y = 1.5x - 0.5x^3, in integers so every interpreter builds it alike."""
    return array("h", [clamp15((3 * x * Q15 * Q15 - x * x * x) //
                               (2 * Q15 * Q15)) for x in positions()])


CUBIC = cubic_curve()


def hard_clip_curve(threshold=0.10, points=2048):
    """A straight hard clip: linear to the rails over +-`threshold` of the
    input span, flat beyond it -- unlike CUBIC, which never has a flat top
    at all. Driven hard, this is the shape audiodsp#99 is about."""
    last = points - 1
    return array("h", [
        clamp15(int(round(max(-1.0, min(1.0, (
            -1.0 + 2.0 * index / last) / threshold)) * 32767)))
        for index in range(points)])


HARD_CLIP = hard_clip_curve()


def render(values, curve=CUBIC, **options):
    """Push one array of interleaved frames through a node and take it back."""
    node = audioshaper.Waveshaper(sample_rate=SAMPLE_RATE, curve=curve,
                                 **options)
    node.play(audiocore.RawSample(
        values, sample_rate=SAMPLE_RATE,
        channel_count=options.get("channel_count", 2)))
    channels = options.get("channel_count", 2)
    out = bytearray()
    frames = len(values) // channels
    while len(out) < frames * 2 * channels:
        out += bytes(audiocore.get_buffer(node)[1])
    taken = array("h")
    taken.frombytes(bytes(out[:frames * 2 * channels]))
    return taken


def sine(cycles, frames, level):
    values = array("h")
    for index in range(frames):
        sample = int(round(level *
                           math.sin(2.0 * math.pi * cycles * index / frames)))
        values.append(clamp15(sample))
        values.append(clamp15(sample))
    return values


def bin_energy(samples, cycles, frames):
    """|X[k]|^2 by Goertzel, k = cycles, exactly on a bin."""
    angle = 2.0 * math.pi * cycles / frames
    coefficient = 2.0 * math.cos(angle)
    first = second = 0.0
    for value in samples:
        current = value + coefficient * first - second
        second = first
        first = current
    real = first - second * math.cos(angle)
    imaginary = second * math.sin(angle)
    return real * real + imaginary * imaginary


def alias_floor_db(oversample, cycles=173, frames=8192, level=16000,
                   **options):
    """Non-harmonic energy relative to the fundamental, in dB.

    `cycles` is a whole number of cycles in the window, so every harmonic and
    every alias lands on its own bin and a rectangular window leaks nothing.
    Harmonics are h*cycles while that is still under Nyquist; everything
    above folds, and where it lands is an alias, which is the distinction the
    measurement exists to make.
    """
    settle = frames
    total = settle + frames
    values = array("h")
    for index in range(total):
        sample = int(round(level * math.sin(
            2.0 * math.pi * cycles * index / frames)))
        values.append(clamp15(sample))
        values.append(clamp15(sample))
    rendered = render(values, oversample=oversample, **options)
    left = [float(rendered[index * 2]) for index in range(settle, total)]
    mean = sum(left) / len(left)
    left = [value - mean for value in left]
    energy = sum(value * value for value in left)
    harmonics = 0.0
    fundamental = 0.0
    harmonic = 1
    while harmonic * cycles < frames // 2:
        power = 2.0 * bin_energy(left, harmonic * cycles, frames) / frames
        if harmonic == 1:
            fundamental = power
        harmonics += power
        harmonic += 1
    alias = max(energy - harmonics, 1e-12)
    return 10.0 * math.log10(alias / fundamental)


def loop_area(oversample, pre_gain, cycles=4, frames=9600, level=20000,
              **options):
    """Normalised area the input/output trajectory encloses, by shoelace.

    A static table's trajectory retraces itself, so it encloses exactly
    nothing -- which is what makes this the measurement that separates a
    curve with memory from one without.
    """
    values = array("h")
    period = frames // cycles
    for index in range(frames):
        phase = (index % period) / float(period)
        triangle = 4.0 * phase - 1.0 if phase < 0.5 else 3.0 - 4.0 * phase
        sample = clamp15(int(round(level * triangle)))
        values.append(sample)
        values.append(sample)
    rendered = render(values, oversample=oversample, pre_gain=pre_gain,
                      **options)
    start = frames - period
    x = [float(values[index * 2]) for index in range(start, frames)]
    y = [float(rendered[index * 2]) for index in range(start, frames)]
    area = 0.0
    for index in range(len(x)):
        nxt = (index + 1) % len(x)
        area += x[index] * y[nxt] - x[nxt] * y[index]
    area = abs(area) * 0.5
    span_x = max(x) - min(x)
    span_y = max(y) - min(y)
    if span_x <= 0.0 or span_y <= 0.0:
        return 0.0
    return area / (span_x * span_y)


class SurfaceTest(unittest.TestCase):
    def test_it_presents_itself_as_a_stereo_sample(self):
        node = audioshaper.Waveshaper(sample_rate=SAMPLE_RATE, curve=CUBIC)
        self.assertEqual(node.sample_rate, SAMPLE_RATE)
        self.assertEqual(node.channel_count, 2)
        self.assertEqual(node.bits_per_sample, 16)
        self.assertTrue(node.samples_signed)
        self.assertEqual(node.oversample, 4)

    def test_a_curve_is_required(self):
        with self.assertRaises(ValueError):
            audioshaper.Waveshaper(sample_rate=SAMPLE_RATE)

    def test_oversample_is_a_power_of_two_up_to_eight(self):
        for factor in (0, 3, 5, 16):
            with self.assertRaises(ValueError):
                audioshaper.Waveshaper(sample_rate=SAMPLE_RATE, curve=CUBIC,
                                       oversample=factor)

    def test_an_unknown_option_is_refused(self):
        with self.assertRaises(TypeError):
            audioshaper.Waveshaper(sample_rate=SAMPLE_RATE, curve=CUBIC,
                                   drive=3.0)

    def test_a_starved_chain_gets_silence_not_a_short_block(self):
        node = audioshaper.Waveshaper(sample_rate=SAMPLE_RATE, curve=CUBIC)
        result, block = audiocore.get_buffer(node)
        self.assertEqual(result, audiocore.GET_BUFFER_MORE_DATA)
        self.assertEqual(len(bytes(block)), audioshaper.FRAMES * 4)
        self.assertEqual(sum(bytes(block)), 0)

    def test_mix_zero_is_the_untouched_input(self):
        values = sine(37, 2048, 12000)
        rendered = render(values, oversample=8, pre_gain=40.0, mix=0.0)
        self.assertEqual(list(rendered), list(values))


class AliasFloorTest(unittest.TestCase):
    """Oversampling lowers the alias floor, and the base rate is the control.

    Everything here is one measurement at four settings of one argument. If
    the interpolate/shape/decimate path were not doing what it says, x4 and
    x8 would read the same as x1, which is exactly what the palette's
    audiofilters.Distortion does read.
    """

    def test_the_floor_falls_as_the_factor_rises(self):
        floors = [alias_floor_db(factor, pre_gain=6.0, post_gain=0.4)
                  for factor in (1, 2, 4, 8)]
        self.assertLess(floors[1], floors[0] - 10.0,
                        "x2 bought less than 10 dB over x1: %r" % (floors,))
        self.assertLess(floors[2], floors[0] - 15.0,
                        "x4 bought less than 15 dB over x1: %r" % (floors,))
        # x8 is where this curve's floor bottoms out -- what is left is the
        # shaping's own in-band aliasing and the int16 output's quantisation,
        # neither of which another doubling touches. It must not get worse.
        self.assertLess(floors[3], floors[2] + 0.5,
                        "x8 was worse than x4: %r" % (floors,))


class HeadroomTest(unittest.TestCase):
    """audiodsp#99: a hard-clipping curve driven hard rings past the rails
    once decimated, and `post_gain` re-clips that overshoot at the base
    rate -- after the oversampling is done, where no factor of it reaches.

    Where this happens in the kernel (`src/shared/audiodsp_shaper.c`):
    `shape_sample` runs the curve at the oversampled rate and its own clamp
    (`curve_lookup`, clampf to +-1) bounds each of those samples, but the
    *decimated* one is not clamped there -- `halfband_down` is a low-pass,
    not a clip, so a hard edge through it can overshoot +-1. `post_gain`
    scales that decimated value (`audiodsp_shaper_process_s16`,
    `oversampled[0] * config->post_gain * 32768.0f`), and the only place
    this node ever clips to int16 is `to_s16`, two lines later, on
    `dry_gain * source + wet_gain * wet`. Everything from the curve to
    `post_gain` is `float` -- nothing here is int16 until that last line.

    HARD_CLIP ramps to the rails over the inner 10% of its span and is flat
    beyond it; driven at 32000 (98% of full scale) it is pinned flat for
    most of every half-cycle, the edge the issue is about. Bare node,
    1010 Hz, 48 kHz, oversample x4; the window is 4800 samples -- exactly
    101 cycles of 1010 Hz, so the fundamental and every harmonic fall on
    their own bin with no window and no rounding.

    Measured (`docs/upstream-diff.md`'s `audioshaper` section carries the
    full table): flat at -54.34 dB through `post_gain` 0.80, -41.24 at
    0.90, -36.44 at 1.00 -- a 13.1 dB fall by 0.90 that the bars below ask
    10 of, for margin. The control is oversample x1: no half-band, no
    ringing, and the floor reads -34.92 dB at every `post_gain` from 0.66
    to 1.00 -- the same curve and the same drive, showing no knee at all,
    which is what proves the x4 knee is the decimator's and not the
    curve's.
    """

    HZ = 1010
    FRAMES = 4800  # 48000 / 1010 * 101 == 4800 exactly: bin 101, no rounding
    CYCLES = HZ * FRAMES // SAMPLE_RATE
    LEVEL = 32000

    def _floor(self, post_gain, oversample=4):
        return alias_floor_db(oversample, cycles=self.CYCLES,
                              frames=self.FRAMES, level=self.LEVEL,
                              curve=HARD_CLIP, post_gain=post_gain)

    def test_the_floor_is_flat_below_the_knee(self):
        """0.66 and 0.74 read the same node to within 1 dB: on this table,
        the knee has not been reached yet at either."""
        low = self._floor(0.66)
        at = self._floor(0.74)
        self.assertLess(abs(at - low), 1.0,
                        "0.66 and 0.74 already differ: %.3f vs %.3f dB"
                        % (low, at))

    def test_the_floor_falls_past_the_knee(self):
        """0.90 is well past it. `CLIP_HEADROOM`'s ~0.74 ceiling has to be
        buying real room, not a fraction of a dB, or the constant is
        theatre -- measured here it is 13.1 dB; the bar asks 10, leaving
        margin rather than pinning the exact figure."""
        at = self._floor(0.74)
        past = self._floor(0.90)
        self.assertLess(at, past - 10.0,
                        "0.90 was not at least 10 dB worse than 0.74: "
                        "%.3f vs %.3f dB" % (at, past))

    def test_the_knee_is_the_decimators_not_the_curves(self):
        """Its control. At oversample x1 there is no half-band to ring, so
        the same curve and the same drive must show no knee at all -- if
        this one went red, the "knee" above would be the curve clipping on
        its own, not the decimator, and `CLIP_HEADROOM` would be the wrong
        fix."""
        floors = [self._floor(post, oversample=1)
                  for post in (0.66, 0.70, 0.74, 0.78, 0.80, 0.90, 1.00)]
        self.assertLess(max(floors) - min(floors), 0.5,
                        "the un-oversampled curve already shows a knee, so "
                        "x4's knee is not the decimator's doing: %r"
                        % (floors,))


class HysteresisTest(unittest.TestCase):
    """The option encloses an area, the area grows with drive, and off is off.

    Saturation TP3 asks for a loop that *grows*: at maximum drive the engaged
    trajectory's enclosed area has to beat the disengaged control by at least
    6 dB, and that excess has to grow monotonically over three drive
    settings. A static table meets TP3's disconfirmation condition a priori --
    its trajectory retraces itself and encloses exactly nothing -- so the
    control here is not a formality, it is the thing the option exists to
    stop being true.
    """

    def test_a_static_table_encloses_nothing(self):
        self.assertEqual(loop_area(1, 4.0), 0.0)

    def test_the_planted_fault_collapses_the_loop(self):
        """Width zero is the fault: the operator runs, and does nothing.

        This is the run that has to go red for the growth test above to mean
        anything -- an area that is there whatever the option says would be
        measuring the filters, not the operator.
        """
        self.assertEqual(loop_area(1, 4.0, hysteresis=0.9,
                                   hysteresis_width=0.0), 0.0)

    def test_the_area_grows_with_the_knob(self):
        """Three settings of `hysteresis`, one drive: the mechanism itself.

        TP3's own three drive settings are the class's to sweep -- a drive
        macro maps to these two floats (the Phase 0 sketch says so in as many
        words) -- and what has to be true of the node underneath is that the
        knob the macro turns is monotone and clears the control by 6 dB.
        """
        control = loop_area(4, 4.0, post_gain=0.5)
        areas = [loop_area(4, 4.0, hysteresis=amount, hysteresis_width=0.12,
                           post_gain=0.5)
                 for amount in (0.3, 0.6, 0.9)]
        for previous, following in zip(areas, areas[1:]):
            self.assertGreater(following, previous,
                               "area did not grow: %r" % (areas,))
        excess = 20.0 * math.log10(areas[-1] / control)
        self.assertGreater(excess, 6.0,
                           "excess over the control is under 6 dB: %.2f"
                           % (excess,))

    def test_drive_does_not_smear_the_loop(self):
        """The half-width is in input units, so the drive knob leaves it be.

        Without the |pre_gain| factor config_finish applies, the same
        absolute half-width would cover less of the input's swing at every
        turn of the drive knob and the enclosed area would *fall* as drive
        rose -- which is exactly the direction audioecho.FeedbackDelay and
        audiodynamics.Dynamics fail TP3 in. Measured without it: 0.163,
        0.084, 0.045 over pre_gain 1, 2, 4.
        """
        areas = [loop_area(4, drive, hysteresis=0.8, hysteresis_width=0.12,
                           post_gain=0.5 / drive)
                 for drive in (1.0, 2.0, 4.0)]
        # The x4 half-bands have a group delay of their own, so even a static
        # table encloses a little area here. It has to be small beside the
        # operator's, or this test would pass with the operator torn out.
        control = loop_area(4, 1.0, post_gain=0.5)
        self.assertGreater(areas[0], control * 4.0,
                           "the loop is the filters, not the operator: "
                           "%r against %r" % (areas, control))
        for area in areas[1:]:
            self.assertGreater(area, areas[0] * 0.75,
                               "drive shrank the loop: %r" % (areas,))
            self.assertLess(area, areas[0] * 1.35,
                            "drive grew the loop by itself: %r" % (areas,))


if __name__ == "__main__":
    unittest.main()


class UniversalTraitTest(unittest.TestCase):
    """S9-S11 - the traits every audiodsp-own node carries."""

    RATE = 48000
    CHANNELS = 2

    def _alternating(self, frames=4096, level=32767):
        values = array("h")
        for frame in range(frames):
            for _channel in range(self.CHANNELS):
                values.append(level if frame % 2 else -level)
        return audiocore.RawSample(values, sample_rate=self.RATE,
                                   channel_count=self.CHANNELS)

    def _alternating_words(self, count, level=32767):
        return [level if (index // self.CHANNELS) % 2 else -level
                for index in range(count)]

    def _silence(self, frames=4096):
        return audiocore.RawSample(
            array("h", bytes(frames * self.CHANNELS * 2)),
            sample_rate=self.RATE, channel_count=self.CHANNELS)

    def _words(self, node, blocks):
        out = []
        for _block in range(blocks):
            data = bytes(audiocore.get_buffer(node)[1])
            for position in range(0, len(data), 2):
                word = data[position] | (data[position + 1] << 8)
                out.append(word - 65536 if word >= 32768 else word)
        return out

    def test_the_neutral_setting_is_exact_at_the_rails(self):
        """S9, this module's form of the identity trait. `mix=0` is already
        covered elsewhere in this file on ordinary material; what is new here is
        **at +/-32767**, which is where an arithmetic width error shows and a
        range check does not. Measured 0 LSB."""
        node = audioshaper.Waveshaper(curve=cubic_curve(),
                                       sample_rate=self.RATE,
                                       channel_count=self.CHANNELS, mix=0.0)
        node.play(self._alternating())
        rendered = self._words(node, 6)
        self.assertEqual(rendered, self._alternating_words(len(rendered)))

    def test_the_identity_trait_discriminates(self):
        """Its control: the same node wet must not satisfy it."""
        node = audioshaper.Waveshaper(curve=cubic_curve(),
                                      sample_rate=self.RATE,
                                      channel_count=self.CHANNELS, mix=1.0)
        node.play(self._alternating())
        rendered = self._words(node, 6)
        self.assertNotEqual(rendered, self._alternating_words(len(rendered)))

    def test_silence_in_settles_on_the_curve_read_at_zero(self):
        """S10, and both of my first two guesses at it were wrong.

        A waveshaper maps every input through its curve, so silence in gives the
        curve read at zero - not zero. Two corrections got to what that means:

        * it is not the table's midpoint entry. `cubic_curve()` has 1024 points,
          so input zero falls *between* two of them and the node interpolates:
          the entry is 47 and the settled output is -1/-2.
        * it is not every sample either. The oversampler's half-bands prime from
          zero, so the render ramps into the value rather than starting there.

        So the trait is: the output **settles on a constant**, and that constant
        is small for a curve through the origin. A drift, or a large offset,
        fails it. The exact-zero form holds where zero lands on a table entry,
        which is the second half below.
        """
        node = audioshaper.Waveshaper(curve=cubic_curve(),
                                      sample_rate=self.RATE,
                                      channel_count=self.CHANNELS, mix=1.0)
        node.play(self._silence())
        rendered = self._words(node, 6)
        settled = rendered[len(rendered) // 2:]
        self.assertLessEqual(len(set(settled)), 2)      # constant, or one LSB
        self.assertLessEqual(max(abs(word) for word in settled), 2)

    def test_a_curve_through_a_table_entry_at_zero_gives_exact_zero(self):
        """S10's exact form, and the control for the bound above: three points
        put zero on an entry, and then it is 0 rather than nearly 0."""
        node = audioshaper.Waveshaper(curve=array("h", [-32768, 0, 32767]),
                                      sample_rate=self.RATE,
                                      channel_count=self.CHANNELS, mix=1.0)
        node.play(self._silence())
        for _block in range(4):
            data = bytes(audiocore.get_buffer(node)[1])
            self.assertEqual(data, bytes(len(data)))

    def test_clear_leaves_the_node_as_a_freshly_built_one(self):
        """S11. Not the same claim as "clear() stops it": this is that a
        cleared node and a node that never played render the same bytes."""
        used = audioshaper.Waveshaper(curve=cubic_curve(),
                                      sample_rate=self.RATE,
                                      channel_count=self.CHANNELS, mix=1.0)
        used.play(self._alternating())
        for _block in range(4):
            audiocore.get_buffer(used)
        used.clear()
        used.play(self._silence())

        fresh = audioshaper.Waveshaper(curve=cubic_curve(),
                                      sample_rate=self.RATE,
                                      channel_count=self.CHANNELS, mix=1.0)
        fresh.play(self._silence())
        for _block in range(6):
            self.assertEqual(bytes(audiocore.get_buffer(used)[1]),
                             bytes(audiocore.get_buffer(fresh)[1]))

    def test_the_clear_trait_discriminates(self):
        """Its control: without the clear the two must differ."""
        used = audioshaper.Waveshaper(curve=cubic_curve(),
                                      sample_rate=self.RATE,
                                      channel_count=self.CHANNELS, mix=1.0)
        used.play(self._alternating())
        for _block in range(4):
            audiocore.get_buffer(used)
        used.play(self._silence())               # deliberately not cleared

        fresh = audioshaper.Waveshaper(curve=cubic_curve(),
                                      sample_rate=self.RATE,
                                      channel_count=self.CHANNELS, mix=1.0)
        fresh.play(self._silence())
        differed = any(bytes(audiocore.get_buffer(used)[1])
                       != bytes(audiocore.get_buffer(fresh)[1])
                       for _block in range(6))
        self.assertTrue(differed, "an uncleared node already matches a fresh "
                        "one, so the clear trait cannot fail")


class SampleHoldTest(unittest.TestCase):
    """audioshaper.SampleHold: exact is the whole claim, so measure exactness.

    The node replaces a pair of `audiospeed.SpeedChanger`s whose rates are
    16.16 fixed point and cannot be made reciprocal except at powers of two.
    Measured on the effects programme's shipped hold, that pair's two rates
    multiplied to 0.9999947184696794 at 48 kHz -- one sample late per 189 339
    frames -- and to 1.0000107865780592 at 44.1 kHz, one sample early per
    92 708 (audiodsp#97). So every check here is an equality against integer
    arithmetic rather than a tolerance, and the control below is the pair
    itself: the same comparison run against what the class used to build must
    fail, or these tests are measuring nothing.
    """

    RATE = 48000

    def _ramp(self, frames, channels=2):
        """A source whose every frame is distinct, and whose value *names* its
        own frame number: `in[m] = (m % 65536) - 32768`. That is what lets a
        rendered frame be traced back to the source frame it was held from,
        which is the measurement the whole file turns on."""
        values = array("h")
        for frame in range(frames):
            for channel in range(channels):
                offset = 0 if channel == 0 else 30011
                values.append(((frame + offset) % 65536) - 32768)
        return audiocore.RawSample(values, sample_rate=self.RATE,
                                   channel_count=channels)

    def _rails(self, frames=2048):
        """Full scale, alternating, and the two channels in opposition: both
        rails on every frame, which is where an arithmetic width error shows
        and a range check does not."""
        values = array("h")
        for frame in range(frames):
            values.append(32767 if frame % 2 else -32768)
            values.append(-32768 if frame % 2 else 32767)
        return values

    def _hold(self, source, num, den):
        return audioshaper.SampleHold(source, num=num, den=den)

    def _render(self, node):
        """Every frame the node renders, until it says it is done."""
        out = array("h")
        while True:
            result, data = audiocore.get_buffer(node)
            out.frombytes(bytes(data))
            if result == audiocore.GET_BUFFER_DONE:
                return out

    def _left(self, rendered, channels=2):
        return rendered[0::channels]

    def _held_frame(self, index, num, den):
        """Which source frame frame `index` must be holding, in closed form.

        Independent arithmetic rather than the node's own loop re-typed: the
        refresh count at or before `index` is `1 + floor(index*den/num)`, and
        the r-th refresh lands on frame `ceil(r*num/den)`. Two rationals, no
        accumulator, so a fault in the accumulator has nowhere to hide.
        """
        refresh = (index * den) // num
        return -((-refresh * num) // den)

    def _refreshes(self, left):
        """Frames on which the held value changed, plus the first frame, which
        always latches. Every source frame is distinct, so a refresh always
        moves the output and this count is the accumulator's wrap count."""
        count = 1
        for index in range(1, len(left)):
            if left[index] != left[index - 1]:
                count += 1
        return count

    def _pair_indices(self, sample_rate, rate_hz, frames):
        """What the two-`SpeedChanger` chain holds, frame by frame.

        `down_q` and `up_q` are the class's own mapping onto audiospeed's Q16
        grid, and the composition is the one the fix for audiodsp#91 made
        exact: `source[(((n*up)>>16)*down)>>16]`.
        """
        down_q = int(65536 * sample_rate / rate_hz + 0.5)
        up_q = int(65536 * 65536 / down_q + 0.5)
        return [(((index * up_q) >> 16) * down_q) >> 16
                for index in range(frames)]

    def test_the_ratio_is_reduced_and_reported(self):
        """A class hands in the two numbers it has; the node hands back the
        pair it is actually running, so the rate it got can be disclosed."""
        node = self._hold(self._ramp(64), 48000, 26040)
        self.assertEqual((node.num, node.den), (400, 217))
        node = self._hold(self._ramp(64), 44100, 26040)
        self.assertEqual((node.num, node.den), (105, 62))
        node = self._hold(self._ramp(64), 22050, 22050)
        self.assertEqual((node.num, node.den), (1, 1))

    def test_the_hold_ratio_is_exact(self):
        """The refresh count over a whole number of periods is `N*den/num`
        exactly -- at three sample rates, and at the hold rate the effects
        programme ships (26 040 Hz, which is 400/217 at 48 kHz, 105/62 at
        44.1 kHz and a clamp to the wire at 22.05 kHz).
        """
        for num, den, periods in ((400, 217, 30),      # 26040 Hz at 48000
                                  (105, 62, 120),      # 26040 Hz at 44100
                                  (2, 1, 6000),        # 11025 Hz at 22050
                                  (1, 1, 12000)):      # the clamped case
            frames = num * periods
            left = self._left(self._render(
                self._hold(self._ramp(frames), num, den)))
            self.assertEqual(len(left), frames)
            self.assertEqual(self._refreshes(left), frames * den // num,
                             "%d/%d refreshed the wrong number of times"
                             % (num, den))

    def test_the_count_over_a_partial_period_is_the_documented_rounding(self):
        """Off a period boundary the count is `1 + floor((N-1)*den/num)`: the
        first frame always latches, and every wrap after it is one more. Said
        here rather than left implicit, because it is the one place the count
        is not simply `floor(N*den/num)` -- at 44.1 kHz over 4000 frames those
        two differ (2362 against 2361) and the first is the right answer."""
        for num, den, frames in ((400, 217, 4000), (105, 62, 4000),
                                 (1024, 3, 3000), (3, 1, 3001)):
            left = self._left(self._render(
                self._hold(self._ramp(frames), num, den)))
            self.assertEqual(self._refreshes(left),
                             1 + ((frames - 1) * den) // num,
                             "%d/%d over %d frames" % (num, den, frames))

    def test_it_holds_the_frame_the_arithmetic_names(self):
        """Every frame, not just the count: the rendered value is the source
        frame the closed form names, for 20 000 frames at four ratios."""
        for num, den in ((400, 217), (105, 62), (2, 1), (1024, 3), (1, 1)):
            frames = 20000
            left = self._left(self._render(
                self._hold(self._ramp(frames), num, den)))
            expected = [(self._held_frame(index, num, den) % 65536) - 32768
                        for index in range(frames)]
            self.assertEqual(list(left), expected,
                             "%d/%d does not hold what the arithmetic names"
                             % (num, den))

    def test_it_has_not_moved_by_the_distance_the_pair_flanged_over(self):
        """400 000 frames -- twice the distance at which the pair was a whole
        sample late -- and the count is still exact and the held frame is
        still the one the closed form names at the very end."""
        frames = 400000
        left = self._left(self._render(
            self._hold(self._ramp(frames), 400, 217)))
        self.assertEqual(len(left), frames)
        self.assertEqual(self._refreshes(left), frames * 217 // 400)
        for index in (196608, 250000, frames - 1):
            self.assertEqual(
                left[index],
                (self._held_frame(index, 400, 217) % 65536) - 32768,
                "the hold has walked by frame %d" % (index,))

    def test_the_exactness_check_discriminates(self):
        """The control, and it is the mechanism this node replaces.

        The two-`SpeedChanger` composition is run through the same comparison
        over the same distance. It must fail -- and where it first fails is
        the measurement from the issue: the pair is holding a frame the
        arithmetic does not name long before 400 000 frames, and by the end
        it is a whole sample adrift. A check both mechanisms passed would be
        measuring the source, not the accumulator.
        """
        frames = 400000
        indices = self._pair_indices(48000, 26040.0, frames)
        exact = [self._held_frame(index, 400, 217) for index in range(frames)]
        self.assertNotEqual(indices, exact,
                            "the fixed-point pair passed the exactness "
                            "check, so the check cannot fail")
        first = next(index for index in range(frames)
                     if indices[index] != exact[index])
        walk = exact[frames - 1] - indices[frames - 1]
        self.assertLess(first, frames)
        self.assertGreaterEqual(walk, 1,
                                "the pair did not fall behind at all: %d"
                                % (walk,))

    def test_one_over_one_is_a_wire(self):
        """At the rails, where an arithmetic width error shows and a range
        check does not. 1/1 refreshes on every frame, so the output is the
        input byte for byte."""
        source = audiocore.RawSample(self._rails(), sample_rate=self.RATE,
                                     channel_count=2)
        rendered = self._render(audioshaper.SampleHold(source, num=1, den=1))
        self.assertEqual(list(rendered), list(self._rails()))

    def test_the_wire_check_discriminates(self):
        """Its control: the same node at 2/1 must not satisfy it."""
        source = audiocore.RawSample(self._rails(), sample_rate=self.RATE,
                                     channel_count=2)
        rendered = self._render(audioshaper.SampleHold(source, num=2, den=1))
        self.assertNotEqual(list(rendered), list(self._rails()))

    def test_the_latency_is_zero_and_says_so(self):
        """The node reports 0 at every ratio, and the report is a
        measurement: the first frame out is the first frame in, because a
        refresh latches the frame it is looking at rather than the one before
        it. What a hold displaces -- an event landing on a frame it drops --
        is up to `ceil(num/den) - 1` frames and belongs to the class that
        turns this into a rate knob, not to this node."""
        for num, den in ((1, 1), (2, 1), (400, 217), (105, 62), (1024, 3)):
            source = self._ramp(512)
            node = self._hold(source, num, den)
            self.assertEqual(node.latency, 0)
            left = self._left(self._render(node))
            self.assertEqual(left[0], -32768, "%d/%d began late"
                             % (num, den))

    def test_silence_in_is_exact_zero_out(self):
        """No filter, no tail, no dither: a held zero is a zero. Exact, from
        the first frame, at a ratio that is not a whole number."""
        frames = 4096
        source = audiocore.RawSample(array("h", bytes(frames * 4)),
                                     sample_rate=self.RATE, channel_count=2)
        rendered = self._render(self._hold(source, 400, 217))
        self.assertEqual(len(rendered), frames * 2)
        self.assertEqual(set(rendered), {0})

    def test_both_channels_hold_together(self):
        """One accumulator for the frame, not one per channel: the two
        channels refresh on exactly the same frames, so a stereo pair cannot
        be smeared apart by the hold."""
        frames = 8192
        rendered = self._render(self._hold(self._ramp(frames), 400, 217))
        left = rendered[0::2]
        right = rendered[1::2]
        moved_left = [index for index in range(1, frames)
                      if left[index] != left[index - 1]]
        moved_right = [index for index in range(1, frames)
                       if right[index] != right[index - 1]]
        self.assertEqual(moved_left, moved_right)
        # And the right channel holds its own value, not the left's: the two
        # differ by a fixed offset in this source, so a channel that followed
        # the wrong one would read as an equal pair.
        self.assertNotEqual(list(left[:64]), list(right[:64]))

    def test_a_frame_in_is_a_frame_out(self):
        """It is a rate reducer, not a resampler: the block after it is the
        length of the block before it, and the stream ends where the source
        ends rather than running on into silence."""
        for frames in (4096, 4000, 257, 256, 255):
            for num, den in ((400, 217), (1, 1), (3, 1)):
                rendered = self._render(
                    self._hold(self._ramp(frames), num, den))
                self.assertEqual(len(rendered), frames * 2,
                                 "%d frames at %d/%d came back as %d"
                                 % (frames, num, den, len(rendered) // 2))

    def test_an_eight_bit_mono_source_is_carried_as_it_is(self):
        """The node holds frames as bytes, so it carries whatever its source
        is. Unsigned 8-bit silence is 0x80 rather than 0, and a hold that
        looked inside a sample would get that wrong."""
        source = audiocore.RawSample(array("B", [128] * 64),
                                     sample_rate=self.RATE, channel_count=1)
        node = audioshaper.SampleHold(source, num=3, den=1)
        self.assertEqual(node.bits_per_sample, 8)
        self.assertEqual(bytes(audiocore.get_buffer(node)[1]), bytes([128]) * 64)

    def test_a_ratio_that_would_invent_frames_is_refused(self):
        """`den > num` is a rate *increase*, which a hold cannot do -- it
        consumes one frame per frame. Refused rather than clamped: a silently
        clamped ratio is a class shipping a hold rate it did not ask for."""
        for num, den in ((1, 2), (400, 401), (0, 1), (1, 0), (-1, 1)):
            with self.assertRaises(ValueError):
                self._hold(self._ramp(64), num, den)

    def test_the_ratio_moves_as_a_pair(self):
        """`set()` takes both halves, and a ratio that actually changed
        re-arms the accumulator while one set to what it already was leaves
        the staircase running. A class writes its settings on every block;
        re-latching 187 times a second would be a defect nobody asked for."""
        source = self._ramp(4096)
        node = self._hold(source, 400, 217)
        first = bytes(audiocore.get_buffer(node)[1])
        node.set(400, 217)
        unchanged = bytes(audiocore.get_buffer(node)[1])
        self.assertNotEqual(first, unchanged)      # the stream moved on

        fresh = self._hold(self._ramp(4096), 400, 217)
        audiocore.get_buffer(fresh)
        self.assertEqual(unchanged, bytes(audiocore.get_buffer(fresh)[1]),
                         "setting the ratio it already had restarted the "
                         "staircase")

        node.set(2, 1)
        self.assertEqual((node.num, node.den), (2, 1))
        with self.assertRaises(ValueError):
            node.set(1, 2)

    def test_a_replayed_node_is_a_fresh_one(self):
        """`play()` re-sources and starts the staircase over, so a node that
        has run and a node that never has render the same bytes."""
        used = self._hold(self._ramp(4096), 400, 217)
        for _block in range(3):
            audiocore.get_buffer(used)
        used.play(self._ramp(4096))
        fresh = self._hold(self._ramp(4096), 400, 217)
        for _block in range(4):
            self.assertEqual(bytes(audiocore.get_buffer(used)[1]),
                             bytes(audiocore.get_buffer(fresh)[1]))

    def test_the_replay_check_discriminates(self):
        """Its control: without the replay the two must differ."""
        used = self._hold(self._ramp(4096), 400, 217)
        for _block in range(3):
            audiocore.get_buffer(used)
        fresh = self._hold(self._ramp(4096), 400, 217)
        differed = any(bytes(audiocore.get_buffer(used)[1])
                       != bytes(audiocore.get_buffer(fresh)[1])
                       for _block in range(4))
        self.assertTrue(differed, "a node that has already run matches a "
                        "fresh one, so the replay trait cannot fail")
