"""audioroute: the traits and the surface the renders cannot check.

`tests/parity/route_probe.py`, `route_dry_probe.py` and `midside_probe.py` pin
what this module renders across the three targets. What is left for here is
what a render cannot say: the argument forms, the errors, the source behaviours
only a Python-defined audiosample can produce, and the numeric traits.

`docs/correctness-standard.md` is what this file implements for `audioroute`:
the module is ours - upstream CircuitPython has no counterpart - so it is held
to three-target agreement plus the traits below, never to a previous version of
its own output.

## The traits, with their bars

| ID | Trait | Bar |
|---|---|---|
| R1 | Taps may be named, and the count and indices are bounded | raises |
| R2 | Every tap reads the same stream | exact |
| R3 | A tap keeps its splitter alive | no collection |
| R4 | A dry source yields silence, not an end | exact |
| R5 | A tap can feed another node | renders |
| R6 | MidSide presents itself as a stereo sample | exact |
| R7 | `width=1` is the identity, through the active path | exact, at the rails |
| R8 | `width=0` collapses to mono | exact |
| R9 | The width clamps to its rails | exact |
| R10 | `set()` moves the width mid-stream | exact |
| R11 | A mono source passes through | exact |
| R12 | A block bigger than the ring survives whole | exact, frame for frame |

R7 is this module's form of the identity trait
(`docs/correctness-standard.md`): an exact answer through the DSP rather than a
bypass, which is where an arithmetic overflow shows and a range check does not.

R12 is audioif#87, and it is a trait rather than a render case because no
render would have caught it: the ring is 8192 frames and every probe here feeds
blocks smaller than that. `get_buffer` takes no length, so a source hands back
what it has -- a `RawSample` over a 9600-frame table returns all 9600 -- and
writing the lot lapped every cursor including the one about to read. The first
1408 frames were destroyed before any tap saw them and the stream had a seam at
8192. The material is a per-frame ramp for that reason: a dropped or repeated
frame reads as an index rather than as a click.
"""

import unittest
from array import array

import audiocore
import audiodynamics
import audioroute

SAMPLE_RATE = 48000


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

class SplitterTest(unittest.TestCase):
    def test_taps_may_be_named(self):
        # The original took the count positionally only; a keyword reads far
        # better at the call sites in the effects library.
        splitter = audioroute.Splitter(source(), taps=3)
        self.assertIsNot(splitter.tap(0), splitter.tap(2))
        self.assertIs(splitter.tap(1), splitter.tap(1))

    def test_the_tap_count_is_bounded(self):
        for count in (0, -1, 5):
            with self.assertRaises(ValueError):
                audioroute.Splitter(source(), count)

    def test_tap_indices_are_bounded(self):
        splitter = audioroute.Splitter(source(), 2)
        for index in (-1, 2, 99):
            with self.assertRaises(ValueError):
                splitter.tap(index)

    def test_every_tap_reads_the_same_stream(self):
        splitter = audioroute.Splitter(source(), 4)
        blocks = []
        for index in range(4):
            blocks.append(bytes(audiocore.get_buffer(splitter.tap(index))[1]))
        self.assertEqual(len(set(blocks)), 1)
        self.assertNotEqual(blocks[0], bytes(len(blocks[0])))

    def test_a_tap_keeps_its_splitter_alive(self):
        # Handing a tap to a Mixer and dropping every other name for the
        # Splitter is the ordinary case, not an unusual one: the tap has to
        # own the ring it reads from.
        import gc
        tap = audioroute.Splitter(source(), 2).tap(0)
        gc.collect()
        self.assertNotEqual(bytes(audiocore.get_buffer(tap)[1]), b"")

    def test_a_dry_source_yields_silence_not_an_end(self):
        splitter = audioroute.Splitter(Dry(blocks=1), 2)
        first = splitter.tap(0)
        self.assertNotEqual(bytes(audiocore.get_buffer(first)[1]),
                            bytes(audioroute.CHUNK_FRAMES * 4))
        for _ in range(2):
            result, data = audiocore.get_buffer(first)
            self.assertEqual(result, audiocore.GET_BUFFER_MORE_DATA)
            self.assertEqual(bytes(data), bytes(audioroute.CHUNK_FRAMES * 4))

    def test_a_block_bigger_than_the_ring_survives_whole(self):
        """R12, audioif#87. 9600 frames into an 8192-frame ring.

        Each frame carries its own index, so what this asserts is not "some
        audio came out" but that frame N out is frame N in, for all 9600.
        Before the fix the tap's first frame was source frame 1408.
        """
        frames = 9600
        self.assertGreater(frames, audioroute.RING_FRAMES)
        table = array("h")
        for frame in range(frames):
            value = frame - 15000
            table.extend((value, value))
        splitter = audioroute.Splitter(
            audiocore.RawSample(table, sample_rate=SAMPLE_RATE,
                                channel_count=2), 2)
        tap = splitter.tap(0)
        out = array("h")
        # Bounded by construction. A tap hands back at least one frame per
        # call, so this is many times what is needed, and a regression that
        # stopped producing fails here rather than running for ever.
        for _ in range(frames):
            if len(out) >= frames * 2:
                break
            out.extend(array("h", bytes(audiocore.get_buffer(tap)[1])))
        self.assertGreaterEqual(len(out), frames * 2, "the tap stopped early")
        got = out[:frames * 2:2]
        # Found and reported by hand rather than with assertEqual on two
        # 9600-element lists: unittest formats that failure with difflib, which
        # takes minutes on sequences this long and reads like a hang.
        for index in range(frames):
            if got[index] != index - 15000:
                self.fail(
                    "frame %d is source frame %d, not %d -- %d frames of the "
                    "block were lost (audioif#87)"
                    % (index, got[index] + 15000, index,
                       got[index] + 15000 - index))

    def test_a_block_that_fits_the_ring_is_unchanged(self):
        """The control: the remainder path must not disturb the normal case.

        Anything up to one ring is written in a single pass and leaves no
        remainder, so the source is pulled exactly when it was before.
        """
        splitter = audioroute.Splitter(source(frames=800), 2)
        tap = splitter.tap(0)
        first = bytes(audiocore.get_buffer(tap)[1])
        self.assertEqual(len(first), audioroute.CHUNK_FRAMES * 4)
        self.assertNotEqual(first, bytes(len(first)))

    def test_a_tap_can_feed_a_dynamics(self):
        splitter = audioroute.Splitter(source(), 2)
        node = audiodynamics.Dynamics(audiodynamics.DYN_COMPRESS,
                                      sample_rate=SAMPLE_RATE,
                                      threshold_db=-30.0)
        node.play(splitter.tap(1))
        data = bytes(audiocore.get_buffer(node)[1])
        self.assertEqual(len(data), audiodynamics.FRAMES * 4)
        self.assertNotEqual(data, bytes(len(data)))


