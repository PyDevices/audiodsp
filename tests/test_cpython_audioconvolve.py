"""audioconvolve: the traits and the surface the renders cannot check.

`tests/parity/convolve_probe.py` pins what this module renders across the three
targets. What is left for here is what a render cannot say, and the numeric
traits with their bars.

`docs/correctness-standard.md` is what this file implements for
`audioconvolve`: the module is ours - upstream CircuitPython has no counterpart
- so it is held to three-target agreement plus the traits below, never to a
previous version of its own output.

## The traits, with their bars

| ID | Trait | Bar |
|---|---|---|
| C1 | Unloaded, it is a bit-exact passthrough, at the rails included | exact, 0 LSB |
| C2 | Loaded at `mix=0` it is an exact wire, latency-compensated | exact, 0 LSB |
| C3 | A unit-impulse IR reproduces the input, delayed by `latency` | 1 LSB |
| C4 | Nothing but silence comes out before `latency` frames have passed | exact |
| C5 | Silence in is exactly zero out | exact |
| C6 | `clear()` leaves the node as a freshly built one | exact |
| C7 | `latency` reports the loaded state, not a constant | exact |
| C8 | A starved node yields a full block of silence, not a short block | exact |

**C1 and C2 are this module's form of the identity trait** that
`docs/correctness-standard.md` asks of every own node: an exact answer *through*
the DSP rather than around it, evaluated at full scale, because that is where an
arithmetic overflow shows and a range check does not.

C2 is worth reading twice, because the obvious expectation is wrong: `mix=0` on
a loaded convolver is **not** an instant wire. The dry path is delayed by the
same `latency` as the wet one, so a mix does not smear the two against each
other. Measured, it is exact once the latency has passed - 0 LSB over 4608
samples - and silence before it.

C3's bar is 1 LSB rather than exact for an arithmetic reason, not a sloppy one:
the tallest impulse int16 can hold is 32767, so a "unit" impulse has gain
32767/32768 and the reproduction is short by that much.

C7 is audiodsp#44's class-side clause. It returned `AUDIODSP_CONVOLVE_FRAMES`
unconditionally until 2026-09-09, so an unloaded convolver - which is a
passthrough and adds no latency at all - reported a whole partition of it.
"""

from array import array
import unittest

import audioconvolve
import audiocore

SAMPLE_RATE = 8000
CHANNELS = 2

#: One partition. `latency` is this when an impulse is loaded and 0 when not.
PARTITION_FRAMES = 256

#: The tallest impulse int16 holds. Its gain is 32767/32768, which is why C3
#: has a 1 LSB bar.
UNIT_IMPULSE = array("h", [32767] + [0] * 255)


