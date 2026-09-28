"""audioecho: the traits and the surface the renders cannot check.

`tests/parity/feedback_delay_probe.py` and `feedback_delay_options_probe.py`
pin what this module renders across the three targets. What is left for here is
what a render cannot say, and the numeric traits with their bars.

`docs/correctness-standard.md` is what this file implements for `audioecho`:
the module is ours - upstream CircuitPython has no counterpart - so it is held
to three-target agreement plus the traits below, never to a previous version of
its own output.

## The traits, with their bars

| ID | Trait | Bar |
|---|---|---|
| E1 | `mix=0` is a bit-exact wire, at the rails included | exact, 0 LSB |
| E2 | Silence in is exactly zero out | exact |
| E3 | `clear()` leaves the node as a freshly built one | exact |
| E4 | A starved node yields a full block of silence, not a short block | exact |
| E5 | A mono render of a short source has no gaps in it | exact, 0 zero samples |
| E6 | `audiodelays.Echo` with no filter is a bit-exact identity with `filter=None` | exact, 0 LSB |
| E7 | `Echo.filter` in the feedback loop changes the delay line | at least 1 sample differs |
| E8 | A setting moved on a live node lands on the render | the response moves to the new setting |
| E9 | `set()` needs no second call to finish the config | exact, every option |
| E10 | After the input stops, the repeats reach exact zero, at every feedback up to the clamp | exact, 0 LSB, and each lap strictly quieter than the last |
| E11 | E10 with the damping low-pass in, at the feedbacks where 0.5 / (1 - feedback) is whole | exact, 0 LSB |
| E12 | A loop filter set to 0 and back plays nothing stale: silence stays silence, a steady line stays put, a high-pass comes back as a fresh one | exact, 0 LSB |

**E10 is audiodsp#153.** The feedback write rounded to nearest, so a repeat x
came back as round(feedback * x) and every |x| <= 0.5 / (1 - feedback) was its
own image: from feedback 0.5 up, a few LSB went round the line forever (1 at
0.5, 5 at 0.9, 50 at the 0.99 clamp), and no class built on the node could
report a finite tail. Where rounding would hand a repeat back unchanged, the
fed-back term now truncates toward zero instead, which makes each lap's largest
value at least one LSB smaller than the last.

**E1 is this module's form of the identity trait** that
`docs/correctness-standard.md` asks of every own node - an exact answer through
the DSP, at full scale, because that is where an arithmetic width error shows
and a range check does not.

**E5 is audiodsp#54, turned into a property.** The MicroPython binding advanced
its destination by `produced * 2` while the DSP writes `channel_count` samples
per frame, so a mono node whose source handed out fewer than 256 frames per
pull interleaved its output with the gap it left. Both conditions had to hold,
which is why every stereo fixture in the probe was correct by coincidence and
the defect lived in a shipped node. The property is exact and cheap: with
material that never crosses zero on a frame boundary, a correct mono render
contains **no zero samples at all** - measured, 0 of 1024 - and a stride error
leaves them by the hundred. The cross-target half of this lives in the probe's
`mono-short` case; this is the half a single interpreter can state on its own.
"""

from array import array
import unittest

import audiocore
import audioecho

SAMPLE_RATE = 8000
CHANNELS = 2

#: Shorter than AUDIODSP_FEEDBACK_DELAY_FRAMES (256), which is the condition
#: audiodsp#54 needed: the inner loop then runs more than once.
SHORT_FRAMES = 100


def alternating(frames=4096, level=20000, channels=CHANNELS):
    values = array("h")
    for frame in range(frames):
        for _channel in range(channels):
            values.append(level if frame % 2 else -level)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def alternating_words(count, level=20000, channels=CHANNELS):
    """Generated rather than rendered, so the comparison cannot inherit a
    node's own bug."""
    return [level if (index // channels) % 2 else -level
            for index in range(count)]


def mono_short(frames=SHORT_FRAMES):
    """Mono, shorter than one DSP chunk, and never zero - so a gap in the
    output is unambiguous."""
    values = array("h")
    for frame in range(frames):
        values.append(((frame * 907) % 20000) - 10000)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=1)


