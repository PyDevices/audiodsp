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
| E13 | A wow depth moved on a live node glides in over 20 ms, then renders as a node built with it | no step past 1.5 x the tone's own; exact after the ramp |
| E14 | E10 in stereo with a cross-feed strictly between 0 and 1, at the feedbacks where the cross-fed sum lands an ulp above its lanes | exact, 0 LSB |
| E15 | `delay_frames=N` reads the line exactly N frames back, at 44.1, 22.05 and 48 kHz, where no `delay_ms` lands on N | exact: the whole click on frame N, nothing on its neighbours |
| E16 | An upward `loop_semitones` climb does not fold back past Nyquist, at 48, 44.1 and 22.05 kHz | the strongest line under the series, with the loop running, within 10 dB of the shifter's own single pass |
| E17 | A held note through `loop_semitones` plays its shifted pitch, C2 to C6, up and down, at 48, 44.1 and 22.05 kHz | the line at f * r within 3 dB of the note's own level |
| E18 | A held chord through `loop_semitones` keeps every note's shifted line | each within 5.7 dB of the note's own level, PitchShift's worst on the same chords |
| E19 | E18 on chords and intervals no splice within half a window fits: A minor, E minor and D minor in the low register, a major sixth, a major seventh | E18's bar, 5.7 dB |

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
import struct
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


class _Repeating(audiocore._AudioSample):
    """Hands the same short buffer on every pull and never ends."""

    def __init__(self, values):
        self.sample_rate = SAMPLE_RATE
        self.channel_count = 1
        self.bits_per_sample = 16
        self._data = bytes(values)

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        return audiocore.GET_BUFFER_MORE_DATA, self._data