class MidSideTest(unittest.TestCase):
    def test_it_presents_itself_as_a_stereo_sample(self):
        node = audioroute.MidSide(sample_rate=SAMPLE_RATE)
        self.assertEqual(node.sample_rate, SAMPLE_RATE)
        self.assertEqual(node.channel_count, 2)
        self.assertEqual(node.bits_per_sample, 16)
        self.assertTrue(node.samples_signed)

    def test_an_unknown_option_is_refused(self):
        node = audioroute.MidSide(source())
        with self.assertRaises(TypeError):
            node.set(depth=0.5)

    def test_a_bad_channel_count_is_refused(self):
        with self.assertRaises(ValueError):
            audioroute.MidSide(channel_count=3)

    def test_width_one_is_the_identity(self):
        values = array("h", (((index * 37) % 65536) - 32768
                             for index in range(1024)))
        node = audioroute.MidSide(
            audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                                channel_count=2), width=1.0)
        data = bytes(audiocore.get_buffer(node)[1])
        self.assertEqual(data, values.tobytes()[:len(data)])

    def test_width_zero_collapses_to_mono(self):
        node = audioroute.MidSide(source(), width=0.0)
        block = array("h")
        block.frombytes(bytes(audiocore.get_buffer(node)[1]))
        for index in range(0, len(block), 2):
            self.assertEqual(block[index], block[index + 1])

    def test_the_width_clamps_to_its_rails(self):
        def render(width):
            node = audioroute.MidSide(source(), width=width)
            return bytes(audiocore.get_buffer(node)[1])
        self.assertEqual(render(-4.0), render(0.0))
        self.assertEqual(render(9.0), render(2.0))

    def test_set_moves_the_width_mid_stream(self):
        node = audioroute.MidSide(source(), width=1.0)
        audiocore.get_buffer(node)
        node.set(width=0.0)
        block = array("h")
        block.frombytes(bytes(audiocore.get_buffer(node)[1]))
        for index in range(0, len(block), 2):
            self.assertEqual(block[index], block[index + 1])

    def test_a_mono_source_passes_through(self):
        values = array("h", (((index * 53) % 2001) - 1000
                             for index in range(600)))
        node = audioroute.MidSide(
            audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                                channel_count=1), width=2.0, channel_count=1)
        data = bytes(audiocore.get_buffer(node)[1])
        self.assertEqual(data, values.tobytes()[:len(data)])

    def test_a_dry_source_yields_silence_not_an_end(self):
        node = audioroute.MidSide(Dry(blocks=1), width=1.5)
        self.assertNotEqual(bytes(audiocore.get_buffer(node)[1]), b"")
        for _ in range(2):
            result, data = audiocore.get_buffer(node)
            self.assertEqual(result, audiocore.GET_BUFFER_MORE_DATA)
            self.assertEqual(bytes(data),
                             bytes(audioroute.MIDSIDE_FRAMES * 4))

    def test_a_tap_can_feed_a_midside(self):
        splitter = audioroute.Splitter(source(), 2)
        node = audioroute.MidSide(width=0.0)
        node.play(splitter.tap(1))
        data = bytes(audiocore.get_buffer(node)[1])
        self.assertEqual(len(data), audioroute.MIDSIDE_FRAMES * 4)
        self.assertNotEqual(data, bytes(len(data)))

    def test_stop_and_play_are_symmetric(self):
        node = audioroute.MidSide(source(), width=0.5)
        self.assertTrue(node.playing)
        node.stop()
        self.assertFalse(node.playing)
        self.assertEqual(bytes(audiocore.get_buffer(node)[1]),
                         bytes(audioroute.MIDSIDE_FRAMES * 4))
        node.play(source())
        self.assertTrue(node.playing)
        self.assertNotEqual(bytes(audiocore.get_buffer(node)[1]),
                            bytes(audioroute.MIDSIDE_FRAMES * 4))


if __name__ == "__main__":
    unittest.main()
