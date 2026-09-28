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
| C9 | A re-synthesis on a playing node drops and repeats nothing: at `mix=0` the render is the uninterrupted one; the block in flight fades from the old room to the new over its unplayed frames at the mix it was computed at; after it the node renders what a node built with the new room renders; the fade does not click | exact; the fade 1 LSB of the line; no step past 1.5 x the rooms' own |

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

C9 is audiodsp#163. `synthesize()` used to end in a reset, so a room moved
while playing dropped the 256 frames in flight, dry and wet, at every mix,
and the tail stopped dead. The frequency-delay line holds only input, so a
node that keeps it and swaps the impulse is, from the next block on, exactly
a node that always had the new room, which is what the second half of C9
checks. The fade is the part that has a choice in it: a hard swap at the
block edge also drops nothing, but on a dark room a move that decorrelates
the two (Room, Predelay) steps the wet up to about three times the largest
step either room makes on its own, and that is a click.

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


def noise_source(frames, channels, level=8000, seed=12345, rate=SAMPLE_RATE,
                 stop=None):
    """Uniform white noise, an LCG so the draw is the same everywhere; zero
    from frame `stop` on."""
    values = array("h", bytes(2 * frames * channels))
    state = seed
    span = 2 * level + 1
    end = frames if stop is None else stop
    for index in range(end * channels):
        state = (state * 1103515245 + 12345) & 0x7fffffff
        values[index] = ((state >> 8) % span) - level
    return values


class Once(audiocore._AudioSample):
    """A source that hands its frames over once and then has none, so a
    pull can stop part way through the node's block."""

    def __init__(self, values, channels):
        self.sample_rate = SAMPLE_RATE
        self.channel_count = channels
        self.bits_per_sample = 16
        self._data = bytes(values)

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        data, self._data = self._data, b""
        if not data:
            return audiocore.GET_BUFFER_ERROR, b""
        return audiocore.GET_BUFFER_MORE_DATA, data


class Resynthesis:
    """One stream through a synthesized room, with calls between pulls."""

    def __init__(self, room, rate=SAMPLE_RATE, channels=CHANNELS, mix=1.0,
                 taps=1024):
        self.rate = rate
        self.channels = channels
        self.node = audioconvolve.Convolver(
            max_taps=taps, ir_channels=channels, sample_rate=rate,
            channel_count=channels, mix=mix)
        self.node.synthesize(**room)

    def play(self, values):
        self.node.play(audiocore.RawSample(values, sample_rate=self.rate,
                                           channel_count=self.channels))

    def pull(self, blocks, calls=None):
        """`blocks` pulls; `calls` maps a pull index to what runs before it."""
        out = []
        for block in range(blocks):
            if calls and block in calls:
                calls[block](self.node)
            data = bytes(audiocore.get_buffer(self.node)[1])
            for position in range(0, len(data), 2):
                word = data[position] | (data[position + 1] << 8)
                out.append(word - 65536 if word >= 32768 else word)
        return out


ROOM = {"decay": 0.1, "damping_hz": 2000.0, "predelay_ms": 0.0,
        "diffusion_ms": 10.0, "seed": 1}

#: One argument moved, every argument `synthesize` takes, and a call that
#: moves nothing.
MOVES = (
    ("same", {}),
    ("decay", {"decay": 0.06}),
    ("damping", {"damping_hz": 500.0}),
    ("predelay", {"predelay_ms": 15.0}),
    ("diffusion", {"diffusion_ms": 0.0}),
    ("seed", {"seed": 36}),
)

AT = 6          # the pull the call comes before
BLOCKS = 12


def moved(change, room=ROOM):
    new = dict(room)
    new.update(change)
    return new


def render(room, values, mix, channels=CHANNELS, calls=None, rate=SAMPLE_RATE,
           taps=1024):
    stream = Resynthesis(room, rate=rate, channels=channels, mix=mix,
                         taps=taps)
    stream.play(values)
    return stream.pull(BLOCKS, calls)


