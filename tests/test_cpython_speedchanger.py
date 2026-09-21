"""`audiospeed.SpeedChanger`: the Q16 rate, and the phase across a boundary.

Two deliberate departures from CircuitPython 10.3.0, both of them upstream
bugs this port declines to reproduce. `docs/upstream-diff.md` carries the
measurements and `docs/upstream-reports/` the reports.

The parity gate cannot see either one. `resampler_probe.py` only ever asks
for 2.0, 1.0 and 0.5, which are exact in Q16 and land on a buffer boundary
with nothing left over, so every interpreter agrees on it whether the
arithmetic is right or not. What these tests measure is the arithmetic.

No numpy: this runs wherever the wheel is installed, and the wheel's numpy is
an extra.

The material is a **sample-and-hold built out of two SpeedChangers** —
decimate at N, restore at 1/N — because that is how the palette builds a
lo-fi rate reducer and it is where both defects show. The source hands out
256-frame buffers, which is the boundary the phase is thrown away at.
"""

import math
import unittest
from array import array

import audiocore
import audiofilters
import audiospeed

RATE = 48000
BLOCK = 256


def blocks(values, rate=RATE, channels=2, block=BLOCK):
    """A source that hands out `block` frames at a time, the way a real graph
    does. A `Filter` with nothing set is a wire; its `buffer_size` is bytes."""
    node = audiofilters.Filter(sample_rate=rate, channel_count=channels,
                               buffer_size=block * channels * 2)
    node.play(audiocore.RawSample(values, sample_rate=rate,
                                  channel_count=channels))
    return node


def hold(values, down, rate=RATE):
    return audiospeed.SpeedChanger(
        audiospeed.SpeedChanger(blocks(values, rate), down), 1.0 / down)


def render(node, frames, channels=2):
    """The first channel of `frames` frames, as plain ints."""
    out = array("h")
    while len(out) < frames * channels:
        result, data = audiocore.get_buffer(node)
        chunk = bytes(data)
        if not chunk:
            break
        out.frombytes(chunk)
        if result == audiocore.GET_BUFFER_DONE:
            break
    return list(out[0:frames * channels:channels])


def tone(frames, hz, level=16384, rate=RATE, channels=2):
    values = array("h", bytes(frames * channels * 2))
    for frame in range(frames):
        value = int(round(level * math.sin(2 * math.pi * hz * frame / rate)))
        for channel in range(channels):
            values[frame * channels + channel] = value
    return values


