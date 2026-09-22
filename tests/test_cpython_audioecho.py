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


if __name__ == "__main__":
    unittest.main()