class ResynthesisTest(unittest.TestCase):
    """C9, audiodsp#163."""

    def test_at_mix_zero_a_resynthesis_is_the_uninterrupted_wire(self):
        """No frame dropped or repeated: at `mix=0` the output is the source
        one partition late whatever the room does, so a render with a call
        must be the render without one, byte for byte."""
        for channels in (2, 1):
            values = noise_source(BLOCKS * PARTITION_FRAMES, channels)
            want = render(ROOM, values, 0.0, channels)
            for tag, change in MOVES:
                with self.subTest(channels=channels, move=tag):
                    new = moved(change)
                    got = render(ROOM, values, 0.0, channels, {
                        AT: lambda node, new=new: node.synthesize(**new)})
                    self.assertEqual(got, want)

    def test_a_resynthesis_mid_block_drops_nothing(self):
        """The same with the call landing part way through a block, where
        the input gathered so far and the unplayed rest of the block in
        flight both have to survive it."""
        for channels in (2, 1):
            values = noise_source(BLOCKS * PARTITION_FRAMES, channels)
            head = 1000                    # not a whole number of blocks
            first = values[:head * channels]
            rest = values[head * channels:]
            for tag, change in MOVES[1:]:
                with self.subTest(channels=channels, move=tag):
                    outs = []
                    for call in (False, True):
                        stream = Resynthesis(ROOM, channels=channels, mix=0.0)
                        stream.node.play(Once(first, channels))
                        out = stream.pull(4)          # 3 whole + 232 frames
                        if call:
                            stream.node.synthesize(**moved(change))
                        stream.play(rest)
                        out += stream.pull(8)
                        outs.append(out)
                    self.assertEqual(outs[1], outs[0])
                    self.assertEqual(len(outs[1]),
                                     (3 * 256 + 232 + 8 * 256) * channels)
                    # And the wet: from the block after the one in flight,
                    # the node built with the new room, fed the same way.
                    wet = []
                    for room in (moved(change), None):
                        stream = Resynthesis(room or ROOM, channels=channels)
                        stream.node.play(Once(first, channels))
                        out = stream.pull(4)
                        if room is None:
                            stream.node.synthesize(**moved(change))
                        stream.play(rest)
                        wet.append(out + stream.pull(8))
                    after = (4 * 256) * channels
                    self.assertEqual(wet[1][after:], wet[0][after:])

    def test_after_the_block_in_flight_it_is_the_new_room(self):
        """Before the call the node is the old room, and from the block
        after the one in flight it is exactly a node built with the new
        room: the frequency-delay line is input only, so keeping it is
        right, and nothing of the old room lingers."""
        for mix in (0.3, 1.0):
            for channels in (2, 1):
                values = noise_source(BLOCKS * PARTITION_FRAMES, channels)
                old = render(ROOM, values, mix, channels)
                for tag, change in MOVES[1:]:
                    with self.subTest(mix=mix, channels=channels, move=tag):
                        new = moved(change)
                        fresh = render(new, values, mix, channels)
                        got = render(ROOM, values, mix, channels, {
                            AT: lambda node, new=new: node.synthesize(**new)})
                        start = AT * PARTITION_FRAMES * channels
                        after = (AT + 1) * PARTITION_FRAMES * channels
                        self.assertEqual(got[:start], old[:start])
                        self.assertEqual(got[after:], fresh[after:])
                        self.assertNotEqual(old[after:], fresh[after:])

    def test_the_block_in_flight_fades_from_the_old_room_to_the_new(self):
        """A straight line over the block's 256 frames, landing on the new
        room: frame k is old + (k + 1) / 256 of the way to new. The bar is
        1 LSB, the two roundings between a line through rounded samples and
        a rounded line."""
        for mix in (0.3, 1.0):
            for channels in (2, 1):
                values = noise_source(BLOCKS * PARTITION_FRAMES, channels)
                old = render(ROOM, values, mix, channels)
                for tag, change in MOVES[1:]:
                    with self.subTest(mix=mix, channels=channels, move=tag):
                        new = moved(change)
                        fresh = render(new, values, mix, channels)
                        got = render(ROOM, values, mix, channels, {
                            AT: lambda node, new=new: node.synthesize(**new)})
                        worst = 0
                        moved_frames = 0
                        for k in range(PARTITION_FRAMES):
                            for c in range(channels):
                                i = (AT * PARTITION_FRAMES + k) * channels + c
                                line = old[i] + (k + 1) / 256.0 * (
                                    fresh[i] - old[i])
                                worst = max(worst, abs(got[i] - line))
                                moved_frames += got[i] != old[i]
                        self.assertLessEqual(worst, 1.0)
                        self.assertGreater(moved_frames, 0)

    def test_the_block_in_flight_keeps_the_mix_it_was_computed_at(self):
        """A Mix move acts on the input after it, one partition later
        (the block in flight was already mixed). A room move straight after
        it must not bring it forward: at `mix=0` moved to 1.0 and then a new
        room, the block in flight is still the dry alone."""
        for channels in (2, 1):
            values = noise_source(BLOCKS * PARTITION_FRAMES, channels)
            dry = render(ROOM, values, 0.0, channels)
            new = moved({"seed": 36})

            def call(node, new=new):
                node.set(mix=1.0)
                node.synthesize(**new)

            got = render(ROOM, values, 0.0, channels, {AT: call})
            after = (AT + 1) * PARTITION_FRAMES * channels
            with self.subTest(channels=channels):
                self.assertEqual(got[:after], dry[:after])
                self.assertNotEqual(got[after:], dry[after:])

    def test_the_fade_does_not_click(self):
        """The wet alone at 48 kHz through a dark room (500 Hz), white noise
        playing through the change, Room and Predelay moved: the largest
        step over the block in flight and 16 frames either side stays
        within 1.5 x the largest step either room makes on its own there.
        A hard swap at the block edge steps up to about 3 x on these cells
        (convolve_fix_click.py in the workspace's effects probes)."""
        rate = 48000
        taps = 3840
        dark = {"decay": 0.08, "damping_hz": 500.0, "predelay_ms": 0.0,
                "diffusion_ms": 10.0}
        worst_ratio = 0.0
        for channels in (2, 1):
            values = noise_source(BLOCKS * PARTITION_FRAMES, channels,
                                  rate=rate)
            for seed in range(1, 9):
                room = dict(dark, seed=seed)
                for change in ({"seed": seed + 35}, {"predelay_ms": 15.0}):
                    new = moved(change, room)
                    a = render(room, values, 1.0, channels, rate=rate,
                               taps=taps)
                    b = render(new, values, 1.0, channels, rate=rate,
                               taps=taps)
                    got = render(room, values, 1.0, channels, {
                        AT: lambda node, new=new: node.synthesize(**new)},
                        rate=rate, taps=taps)
                    lo = AT * PARTITION_FRAMES - 16
                    hi = (AT + 1) * PARTITION_FRAMES + 16

                    def step(v):
                        return max(abs(v[i] - v[i - channels])
                                   for i in range(lo * channels,
                                                  hi * channels))

                    own = max(step(a), step(b))
                    with self.subTest(channels=channels, seed=seed,
                                      change=change):
                        self.assertLessEqual(step(got), 1.5 * own)
                    worst_ratio = max(worst_ratio, step(got) / own)
        self.assertGreater(worst_ratio, 0.5)

    def test_the_first_synthesis_starts_the_latency_from_empty(self):
        """Unchanged by #163: a node with nothing loaded is a bypass with no
        latency, and its first room starts the one-partition latency from
        an empty node, as it always has. A node that played through as a
        bypass and then got its room renders, from then on, what a fresh
        node with that room renders from the same point."""
        for channels in (2, 1):
            values = noise_source(BLOCKS * PARTITION_FRAMES, channels)
            head = 3 * PARTITION_FRAMES * channels
            node = audioconvolve.Convolver(
                max_taps=1024, ir_channels=channels, sample_rate=SAMPLE_RATE,
                channel_count=channels, mix=0.5)
            node.play(audiocore.RawSample(values[:head],
                                          sample_rate=SAMPLE_RATE,
                                          channel_count=channels))
            bypass = words_of(node, 3)
            node.synthesize(**ROOM)
            node.play(audiocore.RawSample(values[head:],
                                          sample_rate=SAMPLE_RATE,
                                          channel_count=channels))
            late = words_of(node, 6)
            fresh = render(ROOM, values[head:], 0.5, channels)[:len(late)]
            with self.subTest(channels=channels):
                self.assertEqual(bypass, list(values[:head]))
                self.assertEqual(late, fresh)
                self.assertEqual(set(late[:PARTITION_FRAMES * channels]),
                                 {0})

    def test_clear_after_a_resynthesis_leaves_a_fresh_node(self):
        """C6 still holds after a mid-stream room move: `clear()` empties
        everything, the block in flight included."""
        values = noise_source(BLOCKS * PARTITION_FRAMES, CHANNELS)
        new = moved({"seed": 36})
        used = Resynthesis(ROOM, mix=0.5)
        used.play(values)
        used.pull(4, {2: lambda node: node.synthesize(**new)})
        used.node.clear()
        used.play(values)
        fresh = Resynthesis(new, mix=0.5)
        fresh.play(values)
        self.assertEqual(used.pull(6), fresh.pull(6))


def words_of(node, blocks):
    return words(node, blocks)


if __name__ == "__main__":
    unittest.main()