def ramp(frames, channels=2):
    """One code per frame, full scale. A hold passes it unchanged except for
    the step it holds, so anything left over is drift."""
    values = array("h", bytes(frames * channels * 2))
    for frame in range(frames):
        value = max(-32768, min(32767, frame - frames // 2))
        for channel in range(channels):
            values[frame * channels + channel] = value
    return values


def line_db(signal, hz, rate=RATE):
    """One Hann-windowed DFT bin, in dB. A line that has moved off its bin
    reads *low*, which is the point: a smeared image is a quiet one."""
    real = imaginary = 0.0
    count = len(signal)
    for index, value in enumerate(signal):
        window = 0.5 - 0.5 * math.cos(2 * math.pi * index / count)
        angle = 2 * math.pi * hz * index / rate
        real += value * window * math.cos(angle)
        imaginary -= value * window * math.sin(angle)
    return 20 * math.log10(max(math.hypot(real, imaginary) / count, 1e-12))


def sinc_db(hz, hold_hz):
    """What a zero-order hold does to a line at `hz`, in dB."""
    x = math.pi * hz / hold_hz
    return 20 * math.log10(abs(math.sin(x) / x)) if x else 0.0


class TheRateRoundsToTheNearestStep(unittest.TestCase):
    """audiodsp#92. Upstream's `rate_to_fp` is `(uint32_t)(rate * (1 << 16))`,
    a truncation, so a float landing a hair under its Q16 neighbour loses a
    whole LSB rather than arriving at it. Every number below was measured on
    this twin before the fix and is in `docs/upstream-diff.md`."""

    def test_a_rate_a_hair_under_unity_arrives_at_unity(self):
        """Truncating gave 65535/65536 = 0.9999847412109375."""
        node = audiospeed.SpeedChanger(blocks(tone(512, 1000.0)),
                                       1.0 / 1.0000000000000004)
        self.assertEqual(node.rate, 1.0)

    def test_a_rate_a_hair_under_a_quarter_arrives_at_a_quarter(self):
        """Truncating gave 16383/65536 = 0.2499847412109375."""
        node = audiospeed.SpeedChanger(blocks(tone(512, 1000.0)),
                                       1.0 / 4.0000000000000036)
        self.assertEqual(node.rate, 0.25)

    def test_a_rate_already_on_a_step_is_not_moved(self):
        """The fault the rounding could plant: nudging what was already
        right. Every one of these is exact in Q16, so both arithmetics must
        agree on them."""
        for asked in (0.0, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 1000.0):
            with self.subTest(rate=asked):
                node = audiospeed.SpeedChanger(blocks(tone(64, 1000.0)),
                                               asked)
                self.assertEqual(node.rate, asked)

    def test_the_rate_is_still_refused_outside_its_range(self):
        """Rounding must not widen the range by half an LSB either."""
        source = blocks(tone(64, 1000.0))
        with self.assertRaises(ValueError):
            audiospeed.SpeedChanger(source, 1000.5)
        with self.assertRaises(ValueError):
            audiospeed.SpeedChanger(source, -0.5)

    def test_a_pair_asked_for_the_running_rate_is_a_wire(self):
        """The one that bites. A class computing `fs / rate_hz` in float gets
        1.0000000000000004 off a log-mapped knob, and the pair built from it
        was not an identity at 44.1 kHz: max |wet - dry| read 27666 codes."""
        frames, rate = 4096, 44100
        down = 1.0000000000000004
        values = tone(frames, 441.0, rate=rate)
        wet = render(hold(values, down, rate), frames)
        dry = render(blocks(values, rate), frames)
        self.assertEqual(max(abs(a - b) for a, b in zip(wet, dry)), 0)

    def test_a_resamplers_bound_ratio_rounds_too(self):
        """`Resampler` reaches the same arithmetic by another road: upstream's
        `calculate_rate` truncates 48000/44100 from 71331.9 to 71331."""
        source = audiocore.RawSample(tone(512, 1000.0, rate=48000),
                                     sample_rate=48000, channel_count=2)
        node = audiospeed.Resampler(source)
        audiofilters.Filter(sample_rate=44100, channel_count=2,
                            buffer_size=512).play(node)
        self.assertEqual(round(node.rate * 65536), 71332)


class ThePhaseCrossesASourceBuffer(unittest.TestCase):
    """audiodsp#91. Upstream zeroes the accumulator every time it takes a new
    buffer from the source, so a hold restarts its staircase 187 times a
    second at 48 kHz and drifts. Every "before" number here was measured on
    this twin and is in `docs/upstream-diff.md`."""

    def rendered_hold(self, values, down, frames, block=BLOCK):
        node = audiospeed.SpeedChanger(
            audiospeed.SpeedChanger(blocks(values, block=block), down),
            1.0 / down)
        return render(node, frames)

    def assertSameFrames(self, got, wanted):
        """Counted, not diffed: unittest's list diff over four thousand
        mostly-different frames takes longer than the render does."""
        wrong = sum(1 for a, b in zip(got, wanted) if a != b)
        self.assertEqual(
            (wrong, len(got)), (0, len(wanted)),
            "%d of %d frames differ" % (wrong, min(len(got), len(wanted))))

    def test_the_render_does_not_depend_on_the_sources_block_size(self):
        """The property, stated plainly: the accumulator counts source frames,
        and a buffer boundary is not one. Upstream's render differed in about
        8000 of every 8192 frames between these four."""
        frames = 4096
        values = tone(frames * 2, 100.0)
        reference = self.rendered_hold(values, 1.8433, frames, block=64)
        for block in (100, 256, 1000):
            with self.subTest(block=block):
                self.assertSameFrames(
                    self.rendered_hold(values, 1.8433, frames, block=block),
                    reference)

    def test_a_hold_is_exactly_what_the_accumulator_says_it_is(self):
        """Output frame `n` of a hold is `source[(((n*up)>>16)*down)>>16]`,
        with no term for the block size. Upstream got 15657 of 16384 frames
        wrong at N = 1.8433 and 16066 at N = 6."""
        frames = 4096
        for down in (1.8433, 2.5, 6.0):
            with self.subTest(rate=down):
                values = tone(frames * 8, 100.0)
                mono = [values[index * 2] for index in range(frames * 8)]
                down_fp = int(down * 65536 + 0.5)
                up_fp = int(65536 / down + 0.5)
                wanted = [mono[(((n * up_fp) >> 16) * down_fp) >> 16]
                          for n in range(frames)]
                self.assertSameFrames(
                    self.rendered_hold(values, down, frames), wanted)

    def test_the_run_lengths_stay_in_the_holds_alphabet(self):
        """A hold at N = 2.5 repeats every sample either two or three times.
        Upstream produced twelve runs of five as well, which is the staircase
        restarting mid-run."""
        frames = 8192
        held = self.rendered_hold(tone(frames * 4, 100.0), 2.5, frames)
        lengths = set()
        length = 1
        for index in range(1, len(held)):
            if held[index] == held[index - 1]:
                length += 1
            else:
                lengths.add(length)
                length = 1
        self.assertEqual(lengths, {2, 3})

    def test_a_held_ramp_does_not_drift(self):
        """A zero-order hold lags its input by at most one held step and never
        accumulates. Upstream's lag reached 73 codes over 65536 frames at
        48 kHz, and 430 at 44.1 kHz, on a ramp of one code per frame."""
        frames = 16384
        values = ramp(frames)
        held = self.rendered_hold(values, 1.8433, frames)
        dry = render(blocks(values), frames)
        self.assertLessEqual(abs(dry[-1] - held[-1]), 2)

    def test_the_image_of_a_held_tone_sits_where_a_hold_puts_it(self):
        """The trait a rate reducer exists for. A 7 kHz tone held at 8 kHz
        puts an image at 1 kHz, and a zero-order hold puts it within 0.22 dB
        of the input. Upstream measured -39.9 dB, because the line is spread
        rather than lost."""
        frames = 4096
        values = tone(frames, 7000.0)
        held = self.rendered_hold(values, 6.0, frames)
        dry = render(blocks(values), frames)
        image = line_db(held, 1000.0) - line_db(dry, 7000.0)
        self.assertAlmostEqual(image, sinc_db(1000.0, 8000.0), delta=0.5)

    def test_the_tilt_follows_sinc(self):
        """And the rest of the response with it: upstream missed by 1.41 dB
        at 5 kHz, which is the drift smearing every line, not a hold."""
        frames = 4096
        for hz in (500.0, 1000.0, 2000.0, 5000.0):
            with self.subTest(hz=hz):
                values = tone(frames, hz)
                held = self.rendered_hold(values, 1.8433, frames)
                dry = render(blocks(values), frames)
                measured = line_db(held, hz) - line_db(dry, hz)
                self.assertAlmostEqual(measured, sinc_db(hz, RATE / 1.8433),
                                       delta=0.1)

    def test_a_reset_still_starts_the_stream_over(self):
        """The fault the carry could plant. `reset_buffer` resets the source
        too, so the accumulator genuinely belongs at zero there -- carrying a
        remainder across it would make a replay differ from a first play."""
        values = tone(2048, 100.0)
        node = audiospeed.SpeedChanger(
            audiocore.RawSample(values, sample_rate=RATE, channel_count=2),
            1.8433)
        first = render(node, 512)
        audiocore.reset_buffer(node)
        self.assertSameFrames(render(node, 512), first)

    def test_a_hold_whose_source_runs_out_reports_itself_done(self):
        """The carry can be whole frames at a rate above 1.0, so the index it
        names can land past the buffer that follows. The stream has to end
        there rather than spin looking for a frame nobody will send."""
        values = tone(600, 100.0)
        finite = audiospeed.SpeedChanger(
            audiocore.RawSample(values, sample_rate=RATE, channel_count=2),
            1.0)
        node = audiospeed.SpeedChanger(
            audiospeed.SpeedChanger(finite, 6.0), 1.0 / 6.0)
        for pull in range(64):
            if audiocore.get_buffer(node)[0] == audiocore.GET_BUFFER_DONE:
                return
        self.fail("the hold never reported itself done")

    def test_a_buffer_too_short_to_hold_a_frame_ends_the_stream(self):
        """Upstream tests `len == 0`, which was enough while the accumulator
        was zeroed at every buffer. With the carry it is not: a buffer of no
        whole frames never advances the index, so it has to end the stream
        here rather than be asked for frame 0 of it. Upstream reads two bytes
        of a four-byte frame and keeps going; in C that is a read off the end
        of the source's buffer."""

        class Stub(audiocore._AudioSample):
            sample_rate, channel_count = RATE, 2
            bits_per_sample, samples_signed = 16, True

            def _get_buffer(self, single_channel_output=False,
                            audio_channel=0):
                return audiocore.GET_BUFFER_MORE_DATA, memoryview(b"\x01\x00")

            def _reset_buffer(self, single_channel_output=False,
                              audio_channel=0):
                pass

        node = audiospeed.SpeedChanger(Stub(), 1.8433)
        self.assertEqual(audiocore.get_buffer(node)[0],
                         audiocore.GET_BUFFER_DONE)


if __name__ == "__main__":
    unittest.main()
