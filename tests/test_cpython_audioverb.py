"""audioverb.Tank: the surface the parity gate cannot check.

What the tank *renders* is pinned across every interpreter by
tests/parity/tank_probe.py through verify_dsp.py. What is left for here is
everything that comparison cannot reach: the argument forms, the errors, the
things a Python-defined audiosample can do to it, and the two behaviours the
module exists for -- a network whose topology comes from Python, and a tail
that reaches exact zero rather than sitting at one LSB forever.
"""

import unittest
from array import array

import audiocore
import audioverb

SAMPLE_RATE = 8000


def source(frames=3000, level=18000, burst=160, channels=2):
    values = array("h")
    for frame in range(frames):
        for channel in range(channels):
            if frame < burst:
                shape = ((frame * (97 + channel * 18)) % 2001) - 1000
                values.append(shape * level // 1000)
            else:
                values.append(0)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def render(node, blocks, channels=2):
    return b"".join(bytes(audiocore.get_buffer(node)[1])
                    for _ in range(blocks))


def samples(data):
    values = array("h")
    values.frombytes(data)
    return values


class TankSurface(unittest.TestCase):
    def test_mix_zero_is_a_wire(self):
        """The whole network runs and the output is the input, sample for
        sample -- which is what makes `mix` safe to automate down to nothing."""
        node = audioverb.Tank(sample_rate=SAMPLE_RATE, max_predelay_ms=40.0,
                              mix=0.0, decay=0.9, mod_rate_hz=1.1,
                              mod_depth_ms=0.3)
        node.play(source(burst=3000))
        rendered = render(node, 6)
        reference = bytes(audiocore.get_buffer(source(burst=3000))[1])
        self.assertEqual(rendered, reference[:len(rendered)])

    def test_tail_reaches_exact_zero(self):
        """Every line write is a magnitude truncation, so a loop under unity
        gain strictly loses magnitude every pass. A rounding quantiser leaves
        the network humming at one LSB for as long as it is pulled."""
        node = audioverb.Tank(sample_rate=SAMPLE_RATE, max_predelay_ms=40.0,
                              mix=2.0, decay=0.4)
        node.play(source(frames=60000, burst=160))
        silent = None
        for index in range(200):
            data = bytes(audiocore.get_buffer(node)[1])
            if not any(data):
                silent = index
                break
        self.assertIsNotNone(silent, "the tail never reached exact zero")
        # And it stays there: nothing re-excites a network with no input.
        for _ in range(8):
            self.assertFalse(any(bytes(audiocore.get_buffer(node)[1])))

    def test_reverberates_and_decays(self):
        node = audioverb.Tank(sample_rate=SAMPLE_RATE, max_predelay_ms=40.0,
                              mix=2.0, decay=0.6)
        node.play(source())
        peaks = [max(abs(value) for value in samples(
            bytes(audiocore.get_buffer(node)[1]))) for _ in range(10)]
        # Something arrives after the 160-frame burst has gone by ...
        self.assertGreater(max(peaks[1:]), 0)
        # ... and the second half of the run is quieter than the first.
        self.assertLess(max(peaks[6:]), max(peaks[:4]))

    def test_predelay_moves_the_wet_onset(self):
        def onset(node):
            values = samples(render(node, 8))
            for index, value in enumerate(values):
                if abs(value) > 8:
                    return index // 2
            return None

        plain = audioverb.Tank(sample_rate=SAMPLE_RATE, max_predelay_ms=60.0,
                               mix=2.0, decay=0.3)
        plain.play(source())
        delayed = audioverb.Tank(sample_rate=SAMPLE_RATE, max_predelay_ms=60.0,
                                 mix=2.0, decay=0.3, predelay_ms=20.0)
        delayed.play(source())
        self.assertEqual(onset(delayed) - onset(plain),
                         int(0.020 * SAMPLE_RATE))

    def test_python_supplied_topology_renders(self):
        delays = [16, 12, 40, 28, 70, 460, 190, 380, 95, 430, 275, 330]
        taps = [0, 9, 27, 0.6, 0, 5, 200, -0.6, 1, 5, 36, 0.6, 1, 11, 12, -0.6]
        node = audioverb.Tank(sample_rate=SAMPLE_RATE, max_predelay_ms=20.0,
                              delays=delays, taps=taps, mix=2.0, decay=0.6)
        node.play(source())
        custom = render(node, 6)
        stock = audioverb.Tank(sample_rate=SAMPLE_RATE, max_predelay_ms=20.0,
                               mix=2.0, decay=0.6)
        stock.play(source())
        self.assertNotEqual(custom, render(stock, 6))
        self.assertTrue(any(custom))

    def test_set_changes_the_render_without_emptying_the_lines(self):
        node = audioverb.Tank(sample_rate=SAMPLE_RATE, max_predelay_ms=40.0,
                              mix=2.0, decay=0.75)
        node.play(source())
        before = render(node, 3)
        node.set(damping_hz=600.0, decay=0.3)
        after = render(node, 3)
        self.assertTrue(any(after), "the lines were emptied by set()")
        self.assertNotEqual(before, after)
        node.clear()
        self.assertFalse(any(render(node, 1)),
                         "clear() left something in the lines")

    def test_starved_gives_silence_not_a_short_block(self):
        node = audioverb.Tank(sample_rate=SAMPLE_RATE, max_predelay_ms=20.0)
        for _ in range(3):
            result, data = audiocore.get_buffer(node)
            data = bytes(data)
            self.assertEqual(result, audiocore.GET_BUFFER_MORE_DATA)
            self.assertEqual(len(data), audioverb.FRAMES * 4)
            self.assertFalse(any(data))

    def test_mono(self):
        node = audioverb.Tank(sample_rate=SAMPLE_RATE, max_predelay_ms=20.0,
                              channel_count=1, mix=2.0, decay=0.6)
        node.play(source(channels=1))
        data = render(node, 4)
        self.assertEqual(len(data), 4 * audioverb.FRAMES * 2)
        self.assertTrue(any(data))


class TankArguments(unittest.TestCase):
    def test_delays_must_be_twelve(self):
        with self.assertRaises(ValueError):
            audioverb.Tank(sample_rate=SAMPLE_RATE, delays=[100] * 11)

    def test_every_line_needs_room_for_the_interpolator(self):
        with self.assertRaises(ValueError):
            audioverb.Tank(sample_rate=SAMPLE_RATE, delays=[3] * 12)

    def test_taps_come_in_fours(self):
        with self.assertRaises(ValueError):
            audioverb.Tank(sample_rate=SAMPLE_RATE, taps=[0, 5, 10])

    def test_a_tap_cannot_name_a_line_that_does_not_exist(self):
        with self.assertRaises(ValueError):
            audioverb.Tank(sample_rate=SAMPLE_RATE, taps=[0, 99, 10, 0.5])

    def test_a_tap_cannot_name_a_third_channel(self):
        with self.assertRaises(ValueError):
            audioverb.Tank(sample_rate=SAMPLE_RATE, taps=[2, 5, 10, 0.5])

    def test_a_tap_cannot_reach_past_its_line(self):
        with self.assertRaises(ValueError):
            audioverb.Tank(sample_rate=SAMPLE_RATE, taps=[0, 5, 10 ** 9, 0.5])

    def test_channel_count(self):
        with self.assertRaises(ValueError):
            audioverb.Tank(sample_rate=SAMPLE_RATE, channel_count=3)

    def test_negative_predelay(self):
        with self.assertRaises(ValueError):
            audioverb.Tank(sample_rate=SAMPLE_RATE, max_predelay_ms=-1.0)

    def test_unknown_option(self):
        with self.assertRaises(TypeError):
            audioverb.Tank(sample_rate=SAMPLE_RATE, roomsize=0.5)

    def test_set_refuses_what_construction_fixed(self):
        """`delays` and `taps` re-cut in place (RecutTest); these three are
        the node's shape and stay fixed."""
        node = audioverb.Tank(sample_rate=SAMPLE_RATE)
        for name in ("sample_rate", "channel_count", "max_predelay_ms"):
            with self.assertRaises(TypeError):
                node.set(**{name: 1})

    def test_options_clamp_rather_than_raise(self):
        """A knob past its bound is held at the bound, the way every other
        node in the palette treats one."""
        node = audioverb.Tank(sample_rate=SAMPLE_RATE, max_predelay_ms=40.0,
                              decay=9.0, diffusion=9.0, mix=9.0, width=9.0,
                              drive=9.0, tone_db=900.0, predelay_ms=9e5,
                              mod_depth_ms=9e5, mod_rate_hz=9e5)
        node.play(source())
        self.assertEqual(len(render(node, 2)), 2 * audioverb.FRAMES * 4)


def noise(frames, channels, seed=7, level=12000):
    values = array("h")
    state = seed
    for _ in range(frames * channels):
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        values.append(((state >> 8) % (2 * level + 1)) - level)
    return values


class ToneStateTest(unittest.TestCase):
    """T1-T2, audiodsp#168: the tilt's pole keeps tracking while `tone_db`
    is 0, so Tone moved off 0 comes in from the signal and not from a state
    frozen when it reached 0.

    T1  Tone out, silence until the wet is exact zero, Tone back in with
        nothing playing: every output sample is 0. Main: 1 382 LSB stereo,
        707 mono at 48 kHz (tank_fix_repro.py); here at 8 kHz both widths
        fail.
    T2  Tone out and back in while the input plays: from the move on, the
        node renders byte for byte what a node handed 2^-24 dB instead of 0
        renders (both tilt gains round to exactly 1 there, so that node is
        flat and its pole never stops). Main differs for tens of frames;
        so does a pole that follows the signal while out, and one held at 0.
    """

    RATE = 8000
    OPTIONS = dict(decay=0.7, diffusion=0.75, damping_hz=2500.0,
                   bandwidth_hz=3500.0, mod_depth_ms=0.2, mod_rate_hz=1.0,
                   mix=0.5, max_predelay_ms=20.0)

    def _node(self, channels, tone_db):
        return audioverb.Tank(sample_rate=self.RATE, channel_count=channels,
                              tone_db=tone_db, **self.OPTIONS)

    def test_tone_back_in_after_silence_plays_nothing(self):
        for channels in (2, 1):
            for before, after in ((12.0, 12.0), (-12.0, 3.0)):
                with self.subTest(channels=channels, before=before,
                                  after=after):
                    node = self._node(channels, before)
                    node.play(audiocore.RawSample(
                        noise(4 * audioverb.FRAMES, channels),
                        sample_rate=self.RATE, channel_count=channels))
                    render(node, 3)
                    node.set(tone_db=0.0)
                    render(node, 1)
                    node.play(audiocore.RawSample(
                        array("h", bytes(2 * audioverb.FRAMES * channels)),
                        sample_rate=self.RATE, channel_count=channels))
                    quiet = 0
                    for _ in range(400):
                        quiet = quiet + 1 if not any(render(node, 1)) else 0
                        if quiet == 4:
                            break
                    self.assertEqual(quiet, 4, "the tail never died")
                    node.set(tone_db=after)
                    peak = max(abs(value) for value in
                               samples(render(node, 2)))
                    self.assertEqual(peak, 0)

    def test_tone_back_in_matches_a_pole_that_never_stopped(self):
        for channels in (2, 1):
            for before, after in ((12.0, 12.0), (-12.0, 6.0), (6.0, -3.0)):
                with self.subTest(channels=channels, before=before,
                                  after=after):
                    material = noise(12 * audioverb.FRAMES, channels, seed=11)
                    outputs = []
                    for held in (0.0, 1.0 / 16777216.0):
                        node = self._node(channels, before)
                        node.play(audiocore.RawSample(
                            material, sample_rate=self.RATE,
                            channel_count=channels))
                        render(node, 3)
                        node.set(tone_db=held)
                        render(node, 3)
                        node.set(tone_db=after)
                        outputs.append(render(node, 3))
                    self.assertTrue(any(outputs[0]))
                    self.assertEqual(outputs[0], outputs[1])


class RecutTest(unittest.TestCase):
    """R1-R4, audiodsp#169: `set(delays=..., taps=...)` re-cuts a playing
    node in place, so a class that changes a reverb's size or character no
    longer builds a new node and loses the source frames the old one held.

    R1  At `mix=0` the output is the source byte for byte across a re-cut,
        on a 1024-frame source (the node holds 768 frames between blocks)
        and on a RawSample handed whole (it holds all of it). Main refuses
        the keywords; a rebuilt node is 512 frames ahead of the source
        (tank_fix_repro.py).
    R2  From the re-cut on, the node renders byte for byte what a node built
        on the new tables renders from the same source frame, with options
        handed in the same call applied too: longer, shorter and same-size
        networks, stereo and mono, with the modulation deep enough that the
        new lines' ceiling holds it. Lines and filters that kept their
        contents, a modulation oscillator that kept its phase, or a ceiling
        from the old lines would each show.
    R3  A refused re-cut (eleven lines, a tap past its line, a bad option in
        the same call) raises and leaves the node exactly as it was.
    R4  A re-cut on `taps` alone keeps the lines' lengths and still starts
        the network empty.
    """

    RATE = 8000
    OPTIONS = dict(decay=0.7, diffusion=0.7, damping_hz=2500.0,
                   bandwidth_hz=3500.0, mod_depth_ms=0.3, mod_rate_hz=1.3,
                   tone_db=3.0, mix=0.5, max_predelay_ms=20.0,
                   predelay_ms=5.0)
    LINES = [16, 12, 40, 28, 70, 460, 190, 380, 95, 430, 275, 330]
    TAPS = [0, 9, 27, 0.6, 0, 5, 200, -0.6, 0, 7, 100, 0.6,
            1, 5, 36, 0.6, 1, 11, 12, -0.6, 1, 10, 90, -0.6]

    def _scaled(self, factor):
        lines = [max(4, int(v * factor)) for v in self.LINES]
        taps = list(self.TAPS)
        for index in range(0, len(taps), 4):
            taps[index + 2] = min(int(taps[index + 2] * factor),
                                  lines[taps[index + 1]] - 1)
        return lines, taps

    def _same_size(self):
        """The same total, cut differently: the buffer is reused."""
        lines = list(self.LINES)
        lines[5] -= 30
        lines[9] += 30
        return lines, list(self.TAPS)

    def test_the_dry_does_not_skip(self):
        for block in (1024, 0):
            for channels in (2, 1):
                with self.subTest(block=block, channels=channels):
                    material = noise(10 * audioverb.FRAMES, channels, seed=5)
                    raw = audiocore.RawSample(material, sample_rate=self.RATE,
                                              channel_count=channels)
                    source = raw
                    if block:
                        import audiofilters
                        source = audiofilters.Filter(
                            filter=None, mix=1, buffer_size=block * channels * 2,
                            sample_rate=self.RATE, bits_per_sample=16,
                            samples_signed=True, channel_count=channels)
                        source.play(raw, loop=False)
                    options = dict(self.OPTIONS, mix=0.0)
                    node = audioverb.Tank(sample_rate=self.RATE,
                                          channel_count=channels, **options)
                    node.play(source)
                    head = render(node, 3)
                    lines, taps = self._scaled(1.3)
                    node.set(delays=lines, taps=taps)
                    tail = render(node, 6)
                    self.assertEqual(head + tail,
                                     bytes(material)[:len(head + tail)])

    def test_a_recut_node_is_a_new_node_on_the_same_source(self):
        cuts = {"longer": self._scaled(1.4), "shorter": self._scaled(0.3),
                "same size": self._same_size()}
        for name, (lines, taps) in cuts.items():
            for channels in (2, 1):
                with self.subTest(cut=name, channels=channels):
                    material = noise(12 * audioverb.FRAMES, channels, seed=9)
                    node = audioverb.Tank(sample_rate=self.RATE,
                                          channel_count=channels,
                                          delays=self.LINES, taps=self.TAPS,
                                          **self.OPTIONS)
                    node.play(audiocore.RawSample(
                        material, sample_rate=self.RATE,
                        channel_count=channels))
                    render(node, 4)
                    # 2 ms of modulation is 16 frames: past the shorter
                    # cut's ceiling (9.5 frames on its 21-frame line), inside
                    # the old lines' (34).
                    node.set(delays=lines, taps=taps, decay=0.5,
                             mod_depth_ms=2.0)
                    moved = render(node, 5)

                    options = dict(self.OPTIONS, decay=0.5, mod_depth_ms=2.0)
                    fresh = audioverb.Tank(sample_rate=self.RATE,
                                           channel_count=channels,
                                           delays=lines, taps=taps, **options)
                    start = 4 * audioverb.FRAMES * channels
                    fresh.play(audiocore.RawSample(
                        material[start:], sample_rate=self.RATE,
                        channel_count=channels))
                    self.assertTrue(any(moved))
                    self.assertEqual(moved, render(fresh, 5))

    def test_a_refused_recut_changes_nothing(self):
        refusals = (dict(delays=[100] * 11), dict(taps=[0, 5, 10 ** 6, 0.5]),
                    dict(delays=self._scaled(1.4)[0], roomsize=0.5),
                    dict(delays=self._scaled(0.5)[0], taps=[0, 5, 999, 0.5]))
        for refusal in refusals:
            with self.subTest(refusal=sorted(refusal)):
                material = noise(8 * audioverb.FRAMES, 2, seed=3)
                renders = []
                for refuse in (True, False):
                    node = audioverb.Tank(sample_rate=self.RATE,
                                          delays=self.LINES, taps=self.TAPS,
                                          **self.OPTIONS)
                    node.play(audiocore.RawSample(
                        material, sample_rate=self.RATE, channel_count=2))
                    out = render(node, 3)
                    if refuse:
                        with self.assertRaises((ValueError, TypeError)):
                            node.set(**refusal)
                    renders.append(out + render(node, 4))
                self.assertEqual(renders[0], renders[1])

    def test_taps_alone_start_the_network_empty(self):
        material = noise(8 * audioverb.FRAMES, 2, seed=4)
        taps = list(self.TAPS)
        taps[2] = 5
        node = audioverb.Tank(sample_rate=self.RATE, delays=self.LINES,
                              taps=self.TAPS, **self.OPTIONS)
        node.play(audiocore.RawSample(material, sample_rate=self.RATE,
                                      channel_count=2))
        render(node, 3)
        node.set(taps=taps)
        moved = render(node, 4)
        fresh = audioverb.Tank(sample_rate=self.RATE, delays=self.LINES,
                               taps=taps, **self.OPTIONS)
        fresh.play(audiocore.RawSample(
            material[3 * audioverb.FRAMES * 2:], sample_rate=self.RATE,
            channel_count=2))
        self.assertEqual(moved, render(fresh, 4))


if __name__ == "__main__":
    unittest.main()


class UniversalTraitTest(unittest.TestCase):
    """V12-V14 - the traits every audiodsp-own node carries."""

    RATE = 8000
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
        """V12, this module's form of the identity trait. `mix=0` is already
        covered elsewhere in this file on ordinary material; what is new here is
        **at +/-32767**, which is where an arithmetic width error shows and a
        range check does not. Measured 0 LSB."""
        node = audioverb.Tank(sample_rate=self.RATE, channel_count=self.CHANNELS,
                                mix=0.0)
        node.play(self._alternating())
        rendered = self._words(node, 6)
        self.assertEqual(rendered, self._alternating_words(len(rendered)))

    def test_the_identity_trait_discriminates(self):
        """Its control: the same node wet must not satisfy it."""
        node = audioverb.Tank(sample_rate=self.RATE, channel_count=self.CHANNELS,
                               mix=1.0)
        node.play(self._alternating())
        rendered = self._words(node, 6)
        self.assertNotEqual(rendered, self._alternating_words(len(rendered)))

    def test_silence_in_is_exactly_zero_out(self):
        """V13. Measured exact."""
        node = audioverb.Tank(sample_rate=self.RATE, channel_count=self.CHANNELS,
                               mix=1.0)
        node.play(self._silence())
        for _block in range(6):
            data = bytes(audiocore.get_buffer(node)[1])
            self.assertEqual(data, bytes(len(data)))

    def test_clear_leaves_the_node_as_a_freshly_built_one(self):
        """V14. Not the same claim as "clear() stops it": this is that a
        cleared node and a node that never played render the same bytes."""
        used = audioverb.Tank(sample_rate=self.RATE, channel_count=self.CHANNELS,
                               mix=1.0)
        used.play(self._alternating())
        for _block in range(4):
            audiocore.get_buffer(used)
        used.clear()
        used.play(self._silence())

        fresh = audioverb.Tank(sample_rate=self.RATE, channel_count=self.CHANNELS,
                               mix=1.0)
        fresh.play(self._silence())
        for _block in range(6):
            self.assertEqual(bytes(audiocore.get_buffer(used)[1]),
                             bytes(audiocore.get_buffer(fresh)[1]))

    def test_the_clear_trait_discriminates(self):
        """Its control: without the clear the two must differ."""
        used = audioverb.Tank(sample_rate=self.RATE, channel_count=self.CHANNELS,
                               mix=1.0)
        used.play(self._alternating())
        for _block in range(4):
            audiocore.get_buffer(used)
        used.play(self._silence())               # deliberately not cleared

        fresh = audioverb.Tank(sample_rate=self.RATE, channel_count=self.CHANNELS,
                               mix=1.0)
        fresh.play(self._silence())
        differed = any(bytes(audiocore.get_buffer(used)[1])
                       != bytes(audiocore.get_buffer(fresh)[1])
                       for _block in range(6))
        self.assertTrue(differed, "an uncleared node already matches a fresh "
                        "one, so the clear trait cannot fail")