def silence(frames=4096, channels=CHANNELS):
    return audiocore.RawSample(array("h", bytes(frames * channels * 2)),
                               sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def words(node, blocks):
    out = []
    for _block in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        for position in range(0, len(data), 2):
            word = data[position] | (data[position + 1] << 8)
            out.append(word - 65536 if word >= 32768 else word)
    return out


def delay(channels=CHANNELS, max_delay_ms=50, **extra):
    return audioecho.FeedbackDelay(max_delay_ms=max_delay_ms,
                                   sample_rate=SAMPLE_RATE,
                                   channel_count=channels, **extra)


class IdentityTest(unittest.TestCase):
    """E1, and the control that gives it teeth."""

    def test_mix_zero_is_a_bit_exact_wire_at_the_rails(self):
        """E1."""
        for level in (20000, 32767):
            with self.subTest(level=level):
                node = delay(mix=0.0)
                node.play(alternating(level=level))
                rendered = words(node, 6)
                self.assertEqual(
                    rendered, alternating_words(len(rendered), level=level))

    def test_E1_discriminates(self):
        """E1's control. A wet node must NOT satisfy it, or the comparison
        would pass for anything."""
        node = delay(mix=1.0, feedback=0.7, delay_ms=20.0)
        node.play(alternating())
        rendered = words(node, 6)
        self.assertNotEqual(rendered, alternating_words(len(rendered)))


class MonoStrideTest(unittest.TestCase):
    """E5 - audiodsp#54 as a property rather than a digest."""

    def test_a_mono_short_source_renders_without_gaps(self):
        """E5."""
        node = audioecho.FeedbackDelay(max_delay_ms=10,
                                       sample_rate=SAMPLE_RATE,
                                       channel_count=1, delay_ms=6.0,
                                       feedback=0.7, mix=1.0)
        node.play(mono_short())
        rendered = words(node, 4)
        self.assertGreater(len(rendered), 1000)
        self.assertEqual([index for index, word in enumerate(rendered)
                          if word == 0], [])

    def test_E5_reads_a_real_render(self):
        """E5's control. "No zero samples" is also true of a read that
        returned nothing, so the render must be as long as it claims and must
        actually vary."""
        node = audioecho.FeedbackDelay(max_delay_ms=10,
                                       sample_rate=SAMPLE_RATE,
                                       channel_count=1, delay_ms=6.0,
                                       feedback=0.7, mix=1.0)
        node.play(mono_short())
        rendered = words(node, 4)
        self.assertEqual(len(rendered), 4 * 256)
        self.assertGreater(len(set(rendered)), 50)


class StateTest(unittest.TestCase):
    """E2, E3, E4."""

    def test_silence_in_is_exactly_zero_out(self):
        """E2."""
        node = delay(mix=1.0, feedback=0.7)
        node.play(silence())
        for _block in range(6):
            data = bytes(audiocore.get_buffer(node)[1])
            self.assertEqual(data, bytes(len(data)))

    def test_clear_leaves_the_node_as_a_freshly_built_one(self):
        """E3."""
        used = delay(mix=1.0, feedback=0.7)
        used.play(alternating())
        for _block in range(4):
            audiocore.get_buffer(used)
        used.clear()
        used.play(silence())

        fresh = delay(mix=1.0, feedback=0.7)
        fresh.play(silence())
        for _block in range(6):
            self.assertEqual(bytes(audiocore.get_buffer(used)[1]),
                             bytes(audiocore.get_buffer(fresh)[1]))

    def test_E3_discriminates_a_node_that_was_not_cleared(self):
        """E3's control."""
        used = delay(mix=1.0, feedback=0.7)
        used.play(alternating())
        for _block in range(4):
            audiocore.get_buffer(used)
        used.play(silence())                      # deliberately not cleared

        fresh = delay(mix=1.0, feedback=0.7)
        fresh.play(silence())
        differed = any(bytes(audiocore.get_buffer(used)[1])
                       != bytes(audiocore.get_buffer(fresh)[1])
                       for _block in range(6))
        self.assertTrue(differed, "an uncleared delay already matches a fresh "
                        "one, so E3 cannot fail")

    def test_a_starved_node_yields_silence_not_a_short_block(self):
        """E4. This node sits mid-graph and never reports itself finished."""
        node = delay(mix=1.0)
        result, data = audiocore.get_buffer(node)
        self.assertEqual(result, audiocore.GET_BUFFER_MORE_DATA)
        self.assertEqual(len(data), 256 * CHANNELS * 2)
        self.assertEqual(bytes(data), bytes(len(data)))


class EchoFilterTest(unittest.TestCase):
    """E6 and E7 - CircuitPython 10.3.0's Echo.filter, on the CPython twin.

    The render agreement lives in `echo_filter_probe.py`. What is left here is
    the empty-chain identity and the control that a set filter is not a no-op.
    """

    def test_no_filter_matches_an_explicit_none(self):
        """E6."""
        import audiodelays
        implicit = audiodelays.Echo(
            max_delay_ms=80, delay_ms=40, decay=0.7, mix=1.0,
            freq_shift=False, sample_rate=SAMPLE_RATE, channel_count=2,
            buffer_size=512)
        explicit = audiodelays.Echo(
            max_delay_ms=80, delay_ms=40, decay=0.7, mix=1.0,
            freq_shift=False, filter=None, sample_rate=SAMPLE_RATE,
            channel_count=2, buffer_size=512)
        implicit.play(alternating())
        explicit.play(alternating())
        for _block in range(6):
            self.assertEqual(bytes(audiocore.get_buffer(implicit)[1]),
                             bytes(audiocore.get_buffer(explicit)[1]))

    def test_E6_discriminates_a_set_filter(self):
        """E6's control."""
        import audiodelays
        import synthio
        bare = audiodelays.Echo(
            max_delay_ms=80, delay_ms=40, decay=0.7, mix=1.0,
            freq_shift=False, sample_rate=SAMPLE_RATE, channel_count=2,
            buffer_size=512)
        filtered = audiodelays.Echo(
            max_delay_ms=80, delay_ms=40, decay=0.7, mix=1.0,
            freq_shift=False,
            filter=synthio.Biquad(synthio.FilterMode.LOW_PASS, 800, 0.7),
            sample_rate=SAMPLE_RATE, channel_count=2, buffer_size=512)
        bare.play(alternating())
        filtered.play(alternating())
        differed = any(bytes(audiocore.get_buffer(bare)[1])
                       != bytes(audiocore.get_buffer(filtered)[1])
                       for _block in range(6))
        self.assertTrue(differed, "a low-pass in the echo loop already "
                        "matches an empty chain, so E6 cannot fail")

    def test_a_lowpass_in_the_loop_changes_the_delay_line(self):
        """E7."""
        import audiodelays
        import synthio
        node = audiodelays.Echo(
            max_delay_ms=80, delay_ms=40, decay=0.7, mix=1.0,
            freq_shift=False,
            filter=synthio.Biquad(synthio.FilterMode.LOW_PASS, 500, 0.7),
            sample_rate=SAMPLE_RATE, channel_count=2, buffer_size=512)
        node.play(alternating())
        rendered = words(node, 6)
        self.assertNotEqual(rendered, alternating_words(len(rendered)))


class LiveSettingTest(unittest.TestCase):
    """E8 and E9 - audiodsp#106.

    The issue was filed on a reading: `set()` never calls `config_finish`, so
    the words derived from a setting were said to stay stale. They do not.
    Every case of `audiodsp_feedback_delay_configure` derives its own words,
    so a setting lands the moment it is written, and a `finish()` after a
    `set()` recomputes the same numbers from the same stored values. E9 is
    what keeps that true: an option added later that derives nothing of its
    own fails here rather than in a caller's ears.

    E8 is the trait the issue's second half asked for and nothing had: move a
    setting on a *live* node - one that has already rendered - and read the
    response, rather than trusting that the write arrived.
    """

    #: One value per option, each far enough from the baseline below that the
    #: render cannot help but move.
    OPTIONS = {
        "delay_ms": 18.0, "feedback": 0.4, "mix": 0.7, "damping_hz": 900.0,
        "cut_hz": 600.0, "wow_hz": 6.0, "wow_depth_ms": 3.0,
        "cross_feed": 1.0, "loop_drive": 1.0, "input_pan": -1.0,
        "delay_slew": 8.0, "wow_am_depth": 0.9, "loop_semitones": -12.0,
        "loop_window_ms": 11.0,
    }

    #: `shift_window_finish` clamps the shift window to a quarter of the
    #: delay line, which is 12.5 ms at this rate and this `max_delay_ms`.
    #: Both values above have to sit under that or the clamp makes them the
    #: same number and the option reads as inert.

    #: A loop that is actually running, so an option that only acts inside it
    #: has something to act on. At feedback 0 most of the table is inert.
    BASELINE = dict(delay_ms=10.0, feedback=0.75, mix=2.0, damping_hz=3000.0,
                    cut_hz=100.0, wow_hz=2.0, wow_depth_ms=1.0,
                    cross_feed=0.5, loop_drive=0.4, input_pan=0.3,
                    delay_slew=1.0, wow_am_depth=0.2, loop_semitones=3.0,
                    loop_window_ms=4.0)

    def _live(self, **changed):
        """A node that has already rendered four blocks, then changed."""
        node = delay(**self.BASELINE)
        node.play(alternating())
        words(node, 4)
        if changed:
            node.set(**changed)
        return node

    def test_a_delay_moved_on_a_live_node_moves_the_echo(self):
        """E8, the delay. A burst, then the tap it comes back on."""
        for delay_ms in (12.0, 30.0):
            with self.subTest(delay_ms=delay_ms):
                node = delay(max_delay_ms=50, delay_ms=45.0, feedback=0.0,
                             mix=2.0)
                node.play(silence(64))
                words(node, 2)                    # live, and the line is clear
                node.set(delay_ms=delay_ms)
                burst = array("h", [0] * (4096 * CHANNELS))
                for index in range(8 * CHANNELS):
                    burst[index] = 20000
                node.play(audiocore.RawSample(burst, sample_rate=SAMPLE_RATE,
                                              channel_count=CHANNELS))
                rendered = words(node, 8)
                left = rendered[0::CHANNELS]
                peak = max(range(len(left)), key=lambda i: abs(left[i]))
                self.assertAlmostEqual(peak / SAMPLE_RATE * 1000.0, delay_ms,
                                       delta=1.0)

    def test_a_feedback_moved_on_a_live_node_moves_the_tail(self):
        """E8, the feedback. More feedback is a louder tail, measured."""
        energy = []
        for feedback in (0.1, 0.9):
            node = delay(max_delay_ms=50, delay_ms=10.0, feedback=0.0,
                         mix=2.0)
            node.play(alternating(256))
            words(node, 2)
            node.set(feedback=feedback)
            node.play(silence(8192))
            tail = words(node, 12)
            energy.append(sum(abs(value) for value in tail))
        self.assertGreater(energy[1], energy[0] * 2)

    def test_every_option_lands_without_a_second_finish(self):
        """E9. `set(option)` then `finish()` renders what `set(option)`
        renders, for every option the node takes."""
        for name, value in self.OPTIONS.items():
            with self.subTest(option=name):
                plain = self._live(**{name: value})
                finished = self._live(**{name: value})
                finished._state.finish()
                self.assertEqual(words(plain, 12), words(finished, 12))

    def test_every_option_moves_the_render(self):
        """The control for E9: an option that changed nothing would pass E9
        for the wrong reason. `delay_slew` is excluded and named - it governs
        how fast a *moving* delay glides, so it is inert while the delay is
        held."""
        unchanged = self._live()
        reference = words(unchanged, 12)
        for name, value in self.OPTIONS.items():
            if name == "delay_slew":
                continue
            with self.subTest(option=name):
                self.assertNotEqual(words(self._live(**{name: value}), 12),
                                    reference)


class TailReachesZeroTest(unittest.TestCase):
    """E10 - audiodsp#153.

    One impulse per channel (+1000 left, -1000 right, so a rounding that is
    wrong on one sign only is caught too), then silence for as long as the
    source lasts: the node only advances while its source delivers frames. A
    10 ms delay at 8 kHz is 80 whole frames, so with no filter each lap holds
    exactly one nonzero value per channel and the lap peaks are the loop's
    magnitude, read directly.
    """

    DELAY_FRAMES = 80
    #: At the 0.99 clamp the peak falls by 1 % a lap until it is small, then
    #: by one LSB a lap: about 330 laps from 1000. 400 is the margin.
    LAPS = 400
    #: 0.5 is where the old write started holding 1 LSB; 1.0 is clamped to
    #: 0.99 by the node, which is the worst case it allows.
    FEEDBACKS = (0.5, 0.6, 0.7, 0.8, 0.9, 0.95, 0.99, 1.0)

    def _render(self, channels, burst=False, **extra):
        frames = self.DELAY_FRAMES * self.LAPS
        data = array("h", bytes(2 * channels * (frames + 512)))
        data[0] = 1000
        if channels == 2:
            data[1] = -1000
        if burst:
            # A 64-frame ramp, unequal between the channels: a lone impulse
            # can fall between the pitch shifter's taps, and a hard-panned
            # pair of equal and opposite impulses averages to nothing.
            for frame in range(64):
                data[frame * channels] = 1000 - 15 * frame
                if channels == 2:
                    data[frame * channels + 1] = -(600 - 9 * frame)
        options = dict(delay_ms=10.0)
        options.update(extra)
        node = delay(channels=channels, mix=2.0, **options)
        node.play(audiocore.RawSample(data, sample_rate=SAMPLE_RATE,
                                      channel_count=channels))
        # A block is 256 frames at either width.
        blocks = -(-frames // 256)
        rendered = words(node, blocks)[:frames * channels]
        self.assertEqual(len(rendered), frames * channels)
        return rendered

    def _lap_peaks(self, rendered, channels, channel):
        lap = self.DELAY_FRAMES * channels
        return [max(abs(value) for value in
                    rendered[start + channel:start + lap:channels])
                for start in range(0, len(rendered) - lap + 1, lap)]

    def test_the_repeats_reach_exact_zero(self):
        """E10: the last lap is silent, at every feedback and both widths."""
        for channels in (2, 1):
            for feedback in self.FEEDBACKS:
                with self.subTest(channels=channels, feedback=feedback):
                    rendered = self._render(channels, feedback=feedback)
                    last_lap = rendered[-self.DELAY_FRAMES * channels:]
                    self.assertEqual(max(abs(v) for v in last_lap), 0)

    def test_each_lap_is_quieter_than_the_last_until_silence(self):
        """E10's shape: the magnitude strictly falls and, once zero, stays
        zero. A floor that only happened to break would pass the first test
        at one length and fail it at another; this one cannot."""
        for channels in (2, 1):
            for feedback in self.FEEDBACKS:
                for channel in range(channels):
                    with self.subTest(channels=channels, feedback=feedback,
                                      channel=channel):
                        peaks = self._lap_peaks(
                            self._render(channels, feedback=feedback),
                            channels, channel)
                        # Lap 0 is the empty line (the delay has not come
                        # round yet); lap 1 is the first repeat, 1000 exact.
                        self.assertEqual(peaks[0], 0)
                        self.assertEqual(peaks[1], 1000)
                        tail = peaks[1:]
                        for before, after in zip(tail, tail[1:]):
                            if before == 0:
                                self.assertEqual(after, 0)
                            else:
                                self.assertLess(after, before)

    #: Each thing the loop can put between the read and the write, one at a
    #: time, then all together. One at a time because each is a place a value
    #: could be held; together because that is what a class builds.
    LOOPS = {
        "fractional delay": dict(delay_ms=10.37),
        "damping": dict(damping_hz=2500.0),
        "cut": dict(cut_hz=150.0),
        "soft-clip": dict(loop_drive=0.8),
        "cross-feed": dict(cross_feed=1.0, input_pan=-1.0),
        "wow": dict(wow_hz=0.7, wow_depth_ms=1.0),
        "pitch shift": dict(loop_semitones=12.0),
        "all": dict(damping_hz=2500.0, cut_hz=150.0, loop_drive=0.5,
                    cross_feed=0.5, wow_hz=0.7, wow_depth_ms=1.0),
    }

    def test_every_loop_element_reaches_exact_zero(self):
        """E10 through everything the loop can hold a value in. All but one
        are a convex mix or a shrink, which cannot undo a write that is
        always smaller than what the loop sent it; the cut is a one-pole high-pass, which can overshoot,
        so it is here to be measured rather than argued. On the old write
        31 of these 48 held a residue (1 to 50 LSB)."""
        for name, loop in self.LOOPS.items():
            for channels in (2, 1):
                for feedback in (0.5, 0.9, 0.99):
                    with self.subTest(loop=name, channels=channels,
                                      feedback=feedback):
                        rendered = self._render(channels, burst=True,
                                                feedback=feedback, **loop)
                        self.assertGreater(max(abs(v) for v in rendered), 0)
                        last_lap = rendered[-self.DELAY_FRAMES * channels:]
                        self.assertEqual(max(abs(v) for v in last_lap), 0)


def render_48k(channels, lead, silent, level, **options):
    """`lead` frames of a `level` LSB DC, then `silent` frames of silence,
    through a node at 48 kHz, where the damping corners the delay classes
    hand the node are narrow enough to show the float32 stall. Returns the
    output words."""
    data = array("h", [level] * (lead * channels))
    data.extend(array("h", bytes(2 * channels * silent)))
    node = audioecho.FeedbackDelay(sample_rate=48000, channel_count=channels,
                                   mix=2.0, **options)
    node.play(audiocore.RawSample(data, sample_rate=48000,
                                  channel_count=channels))
    rendered = []
    for _block in range((lead + silent) // 256):
        rendered.extend(array("h", bytes(audiocore.get_buffer(node)[1])))
    return rendered


class DampedTailReachesZeroTest(unittest.TestCase):
    """E11 - audiodsp#157. E10 with the damping low-pass in the loop, at the
    feedbacks where it used to hold.

    The loop sends the damping state round, a float32 one-pole. With the
    line holding a steady v it approaches v from above and stopped a few ulps
    short, where its step rounded away. At f = 1 - 0.5 / v the feedback write
    then rounded f * (v + a few ulps) back to v, and v went round for ever:
    1 / 2 / 3 / 4 / 5 LSB at feedback 0.5 / 0.75 / 0.8333 / 0.875 / 0.9 with
    an 800 Hz corner at 48 kHz, 1 LSB at 0.5 with 3 kHz. A step too small to
    move the state now lands it on its input, so the line empties.
    """

    #: 0.5 / (1 - f) whole, which is where the stall was: the value held.
    FEEDBACKS = (0.5, 0.75, 5.0 / 6.0, 0.875, 0.9)

    def test_a_damped_dc_tail_reaches_exact_zero(self):
        """E11: the last 20 ms after 1.3 s of silence is exactly zero."""
        for channels in (2, 1):
            for damping_hz in (800.0, 3000.0):
                for feedback in self.FEEDBACKS:
                    for level in (2, 5, 100):
                        with self.subTest(channels=channels,
                                          damping_hz=damping_hz,
                                          feedback=feedback, level=level):
                            rendered = render_48k(
                                channels, 256 * 20, 256 * 250, level,
                                max_delay_ms=20.0, delay_ms=12.5,
                                feedback=feedback, damping_hz=damping_hz)
                            tail = rendered[-960 * channels:]
                            self.assertEqual(max(abs(v) for v in tail), 0)


class SwitchedFilterTest(unittest.TestCase):
    """E12 - audiodsp#158 and #159. A loop filter taken out and put back in
    plays nothing stale.

    A loud square with the filter in, the filter set to 0 while it still
    plays, silence until the line is empty, then the filter back in with
    nothing playing. With the filter at 0 its state used to freeze, and the
    node put the frozen value out of silence (15 393 LSB at 48 kHz for the
    low-pass, 19 110 for the high-pass). Now the low-pass state follows the
    tap while out, and the high-pass state is held at zero, where its output
    is its input.
    """

    #: The option, and the corner it goes back in at.
    FILTERS = (("damping_hz", 900.0), ("cut_hz", 300.0))

    def _render(self, channels, option, corner, feedback):
        node = delay(channels=channels, max_delay_ms=50, delay_ms=20.0,
                     feedback=feedback, mix=2.0, **{option: corner})
        loud = array("h")
        for frame in range(2048):
            level = 20000 if (frame // 40) % 2 else 6000
            for channel in range(channels):
                loud.append(level if channel == 0 else -level)
        node.play(audiocore.RawSample(loud, sample_rate=SAMPLE_RATE,
                                      channel_count=channels))
        words(node, 6)                     # filter in, playing
        node.set(**{option: 0.0})
        words(node, 2)                     # filter out, still playing
        node.play(silence(256 * 64, channels))
        emptied = words(node, 60)          # the line runs dry
        node.set(**{option: corner})
        return emptied, words(node, 4)     # filter back in, silence

    def test_a_filter_put_back_in_after_silence_is_silent(self):
        """E12: the line is empty before the filter goes back in, and the
        output stays exactly zero after."""
        for option, corner in self.FILTERS:
            for channels in (2, 1):
                for feedback in (0.0, 0.5, 0.9):
                    with self.subTest(option=option, channels=channels,
                                      feedback=feedback):
                        emptied, after = self._render(channels, option,
                                                      corner, feedback)
                        self.assertEqual(
                            max(abs(v) for v in emptied[-256 * channels:]), 0)
                        self.assertEqual(max(abs(v) for v in after), 0)

    def test_a_lowpass_put_back_in_on_a_steady_line_changes_nothing(self):
        """E12 on a live signal: a DC that fills the line, the low-pass out
        and back in while it plays. A state that followed the tap is the DC
        itself, so the output does not move by one LSB; a frozen state jumps
        and a zeroed one dips."""
        node = delay(delay_ms=20.0, feedback=0.0, mix=2.0, damping_hz=900.0)
        node.play(audiocore.RawSample(array("h", [12345] * (4096 * CHANNELS)),
                                      sample_rate=SAMPLE_RATE,
                                      channel_count=CHANNELS))
        words(node, 2)
        node.set(damping_hz=0.0)
        node.play(audiocore.RawSample(array("h", [-7000] * (4096 * CHANNELS)),
                                      sample_rate=SAMPLE_RATE,
                                      channel_count=CHANNELS))
        words(node, 2)                     # the line is all -7000 now
        node.set(damping_hz=900.0)
        self.assertEqual(set(words(node, 2)), {-7000})

    def test_a_highpass_put_back_in_is_a_highpass_put_in_for_the_first_time(
            self):
        """E12 for the cut, on a steady line. At feedback 0 the line holds
        only the input, so a node whose high-pass went in, out and back in
        renders, from the moment it is back, exactly what a node that never
        had one renders once it is put in at the same moment - and that
        starts where the unfiltered output was, then falls away. A frozen
        state subtracts the old DC at once; one that followed the signal
        would too, which is why the high-pass is held at zero instead."""
        switched = delay(delay_ms=20.0, feedback=0.0, mix=2.0, cut_hz=300.0)
        fresh = delay(delay_ms=20.0, feedback=0.0, mix=2.0)
        for node in (switched, fresh):
            node.play(audiocore.RawSample(
                array("h", [12000] * (4096 * CHANNELS)),
                sample_rate=SAMPLE_RATE, channel_count=CHANNELS))
        words(switched, 3)
        words(fresh, 3)
        switched.set(cut_hz=0.0)
        self.assertEqual(set(words(switched, 3)), {12000})
        words(fresh, 3)
        switched.set(cut_hz=300.0)
        fresh.set(cut_hz=300.0)
        back = words(switched, 4)
        self.assertEqual(back, words(fresh, 4))
        # 1 - a of the line on the first frame back, a = 0.21 at 300 Hz.
        self.assertGreater(back[0], 9000)
        self.assertLess(abs(back[-1]), 100)

    def test_the_filter_out_renders_as_no_filter(self):
        """While a filter is out, the render is the render of a node that
        never had it, byte for byte - following the signal must not leak
        into what plays."""
        for option, corner in self.FILTERS:
            with self.subTest(option=option):
                plain = delay(delay_ms=20.0, feedback=0.7, mix=1.0)
                plain.play(alternating())
                switched = delay(delay_ms=20.0, feedback=0.7, mix=1.0,
                                 **{option: corner})
                switched.set(**{option: 0.0})
                switched.play(alternating())
                self.assertEqual(words(switched, 12), words(plain, 12))


if __name__ == "__main__":
    unittest.main()