def alternating(frames=6144, level=20000, channels=CHANNELS):
    """Full-scale-ish alternating: adjacent frames at opposite rails, which is
    where an arithmetic width error shows."""
    values = array("h")
    for frame in range(frames):
        for _channel in range(channels):
            values.append(level if frame % 2 else -level)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def alternating_words(count, level=20000, channels=CHANNELS):
    """What `alternating` renders, as signed words, so a comparison needs no
    second node and cannot inherit a node's own bug."""
    return [level if (index // channels) % 2 else -level
            for index in range(count)]


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


def convolver(**extra):
    return audioconvolve.Convolver(max_taps=256, sample_rate=SAMPLE_RATE,
                                   channel_count=CHANNELS, **extra)


class IdentityTest(unittest.TestCase):
    """C1, C2, C3 - the exact answers, through the DSP, at the rails."""

    def test_unloaded_is_a_bit_exact_passthrough(self):
        """C1. The header says an unloaded convolver passes its input through;
        this is that claim as a number, at ±32767 where a width error shows."""
        for level in (20000, 32767):
            with self.subTest(level=level):
                node = convolver(mix=1.0)
                node.play(alternating(level=level))
                rendered = words(node, 8)
                self.assertEqual(
                    rendered, alternating_words(len(rendered), level=level))

    def test_loaded_at_mix_zero_is_an_exact_wire_after_the_latency(self):
        """C2. Not an instant wire: the dry path carries the same latency as
        the wet one, so a mix does not smear them against each other."""
        node = convolver(impulse=UNIT_IMPULSE, mix=0.0)
        self.assertEqual(node.latency, PARTITION_FRAMES)
        node.play(alternating())
        rendered = words(node, 10)
        offset = PARTITION_FRAMES * CHANNELS
        wanted = alternating_words(len(rendered))
        self.assertEqual(rendered[offset:], wanted[:len(rendered) - offset])

    def test_a_unit_impulse_reproduces_the_input(self):
        """C3, with its 1 LSB bar earned by 32767/32768 rather than assumed."""
        node = convolver(impulse=UNIT_IMPULSE, mix=1.0)
        node.play(alternating())
        rendered = words(node, 10)
        offset = node.latency * CHANNELS
        wanted = alternating_words(len(rendered))
        worst = max(abs(rendered[index] - wanted[index - offset])
                    for index in range(offset, len(rendered)))
        self.assertLessEqual(worst, 1)

    def test_the_latency_period_is_exactly_silence(self):
        """C4."""
        node = convolver(impulse=UNIT_IMPULSE, mix=1.0)
        node.play(alternating())
        rendered = words(node, 4)
        offset = node.latency * CHANNELS
        self.assertEqual(set(rendered[:offset]), {0})

    def test_C1_discriminates(self):
        """C1's control. A comparison against a generated reference cannot
        pass by accident the way one against another node can - but it can
        pass on an empty read, so the reference must be non-trivial and the
        render must be as long as it claims."""
        node = convolver(mix=1.0)
        node.play(alternating())
        rendered = words(node, 8)
        self.assertGreater(len(rendered), 4000)
        self.assertEqual(len(set(rendered)), 2)      # two rails, nothing else


class StateTest(unittest.TestCase):
    """C5, C6, C7, C8."""

    def test_silence_in_is_exactly_zero_out(self):
        """C5."""
        for extra in ({}, {"impulse": UNIT_IMPULSE}):
            with self.subTest(loaded=bool(extra)):
                node = convolver(**extra)
                node.play(silence())
                for _block in range(6):
                    data = bytes(audiocore.get_buffer(node)[1])
                    self.assertEqual(data, bytes(len(data)))

    def test_clear_leaves_the_node_as_a_freshly_built_one(self):
        """C6."""
        used = convolver(impulse=UNIT_IMPULSE, mix=1.0)
        used.play(alternating())
        for _block in range(4):
            audiocore.get_buffer(used)
        used.clear()
        used.play(silence())

        fresh = convolver(impulse=UNIT_IMPULSE, mix=1.0)
        fresh.play(silence())
        for _block in range(6):
            self.assertEqual(bytes(audiocore.get_buffer(used)[1]),
                             bytes(audiocore.get_buffer(fresh)[1]))

    def test_C6_discriminates_a_node_that_was_not_cleared(self):
        """C6's control: without the clear the two must differ."""
        used = convolver(impulse=UNIT_IMPULSE, mix=1.0)
        used.play(alternating())
        for _block in range(4):
            audiocore.get_buffer(used)
        used.play(silence())                     # deliberately not cleared

        fresh = convolver(impulse=UNIT_IMPULSE, mix=1.0)
        fresh.play(silence())
        differed = any(bytes(audiocore.get_buffer(used)[1])
                       != bytes(audiocore.get_buffer(fresh)[1])
                       for _block in range(6))
        self.assertTrue(differed, "an uncleared convolver already matches a "
                        "fresh one, so C6 cannot fail")

    def test_latency_reports_the_loaded_state(self):
        """C7, audiodsp#44."""
        self.assertEqual(convolver().latency, 0)
        self.assertEqual(convolver(impulse=UNIT_IMPULSE).latency,
                         PARTITION_FRAMES)
        self.assertEqual(convolver().taps, 0)
        self.assertEqual(convolver(impulse=UNIT_IMPULSE).taps,
                         PARTITION_FRAMES)

    def test_a_starved_node_yields_silence_not_a_short_block(self):
        """C8. This node sits mid-graph and never reports itself finished."""
        node = convolver(impulse=UNIT_IMPULSE)
        result, data = audiocore.get_buffer(node)
        self.assertEqual(result, audiocore.GET_BUFFER_MORE_DATA)
        self.assertEqual(len(data), PARTITION_FRAMES * CHANNELS * 2)
        self.assertEqual(bytes(data), bytes(len(data)))


if __name__ == "__main__":
    unittest.main()