def mono_short(frames=SHORT_FRAMES):
    """Mono, shorter than one DSP chunk, and never zero - so a gap in the
    output is unambiguous. It hands the same buffer on every pull and never
    ends, so a block is several short runs: a `RawSample` ends after one now
    that the node sees `GET_BUFFER_DONE` (audiodsp#180)."""
    values = array("h")
    for frame in range(frames):
        values.append(((frame * 907) % 20000) - 10000)
    return _Repeating(values)


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
        "loop_window_ms": 11.0, "delay_frames": 150.0,
    }

    #: `shift_window_finish` clamps the shift window to a quarter of the
    #: delay line, which is 12.5 ms at this rate and this `max_delay_ms`.
    #: Both values above have to sit under that or the clamp makes them the
    #: same number and the option reads as inert.

    #: A loop that is actually running, so an option that only acts inside it
    #: has something to act on. At feedback 0 most of the table is inert.
    #: The shift is downward: the material alternates at Nyquist, which an
    #: upward shift's band limit (E16) takes out of the loop, and then most of
    #: the table would act on silence.
    BASELINE = dict(delay_ms=10.0, feedback=0.75, mix=2.0, damping_hz=3000.0,
                    cut_hz=100.0, wow_hz=2.0, wow_depth_ms=1.0,
                    cross_feed=0.5, loop_drive=0.4, input_pan=0.3,
                    delay_slew=1.0, wow_am_depth=0.2, loop_semitones=-3.0,
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
            # Long enough to be playing still when the feedback moves.
            node.play(alternating())
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


def sine_words(frames, hz, level, channels=CHANNELS):
    import math
    values = array("h")
    for frame in range(frames):
        value = int(round(level * math.sin(2 * math.pi * hz * frame /
                                           SAMPLE_RATE)))
        for _channel in range(channels):
            values.append(value)
    return values


class WowDepthGlideTest(unittest.TestCase):
    """E13 - audiodsp#160. A new `wow_depth_ms` ramps in over 20 ms.

    It used to land on the next frame, so the read head jumped by the whole
    change between two samples: at the wow's crest a 3 ms move on a 250 Hz
    tone stepped the output far past anything the tone does by itself. Now
    the depth walks there in a straight line, 160 frames at 8 kHz, and lands
    exactly, so from then on the node is the node built with that depth.
    """

    #: The crest of a 0.7 Hz sine is at 0.357 s: block 11 at 8 kHz.
    CREST_BLOCKS = 11

    def _pair(self, depth):
        tone = sine_words(4096, 250.0, 20000)
        moved = delay(max_delay_ms=50, delay_ms=20.0, feedback=0.0, mix=2.0,
                      wow_hz=0.7, wow_depth_ms=0.0)
        built = delay(max_delay_ms=50, delay_ms=20.0, feedback=0.0, mix=2.0,
                      wow_hz=0.7, wow_depth_ms=depth)
        for node in (moved, built):
            node.play(audiocore.RawSample(tone, sample_rate=SAMPLE_RATE,
                                          channel_count=CHANNELS))
        lead = words(moved, self.CREST_BLOCKS)
        words(built, self.CREST_BLOCKS)
        moved.set(wow_depth_ms=depth)
        return lead, words(moved, 4), words(built, 4)

    def test_a_depth_move_glides(self):
        """E13: no sample-to-sample step after the move is more than 1.5 x
        the largest the tone takes on its own."""
        for depth in (0.5, 1.5, 3.0):
            with self.subTest(depth=depth):
                lead, after, _built = self._pair(depth)
                left = lead[0::CHANNELS]
                own = max(abs(b - a) for a, b in zip(left[200:], left[201:]))
                moved = left[-1:] + after[0::CHANNELS]
                step = max(abs(b - a) for a, b in zip(moved, moved[1:]))
                self.assertLess(step, own * 1.5)

    def test_after_the_ramp_it_is_the_node_built_with_that_depth(self):
        """E13: at feedback 0 the line holds only the input, so once the
        160-frame ramp is done the moved node renders exactly what a node
        built with the new depth renders."""
        for depth in (0.5, 3.0):
            with self.subTest(depth=depth):
                _lead, after, built = self._pair(depth)
                self.assertNotEqual(after[:160 * CHANNELS],
                                    built[:160 * CHANNELS])
                self.assertEqual(after[160 * CHANNELS:],
                                 built[160 * CHANNELS:])



class WowDepthRampTimeTest(unittest.TestCase):
    """`wow_depth_ramp_ms` sets how long that glide takes: a depth move walks
    there in a straight line over that many milliseconds, at least one
    frame, and lands exactly. 20 is the default, and the 160 frames above."""

    def _pair(self, depth, **ramp):
        tone = sine_words(4096, 250.0, 20000)
        moved = delay(max_delay_ms=50, delay_ms=20.0, feedback=0.0, mix=2.0,
                      wow_hz=0.7, wow_depth_ms=0.0, **ramp)
        built = delay(max_delay_ms=50, delay_ms=20.0, feedback=0.0, mix=2.0,
                      wow_hz=0.7, wow_depth_ms=depth)
        for node in (moved, built):
            node.play(audiocore.RawSample(tone, sample_rate=SAMPLE_RATE,
                                          channel_count=CHANNELS))
        words(moved, WowDepthGlideTest.CREST_BLOCKS)
        words(built, WowDepthGlideTest.CREST_BLOCKS)
        moved.set(wow_depth_ms=depth)
        return words(moved, 4), words(built, 4)

    def test_the_ramp_takes_the_time_it_is_given(self):
        """100 ms at 8 kHz is 800 frames: different until then, the node
        built with that depth from then on."""
        after, built = self._pair(3.0, wow_depth_ramp_ms=100.0)
        self.assertNotEqual(after[700 * CHANNELS:800 * CHANNELS],
                            built[700 * CHANNELS:800 * CHANNELS])
        self.assertEqual(after[800 * CHANNELS:], built[800 * CHANNELS:])

    def test_a_ramp_set_while_playing_takes_the_next_move(self):
        tone = sine_words(4096, 250.0, 20000)
        node = delay(max_delay_ms=50, delay_ms=20.0, feedback=0.0, mix=2.0,
                     wow_hz=0.7, wow_depth_ms=0.0)
        node.play(audiocore.RawSample(tone, sample_rate=SAMPLE_RATE,
                                      channel_count=CHANNELS))
        words(node, WowDepthGlideTest.CREST_BLOCKS)
        node.set(wow_depth_ramp_ms=50.0, wow_depth_ms=3.0)
        after = words(node, 4)
        built = delay(max_delay_ms=50, delay_ms=20.0, feedback=0.0, mix=2.0,
                      wow_hz=0.7, wow_depth_ms=3.0)
        built.play(audiocore.RawSample(tone, sample_rate=SAMPLE_RATE,
                                       channel_count=CHANNELS))
        words(built, WowDepthGlideTest.CREST_BLOCKS)
        built = words(built, 4)
        self.assertNotEqual(after[300 * CHANNELS:400 * CHANNELS],
                            built[300 * CHANNELS:400 * CHANNELS])
        self.assertEqual(after[400 * CHANNELS:], built[400 * CHANNELS:])

    def test_zero_is_one_frame(self):
        after, built = self._pair(3.0, wow_depth_ramp_ms=0.0)
        self.assertEqual(after[CHANNELS:], built[CHANNELS:])

    def test_the_default_is_twenty(self):
        after, built = self._pair(3.0, wow_depth_ramp_ms=20.0)
        default, _ = self._pair(3.0)
        self.assertEqual(after, default)


def shape_words(sign):
    import math
    return array("h", [int(round(sign * 32767 * math.sin(2 * math.pi * i / 256)))
                       for i in range(256)])


class WowShapeSwapTest(unittest.TestCase):
    """A new `wow_shape` on a swinging wow ramps the depth to zero over
    `wow_depth_ramp_ms`, changes the table there, and ramps back.

    Swapped at once, the read head moved by the depth times the difference
    between the two tables on the next frame, a pitch blip: at the crest of
    a 3 ms swing, a table turned upside down is 6 ms in one sample. At zero
    depth the table moves nothing. 160 frames down and 160 back at 8 kHz,
    then the node is the node built with the new table.
    """

    def _pair(self, **ramp):
        tone = sine_words(4096, 250.0, 20000)
        up = shape_words(1)
        down = shape_words(-1)
        moved = delay(max_delay_ms=50, delay_ms=20.0, feedback=0.0, mix=2.0,
                      wow_hz=0.7, wow_depth_ms=3.0, wow_shape=up, **ramp)
        built = delay(max_delay_ms=50, delay_ms=20.0, feedback=0.0, mix=2.0,
                      wow_hz=0.7, wow_depth_ms=3.0, wow_shape=down)
        for node in (moved, built):
            node.play(audiocore.RawSample(tone, sample_rate=SAMPLE_RATE,
                                          channel_count=CHANNELS))
        lead = words(moved, WowDepthGlideTest.CREST_BLOCKS)
        words(built, WowDepthGlideTest.CREST_BLOCKS)
        moved.set(wow_shape=down)
        return lead, words(moved, 4), words(built, 4)

    def test_a_shape_change_glides(self):
        lead, after, _built = self._pair()
        left = lead[0::CHANNELS]
        own = max(abs(b - a) for a, b in zip(left[200:], left[201:]))
        moved = left[-1:] + after[0::CHANNELS]
        step = max(abs(b - a) for a, b in zip(moved, moved[1:]))
        self.assertLess(step, own * 1.5)

    def test_after_the_dip_it_is_the_node_built_with_that_shape(self):
        _lead, after, built = self._pair()
        self.assertNotEqual(after[300 * CHANNELS:320 * CHANNELS],
                            built[300 * CHANNELS:320 * CHANNELS])
        self.assertEqual(after[320 * CHANNELS:], built[320 * CHANNELS:])

    def test_the_dip_takes_the_ramp_time(self):
        _lead, after, built = self._pair(wow_depth_ramp_ms=50.0)
        self.assertNotEqual(after[700 * CHANNELS:800 * CHANNELS],
                            built[700 * CHANNELS:800 * CHANNELS])
        self.assertEqual(after[800 * CHANNELS:], built[800 * CHANNELS:])

    def test_the_same_table_again_changes_nothing(self):
        tone = sine_words(4096, 250.0, 20000)
        up = shape_words(1)
        nodes = [delay(max_delay_ms=50, delay_ms=20.0, feedback=0.0, mix=2.0,
                       wow_hz=0.7, wow_depth_ms=3.0, wow_shape=up)
                 for _ in range(2)]
        for node in nodes:
            node.play(audiocore.RawSample(tone, sample_rate=SAMPLE_RATE,
                                          channel_count=CHANNELS))
            words(node, WowDepthGlideTest.CREST_BLOCKS)
        nodes[0].set(wow_shape=up)
        self.assertEqual(words(nodes[0], 4), words(nodes[1], 4))

    def test_a_shape_at_zero_depth_goes_in_at_once(self):
        tone = sine_words(4096, 250.0, 20000)
        moved = delay(max_delay_ms=50, delay_ms=20.0, feedback=0.0, mix=2.0,
                      wow_hz=0.7, wow_depth_ms=0.0, wow_shape=shape_words(1))
        built = delay(max_delay_ms=50, delay_ms=20.0, feedback=0.0, mix=2.0,
                      wow_hz=0.7, wow_depth_ms=0.0, wow_shape=shape_words(-1))
        for node in (moved, built):
            node.play(audiocore.RawSample(tone, sample_rate=SAMPLE_RATE,
                                          channel_count=CHANNELS))
            words(node, 2)
        moved.set(wow_shape=shape_words(-1), wow_depth_ms=3.0)
        built.set(wow_depth_ms=3.0)
        self.assertEqual(words(moved, 4), words(built, 4))


def _f32(value):
    return struct.unpack("f", struct.pack("f", value))[0]


def _f32_step(value, steps):
    bits = struct.unpack("I", struct.pack("f", value))[0]
    return struct.unpack("f", struct.pack("I", bits + steps))[0]


def crossfeed_stall_cells():
    """The (s, k, feedback) cells where audiodsp main's float32 arithmetic
    handed a landed stereo line of k back: cross_feed s / 127, both lanes at
    k, feedback within three float32 steps of 1 - 0.5 / k. Worked out here in
    the node's own float32 order (each product rounded, then the sum; the
    stall test at 0.5 as it was), so the cells do not depend on the build
    under test."""
    def write(sent, feedback):
        back = _f32(feedback * sent)
        if _f32(abs(sent) - abs(back)) <= 0.5:
            back = float(int(back))
        return int(_f32(back + 0.5))

    cells = []
    for s in range(1, 127):
        crossed = _f32(s / 127.0)
        direct = _f32(1.0 - crossed)
        for k in range(1, 51):
            sent = _f32(_f32(k * direct) + _f32(k * crossed))
            if not sent > k:
                continue
            centre = _f32(1.0 - 0.5 / k)
            for steps in range(-3, 4):
                feedback = _f32_step(centre, steps)
                if write(sent, feedback) >= k > write(float(k), feedback):
                    cells.append((s, k, feedback))
    return cells


class CrossFedTailReachesZeroTest(unittest.TestCase):
    """E14 - audiodsp#170. E10 in stereo with the cross-feed strictly
    between 0 and 1.

    With both lanes of the line at the same whole k, each lane sends
    `k * (1 - cross_feed) + k * cross_feed` round the loop. That is k, but
    the two float32 products could sum to an ulp above it; one or two float32
    steps under f = 1 - 0.5 / k the stall test then saw a gap a hair over 0.5,
    rounded, and wrote k back on every lap: 117 cells on the 7-bit cross_feed
    grid, 9 to 50 LSB held for ever, while the same feedback ended at
    cross_feed 0 and in mono. Within 2^-14 of that edge the node now works
    out the write and keeps the rounding only if it is smaller than the
    larger lane, which holds however the sum is rounded and leaves every
    other write as it was.
    """

    def _held(self, rate, delay_ms, feedback, cross_feed, level,
              channels=2, silent_blocks=40):
        lap = int(rate * delay_ms / 1000.0 + 0.5)
        lead = (4 * lap + 255) // 256 * 256
        data = array("h", [level] * (lead * channels))
        data.extend(array("h", bytes(2 * channels * 256 * silent_blocks)))
        node = audioecho.FeedbackDelay(
            sample_rate=rate, channel_count=channels,
            max_delay_ms=delay_ms + 2.0, delay_ms=delay_ms,
            feedback=feedback, mix=2.0, cross_feed=cross_feed)
        node.play(audiocore.RawSample(data, sample_rate=rate,
                                      channel_count=channels))
        rendered = words(node, lead // 256 + silent_blocks)
        self.assertGreater(max(abs(v) for v in rendered[:lead * channels]),
                           0)
        return max(abs(v) for v in rendered[-lap * channels:])

    def test_the_search_finds_the_cells_it_was_written_for(self):
        """The cells below are the 117 AnalogDelay's audit found."""
        cells = crossfeed_stall_cells()
        self.assertEqual(len(cells), 117)
        self.assertEqual(len(set(k for _s, k, _f in cells)), 30)

    def test_a_cross_fed_stereo_tail_reaches_exact_zero(self):
        """E14: every cell, both signs, a 1 ms line at 8 kHz so a lap is
        eight frames: the last lap is silent."""
        for s, k, feedback in crossfeed_stall_cells():
            for level in (2 * k + 2, -(2 * k + 2)):
                with self.subTest(s=s, k=k, feedback=feedback, level=level):
                    self.assertEqual(self._held(8000, 1.0, feedback,
                                                s / 127.0, level), 0)

    def test_the_reported_cells_at_48k(self):
        """E14 as it was found: 48 kHz, 20 ms, and the typed feedback
        0.9899999 with cross_feed 39 / 127 that held 50 LSB. Four laps of
        DC build the line to about 4 x the level, and at 0.99 that takes
        some 300 laps to come down; 1 600 blocks is 426 laps."""
        for feedback, s, k in ((0.9444443583, 1, 9), (0.9545453787, 3, 11),
                               (0.9899999499, 2, 50), (0.9899999, 39, 50),
                               (0.9666666, 21, 15)):
            with self.subTest(feedback=feedback, s=s):
                self.assertEqual(self._held(48000, 20.0, feedback, s / 127.0,
                                            2 * k + 2, silent_blocks=1600), 0)

    def test_the_controls_end_as_they_did(self):
        """Cross-feed 0 and 1 and mono ended before the fix and still do."""
        for s, k, feedback in crossfeed_stall_cells()[::9]:
            for cross_feed, channels in ((0.0, 2), (1.0, 2),
                                         (s / 127.0, 1)):
                with self.subTest(s=s, k=k, cross_feed=cross_feed,
                                  channels=channels):
                    self.assertEqual(self._held(8000, 1.0, feedback,
                                                cross_feed, 2 * k + 2,
                                                channels=channels), 0)


class DelayFramesTest(unittest.TestCase):
    """E15, audiodsp#179. `delay_ms` becomes frames as `ms * rate / 1000` in
    float32, and at 44.1 kHz no float32 millisecond value lands on 16457: the
    read sits a fraction of a frame off and splits a click across two frames.
    `delay_frames` is the read offset itself."""

    #: (rate, frames). 16457 and 8231 are frames no float32 `delay_ms`
    #: within 64 ulps lands on; 17913 at 48 kHz is the control rate.
    CELLS = ((44100, 16457), (22050, 8231), (48000, 17913))

    @staticmethod
    def _click_at(rate, **options):
        frames = int(options.pop("frames")) + 512
        values = array("h", [0] * (frames * 2))
        values[0] = values[1] = 20000
        source = audiocore.RawSample(values, sample_rate=rate,
                                     channel_count=2)
        node = audioecho.FeedbackDelay(sample_rate=rate, max_delay_ms=600.0,
                                       feedback=0.0, mix=2.0, **options)
        node.play(source)
        heard = {}
        frame = 0
        while frame < frames:
            data = array("h", bytes(audiocore.get_buffer(node)[1]))
            for index in range(0, len(data), 2):
                if data[index]:
                    heard[frame] = data[index]
                frame += 1
        return heard

    def test_delay_frames_lands_on_the_whole_frame(self):
        for rate, frames in self.CELLS:
            with self.subTest(rate=rate):
                self.assertEqual(
                    self._click_at(rate, frames=frames, delay_frames=frames),
                    {frames: 20000})

    def test_E15_discriminates_a_split_read(self):
        """The control: through `delay_ms` the same frame at 44.1 kHz is a
        split read, so the bar above can fail."""
        heard = self._click_at(44100, frames=16457,
                               delay_ms=16457 * 1000.0 / 44100)
        self.assertGreater(len(heard), 1)

    def test_both_units_in_one_call_is_refused(self):
        with self.assertRaises(TypeError):
            delay(delay_ms=10.0, delay_frames=80)
        node = delay()
        with self.assertRaises(TypeError):
            node.set(delay_ms=10.0, delay_frames=80)


try:
    import numpy as np
except ImportError:     # pragma: no cover - numpy is a CI dependency
    np = None


@unittest.skipIf(np is None, "E16 measures a spectrum and needs numpy")
class UpwardShiftBandLimitTest(unittest.TestCase):
    """E16. Read at a ratio r, a line holding f plays f * r, so anything on
    the line above fs / (2 r) comes back past Nyquist, folded, as a tone that
    is not in the series, and then climbs from there. Before the band limit,
    ShimmerHall's Rise did exactly that at its edges.

    The measure: a short tone burst through the loop, wet only, and the
    strongest spectral line below the first shifted tone, against the
    strongest line of all. A shifted line is never below the first one, so
    anything there is either a fold or the shifter's own splice artefacts.
    The second are there on a single pass too (feedback 0), so the loop is
    held to that level plus 10 dB. Unlimited, these cells measured 12.6 to 32 dB
    over it; limited, 6 or less."""

    #: (rate, tone Hz, semitones).
    CELLS = ((48000, 1500, 24), (44100, 1500, 24), (22050, 1500, 24),
             (48000, 1700, 12), (48000, 1100, 19), (44100, 1100, 19))

    @staticmethod
    def _render(rate, tone, semitones, feedback):
        burst = int(0.06 * rate)
        values = array("h", [int(12000 * np.sin(2 * np.pi * tone * n / rate))
                             for n in range(burst)])
        values.extend(array("h", [0]) * int(1.6 * rate))
        node = audioecho.FeedbackDelay(
            sample_rate=rate, channel_count=1, max_delay_ms=200.0,
            delay_ms=100.0, feedback=feedback, mix=2.0,
            loop_semitones=semitones)
        node.play(audiocore.RawSample(values, sample_rate=rate))
        out = array("h")
        while len(out) < len(values):
            out.frombytes(bytes(audiocore.get_buffer(node)[1]))
        return np.array(out[burst:len(values)], dtype=float)

    @staticmethod
    def _stray_db(signal, rate, below):
        spectrum = np.abs(np.fft.rfft(signal * np.hanning(len(signal))))
        hz = np.fft.rfftfreq(len(signal), 1.0 / rate)
        stray = spectrum[(hz > 20.0) & (hz < below)].max()
        return 20.0 * np.log10(stray / spectrum.max())

    def test_upward_climb_does_not_fold(self):
        for rate, tone, semitones in self.CELLS:
            with self.subTest(rate=rate, tone=tone, semitones=semitones):
                below = tone * 2.0 ** (semitones / 12.0) * 0.85
                single = self._stray_db(
                    self._render(rate, tone, semitones, 0.0), rate, below)
                looped = self._stray_db(
                    self._render(rate, tone, semitones, 0.6), rate, below)
                self.assertLessEqual(looped, single + 10.0,
                                     "%.1f dB against %.1f on one pass"
                                     % (looped, single))
@unittest.skipIf(np is None, "E17 measures a spectrum and needs numpy")
class HeldNoteSpliceTest(unittest.TestCase):
    """E17. The shifter reads the line through two taps half a window apart
    and crossfades between them. Fixed there, the two were in antiphase on
    every note whose period goes into half the window an odd number of
    halves - G3 against the 25 ms default - and each crossfade cancelled
    the note: what came out was its neighbours a grain rate either side, not
    the shifted note. Each tap is now placed in phase with the other when it
    starts again from zero gain.

    The measure: a held note, one pass (feedback 0, wet only), and the level
    of the spectral line at the shifted pitch against the note's own. Before
    the splice was aligned, the median note at +12 measured 54 dB under."""

    #: (rate, semitones). Every note from C2 (36) to C6 (84) in each.
    CELLS = ((48000, 12.0), (48000, 7.0), (44100, -12.0), (22050, 12.0))
    LEVEL = 10000

    #: (rate, semitones, loop_window_ms, delay_ms): short windows, which
    #: splice often and leave little room to move, and delays shorter than
    #: the stretch the splice compares.
    EDGE_CELLS = ((44100, 12.0, 3.0, 50.0), (44100, 12.0, 5.0, 50.0),
                  (44100, -12.0, 8.0, 50.0), (44100, 7.0, 10.0, 50.0),
                  (48000, 12.0, 25.0, 5.0), (48000, 24.0, 25.0, 8.0),
                  (22050, 12.0, 25.0, 3.0))

    def _level_db(self, rate, midi, semitones, window_ms=25.0, delay_ms=50.0):
        hz = 440.0 * 2.0 ** ((midi - 69) / 12.0)
        frames = rate
        values = array("h", [int(self.LEVEL * np.sin(2 * np.pi * hz * n / rate))
                             for n in range(frames)])
        node = audioecho.FeedbackDelay(
            sample_rate=rate, channel_count=1, max_delay_ms=200.0,
            delay_ms=delay_ms, feedback=0.0, mix=2.0,
            loop_semitones=semitones, loop_window_ms=window_ms)
        node.play(audiocore.RawSample(values, sample_rate=rate))
        out = array("h")
        while len(out) < frames:
            out.frombytes(bytes(audiocore.get_buffer(node)[1]))
        held = np.array(out[int(0.3 * rate):frames], dtype=float)
        window = np.hanning(len(held))
        spectrum = np.abs(np.fft.rfft(held * window)) / (window.sum() / 2)
        bins = np.fft.rfftfreq(len(held), 1.0 / rate)
        shifted = hz * 2.0 ** (semitones / 12.0)
        near = (bins > shifted * 0.985) & (bins < shifted * 1.015)
        return 20.0 * np.log10(spectrum[near].max() / self.LEVEL + 1e-12)

    def test_held_notes_play_their_shifted_pitch(self):
        for rate, semitones in self.CELLS:
            for midi in range(36, 85):
                with self.subTest(rate=rate, semitones=semitones, midi=midi):
                    self.assertGreater(self._level_db(rate, midi, semitones),
                                       -3.0)

    def test_short_windows_and_delays_play_their_shifted_pitch(self):
        for rate, semitones, window_ms, delay_ms in self.EDGE_CELLS:
            for midi in range(36, 85, 2):
                with self.subTest(rate=rate, semitones=semitones,
                                  window_ms=window_ms, delay_ms=delay_ms,
                                  midi=midi):
                    self.assertGreater(
                        self._level_db(rate, midi, semitones, window_ms,
                                       delay_ms), -3.0)



@unittest.skipIf(np is None, "E18 measures a spectrum and needs numpy")
class HeldChordSpliceTest(unittest.TestCase):
    """E18. A splice that matches one note's phase can leave another's in
    antiphase, so a chord can lose a note at every splice even where each
    note alone is held (E17). Measured as E17 is, one pass at +12, each
    note's line at twice its pitch against its own level. Before the splice
    compared a stretch long enough to hold the chord's root, a C3 triad's
    root came out 22.7 dB down and a C2 power chord's 23.0. The bar is
    `audiodelays.PitchShift`'s worst on the same four chords, 5.7 dB."""

    RATE = 48000
    LEVEL = 3333
    CHORDS = (("C3 triad", (48, 52, 55)), ("G2 and D4", (43, 62)),
              ("A2 and A#2", (45, 46)), ("C2 power chord", (36, 43, 48)))

    def _levels(self, midis, semitones=12.0):
        rate = self.RATE
        hz = [440.0 * 2.0 ** ((m - 69) / 12.0) for m in midis]
        n = np.arange(rate)
        mixed = sum(self.LEVEL * np.sin(2 * np.pi * h * n / rate) for h in hz)
        values = array("h", mixed.astype(np.int16).tolist())
        node = audioecho.FeedbackDelay(
            sample_rate=rate, channel_count=1, max_delay_ms=200.0,
            delay_ms=50.0, feedback=0.0, mix=2.0, loop_semitones=semitones)
        node.play(audiocore.RawSample(values, sample_rate=rate))
        out = array("h")
        while len(out) < rate:
            out.frombytes(bytes(audiocore.get_buffer(node)[1]))
        held = np.array(out[int(0.3 * rate):rate], dtype=float)
        window = np.hanning(len(held))
        spectrum = np.abs(np.fft.rfft(held * window)) / (window.sum() / 2)
        bins = np.fft.rfftfreq(len(held), 1.0 / rate)
        ratio = 2.0 ** (semitones / 12.0)
        return [20.0 * np.log10(spectrum[(bins > ratio * h * 0.99) &
                                         (bins < ratio * h * 1.01)].max() /
                                self.LEVEL + 1e-12) for h in hz]

    def test_held_chords_keep_every_note(self):
        for name, midis in self.CHORDS:
            for midi, level in zip(midis, self._levels(midis)):
                with self.subTest(chord=name, midi=midi):
                    self.assertGreaterEqual(level, -5.7)


@unittest.skipIf(np is None, "E19 measures a spectrum and needs numpy")
class UnfitChordSpliceTest(HeldChordSpliceTest):
    """E19, audiodsp#246. Some chords have no offset within half a window
    that holds every note in phase. The splice searched only that far, took
    the least bad offset, and the note it fitted worst came and went: A2, C3
    and E3 at +12 lost A2 by 19.5 dB and C3 by 13.1, F2 with D3 lost F2 by
    60 dB. Where the best match that close still differs by more than an
    eighth of the line's level, the splice now searches a whole window
    either way, and every note here is back within E18's bar (1.4 dB at
    worst)."""

    #: (loop_semitones, name, notes).
    CASES = ((12.0, "A2 minor", (45, 48, 52)),
             (12.0, "E3 minor", (52, 55, 59)),
             (12.0, "D3 minor", (50, 53, 57)),
             (12.0, "F2 and D3", (41, 51)),
             (12.0, "E3 and D#4", (52, 63)),
             (7.0, "F2 and D3", (41, 51)),
             (7.0, "E3 and D#4", (52, 63)))

    def test_held_chords_keep_every_note(self):
        for semitones, name, midis in self.CASES:
            for midi, level in zip(midis, self._levels(midis, semitones)):
                with self.subTest(semitones=semitones, chord=name, midi=midi):
                    self.assertGreaterEqual(level, -5.7)


if __name__ == "__main__":
    unittest.main()
