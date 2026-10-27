"""audioconvolve: the traits and the surface the renders cannot check.

`tests/parity/convolve_probe.py` pins what this module renders across the three
targets. What is left for here is what a render cannot say, and the numeric
traits with their bars.

`docs/how-it-works/correctness-standard.md` is what this file implements for
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
| C11 | A `load()` on a playing node drops and repeats nothing, as C9: at `mix=0` the render is the uninterrupted one; the block in flight fades from the old impulse to the new over its unplayed frames; after it the node renders what a node built with the new impulse renders; a first load, and an unload, still start from empty | exact; the fade 1 LSB of the line |
| C12 | `wet_gain` scales the wet path alone: the dry is untouched, the wet is `wet_gain` times the wet at 1.0, 1.0 (the default) renders what a node without it renders, a move acts after the block in flight, as a Mix move does, and one made with a re-synthesis fades with the room, from the old room at the old gain to the new at the new | 1 LSB; exact at the default, before the move and after the fade |
| C13 | `room_peak_gain` is the room's own peak gain on low material: the largest of its step response's magnitude and the steady-state peak of a +-1 square from 40 to 160 Hz in eighth-octave steps, the worse side, read off the room's impulse independently; 0.0 before a synthesis and after a `load()`; `room_peak_gain_for` reads the same before the room is built | 0.5 % of the reading, or 0.005; the read-ahead exact |
| C10 | Each side of a synthesized stereo room is the unit-energy room of its own noise: the left is the mono room of the same seed, the right the mono room of the right side's seed; the two sides balance, together they are the level of a mono room, and they stay two different noises | exact; 0.01 dB; correlation under 0.5 |

**C1 and C2 are this module's form of the identity trait** that
`docs/how-it-works/correctness-standard.md` asks of every own node: an exact answer *through*
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

C11 is C9 for `load()`. The fix for #163 left `load()` ending in the reset
it always had, so a cabinet or a room loaded from a file while playing
dropped the 256 frames in flight and started the new impulse on an empty
history, the tail cut dead: the mechanism was there and only `synthesize()`
used it. Now both take the same path, so a convolver can walk through a set
of recorded impulses while audio plays the way it can already walk through
synthesized rooms. What is kept of the old behaviour, deliberately: a node
with nothing loaded is a bypass with no latency and starts its latency from
empty on its first impulse, and `load(b"")` makes it that bypass again,
since a bypass holds no audio to fade to.

C10 is audiodsp#164. The room used to be scaled by the mean of its two
sides' energies, so the pair was unit energy but one side could come back
5.3 dB louder than the other, and every argument moved which. Each side now
has its own scale. The exact form of the trait follows from how the noise is
seeded: the right side's generator starts at `seed + 0x9e3779b9`, so it is
the mono room of that seed, and the left side is the mono room of `seed`.
What a per-side scale must not do is make the room mono, so C10 also asks
that the two sides stay different noises.

C7 is audiodsp#44's class-side clause. It returned `AUDIODSP_CONVOLVE_FRAMES`
unconditionally until 2026-09-09, so an unloaded convolver - which is a
passthrough and adds no latency at all - reported a whole partition of it.
"""

from array import array
import math
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

    def feed(self, values):
        """`values` straight through the node's kernel, in one call of
        whatever length, without a pull."""
        data = self.node._state.process(bytes(values))
        out = []
        for position in range(0, len(data), 2):
            word = data[position] | (data[position + 1] << 8)
            out.append(word - 65536 if word >= 32768 else word)
        return out

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
        """The same with the call landing part way through the kernel's
        block, where the input gathered so far and the unplayed rest of the
        block in flight both have to survive it. A node hands on whole blocks
        whatever its source does (audiodsp#180), so between two pulls its
        kernel is never part way through one; the kernel is driven directly
        here instead, in the uneven calls it accepts from a binding."""
        for channels in (2, 1):
            values = noise_source(BLOCKS * PARTITION_FRAMES, channels)
            head = 1000                    # not a whole number of blocks
            first = values[:head * channels]
            rest = values[head * channels:(head + 8 * 256) * channels]
            for tag, change in MOVES[1:]:
                with self.subTest(channels=channels, move=tag):
                    outs = []
                    for call in (False, True):
                        stream = Resynthesis(ROOM, channels=channels, mix=0.0)
                        out = stream.feed(first)      # 3 whole + 232 frames
                        if call:
                            stream.node.synthesize(**moved(change))
                        out += stream.feed(rest)
                        outs.append(out)
                    self.assertEqual(outs[1], outs[0])
                    self.assertEqual(len(outs[1]),
                                     (3 * 256 + 232 + 8 * 256) * channels)
                    # And the wet: from the block after the one in flight,
                    # the node built with the new room, fed the same way.
                    wet = []
                    for room in (moved(change), None):
                        stream = Resynthesis(room or ROOM, channels=channels)
                        out = stream.feed(first)
                        if room is None:
                            stream.node.synthesize(**moved(change))
                        wet.append(out + stream.feed(rest))
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


GOLDEN = 0x9e3779b9      # the right side's seed offset in the C


def click_energies(room, rate, taps, ir_channels=2):
    """Each side's energy over the room's own impulse: a 32767 click at
    mix 1.0, the wet alone, read to the end of the impulse."""
    node = audioconvolve.Convolver(max_taps=taps, ir_channels=ir_channels,
                                   sample_rate=rate,
                                   channel_count=ir_channels, mix=1.0)
    node.synthesize(**room)
    frames = taps + 2 * PARTITION_FRAMES
    values = array("h", bytes(2 * frames * ir_channels))
    for c in range(ir_channels):
        values[c] = 32767
    node.play(audiocore.RawSample(values, sample_rate=rate,
                                  channel_count=ir_channels))
    out = words(node, frames // PARTITION_FRAMES)
    energy = [0.0] * ir_channels
    for index, value in enumerate(out):
        energy[index % ir_channels] += value * value
    return energy, out


def db(ratio):
    return 10.0 * math.log10(ratio)


#: The widest cells the class's walks found on main (L - R -5.296, -5.256,
#: -4.675 and +4.489 dB), as the node's arguments, with the 0.08 s room's
#: taps.
WIDEST = (
    (48000, 3840, {"decay": 0.05, "damping_hz": 500.0, "predelay_ms": 0.0,
                   "diffusion_ms": 12 / 127.0 * 12.5, "seed": 36}),
    (44100, 3584, {"decay": 0.05, "damping_hz": 500.0, "predelay_ms": 0.0,
                   "diffusion_ms": 13 / 127.0 * 12.5, "seed": 36}),
    (22050, 1792, {"decay": 0.05, "damping_hz": 500.0, "predelay_ms": 0.0,
                   "diffusion_ms": 22 / 127.0 * 12.5, "seed": 36}),
    (48000, 3840, {"decay": 0.05, "damping_hz": 500.0, "predelay_ms": 0.0,
                   "diffusion_ms": 0.0, "seed": 4}),
)

#: Rooms at 8 kHz across every argument, for the exact trait.
ROOMS = (
    ROOM,
    moved({"damping_hz": 500.0}),
    moved({"decay": 0.06, "diffusion_ms": 0.0}),
    moved({"predelay_ms": 15.0, "seed": 36}),
    moved({"damping_hz": 0.0, "diffusion_ms": 30.0, "seed": 4}),
)


class StereoRoomTest(unittest.TestCase):
    """C10, audiodsp#164."""

    def test_each_side_is_the_mono_room_of_its_own_noise(self):
        """Exact. The left of a stereo room renders what a mono room of the
        same arguments renders from the left alone, and the right what a
        mono room seeded where the right side's noise starts renders from
        the right alone: one unit-energy scale per side."""
        values = noise_source(BLOCKS * PARTITION_FRAMES, 2)
        left = array("h", values[0::2])
        right = array("h", values[1::2])
        for room in ROOMS:
            with self.subTest(room=room):
                stereo = render(room, values, 0.5, 2)
                mono_left = render(room, left, 0.5, 1)
                shifted = dict(room, seed=(room["seed"] + GOLDEN) & 0xffffffff)
                mono_right = render(shifted, right, 0.5, 1)
                self.assertEqual(stereo[0::2], mono_left)
                self.assertEqual(stereo[1::2], mono_right)

    def test_the_sides_balance_and_the_pair_keeps_its_level(self):
        """At the widest cells found before the fix: each side's energy over
        the impulse within 0.01 dB of the other, and the two together within
        0.01 dB of twice a mono room's, which is what the pair was before."""
        for rate, taps, room in WIDEST:
            with self.subTest(rate=rate, room=room):
                (left, right), _ = click_energies(room, rate, taps)
                (mono,), _ = click_energies(room, rate, taps, 1)
                self.assertLess(abs(db(left / right)), 0.01)
                self.assertLess(abs(db((left + right) / (2.0 * mono))), 0.01)

    def test_a_stereo_room_stays_stereo(self):
        """What a per-side scale must not cost: the two sides are still two
        different noises, correlated at lag 0 by under 0.5 (the darkest,
        shortest rooms read about 0.25), where a room with the same noise
        on both sides reads 1."""
        for rate, taps, room in WIDEST[:3]:
            with self.subTest(rate=rate):
                _, out = click_energies(room, rate, taps)
                left, right = out[0::2], out[1::2]
                cross = sum(a * b for a, b in zip(left, right))
                norm = (sum(a * a for a in left)
                        * sum(b * b for b in right)) ** 0.5
                self.assertNotEqual(left, right)
                self.assertLess(abs(cross / norm), 0.5)


def impulse_taps(frames, channels, seed=1, level=1500, spike=8000):
    """A deterministic int16 impulse: a leading spike, then decaying
    pseudo-noise, so every tap and every bin carries something. Quiet
    enough that `noise_source` through it stays well inside int16, so a
    comparison here never meets the rails."""
    values = array("h", bytes(2 * frames * channels))
    state = (seed * 2654435761) & 0x7fffffff
    for frame in range(frames):
        envelope = level * (frames - frame) // frames
        for channel in range(channels):
            state = (state * 1103515245 + 12345) & 0x7fffffff
            values[frame * channels + channel] = (
                (state >> 8) % (2 * envelope + 1)) - envelope
    for channel in range(channels):
        values[channel] = spike
    return values


#: Three partitions, so a move can go shorter or longer.
IMPULSE = {"frames": 768, "seed": 1, "gain": 1.0}

#: A reload that changes nothing, a different response of the same length,
#: a shorter one, a longer one, and the same taps at another gain.
RELOADS = (
    ("same", {}),
    ("taps", {"seed": 2}),
    ("shorter", {"frames": 512}),
    ("longer", {"frames": 1024}),
    ("gain", {"gain": 0.5}),
)


def reloaded(change, spec=IMPULSE):
    new = dict(spec)
    new.update(change)
    return new


class Reload(Resynthesis):
    """One stream through a loaded impulse, with calls between pulls."""

    def __init__(self, spec, rate=SAMPLE_RATE, channels=CHANNELS, mix=1.0,
                 taps=1024):
        self.rate = rate
        self.channels = channels
        self.node = audioconvolve.Convolver(
            max_taps=taps, ir_channels=channels, sample_rate=rate,
            channel_count=channels, mix=mix)
        if spec is not None:
            load(self.node, spec, channels)


def load(node, spec, channels):
    node.load(impulse_taps(spec["frames"], channels, spec["seed"]),
              channels=channels, gain=spec["gain"])


def render_loaded(spec, values, mix, channels=CHANNELS, calls=None):
    stream = Reload(spec, channels=channels, mix=mix)
    stream.play(values)
    return stream.pull(BLOCKS, calls)


class ReloadTest(unittest.TestCase):
    """C11: `load()` on a playing node, held to what C9 holds `synthesize()`
    to. The same frames, the same pull the call comes before, so a cell
    that fails here and passes there is the one path differing."""

    def test_at_mix_zero_a_load_is_the_uninterrupted_wire(self):
        """No frame dropped or repeated: at `mix=0` the output is the source
        one partition late whatever the impulse does, so a render with a
        load must be the render without one, byte for byte."""
        for channels in (2, 1):
            values = noise_source(BLOCKS * PARTITION_FRAMES, channels)
            want = render_loaded(IMPULSE, values, 0.0, channels)
            for tag, change in RELOADS:
                with self.subTest(channels=channels, reload=tag):
                    new = reloaded(change)
                    got = render_loaded(IMPULSE, values, 0.0, channels, {
                        AT: lambda node, new=new: load(node, new, channels)})
                    self.assertEqual(got, want)

    def test_a_load_mid_block_drops_nothing(self):
        """The kernel driven in uneven calls, so the load lands part way
        through a block: the input gathered so far and the unplayed rest
        of the block in flight both survive it."""
        for channels in (2, 1):
            values = noise_source(BLOCKS * PARTITION_FRAMES, channels)
            head = 1000                    # not a whole number of blocks
            first = values[:head * channels]
            rest = values[head * channels:(head + 8 * 256) * channels]
            for tag, change in RELOADS[1:]:
                with self.subTest(channels=channels, reload=tag):
                    new = reloaded(change)
                    outs = []
                    for call in (False, True):
                        stream = Reload(IMPULSE, channels=channels, mix=0.0)
                        out = stream.feed(first)      # 3 whole + 232 frames
                        if call:
                            load(stream.node, new, channels)
                        out += stream.feed(rest)
                        outs.append(out)
                    self.assertEqual(outs[1], outs[0])
                    self.assertEqual(len(outs[1]),
                                     (3 * 256 + 232 + 8 * 256) * channels)
                    # And the wet: from the block after the one in flight,
                    # the node built with the new impulse, fed the same way.
                    wet = []
                    for spec in (new, None):
                        stream = Reload(spec or IMPULSE, channels=channels)
                        out = stream.feed(first)
                        if spec is None:
                            load(stream.node, new, channels)
                        wet.append(out + stream.feed(rest))
                    after = (4 * 256) * channels
                    self.assertEqual(wet[1][after:], wet[0][after:])

    def test_after_the_block_in_flight_it_is_the_new_impulse(self):
        """Before the call the node is the old impulse, and from the block
        after the one in flight it is exactly a node built with the new
        one: the history is kept, not started empty. A shorter impulse
        reads fewer slots of the same history and a longer one more."""
        for mix in (0.3, 1.0):
            for channels in (2, 1):
                values = noise_source(BLOCKS * PARTITION_FRAMES, channels)
                old = render_loaded(IMPULSE, values, mix, channels)
                for tag, change in RELOADS[1:]:
                    with self.subTest(mix=mix, channels=channels, reload=tag):
                        new = reloaded(change)
                        fresh = render_loaded(new, values, mix, channels)
                        got = render_loaded(IMPULSE, values, mix, channels, {
                            AT: lambda node, new=new: load(node, new, channels)})
                        start = AT * PARTITION_FRAMES * channels
                        after = (AT + 1) * PARTITION_FRAMES * channels
                        self.assertEqual(got[:start], old[:start])
                        self.assertEqual(got[after:], fresh[after:])
                        self.assertNotEqual(old[after:], fresh[after:])

    def test_the_block_in_flight_fades_from_the_old_impulse_to_the_new(self):
        """The same line as C9: frame k of the block in flight is old plus
        (k + 1) / 256 of the way to new, within 1 LSB."""
        for mix in (0.3, 1.0):
            for channels in (2, 1):
                values = noise_source(BLOCKS * PARTITION_FRAMES, channels)
                old = render_loaded(IMPULSE, values, mix, channels)
                for tag, change in RELOADS[1:]:
                    with self.subTest(mix=mix, channels=channels, reload=tag):
                        new = reloaded(change)
                        fresh = render_loaded(new, values, mix, channels)
                        got = render_loaded(IMPULSE, values, mix, channels, {
                            AT: lambda node, new=new: load(node, new, channels)})
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

    def test_the_first_load_starts_the_latency_from_empty(self):
        """Unchanged: a node with nothing loaded is a bypass with no
        latency, and its first impulse starts the one-partition latency
        from an empty node. From then on it renders what a fresh node with
        that impulse renders from the same point."""
        for channels in (2, 1):
            values = noise_source(BLOCKS * PARTITION_FRAMES, channels)
            head = 3 * PARTITION_FRAMES * channels
            stream = Reload(None, channels=channels, mix=0.5)
            stream.play(values[:head])
            bypass = stream.pull(3)
            load(stream.node, IMPULSE, channels)
            stream.play(values[head:])
            late = stream.pull(6)
            fresh = render_loaded(IMPULSE, values[head:], 0.5,
                                  channels)[:len(late)]
            with self.subTest(channels=channels):
                self.assertEqual(bypass, list(values[:head]))
                self.assertEqual(late, fresh)
                self.assertEqual(set(late[:PARTITION_FRAMES * channels]),
                                 {0})

    def test_an_empty_impulse_makes_a_bypass_again(self):
        """Unchanged too: `load(b"")` unloads, and an unloaded node is a
        bypass with no latency, so from the next pull the output is the
        input as it stands. There is no room to fade to, and nothing of the
        old tail is kept."""
        for channels in (2, 1):
            values = noise_source(BLOCKS * PARTITION_FRAMES, channels)
            stream = Reload(IMPULSE, channels=channels, mix=1.0)
            stream.play(values)
            before = stream.pull(AT)
            stream.node.load(b"", channels=channels)
            after = stream.pull(BLOCKS - AT)
            start = AT * PARTITION_FRAMES * channels
            with self.subTest(channels=channels):
                self.assertEqual(stream.node.taps, 0)
                self.assertEqual(stream.node.latency, 0)
                self.assertNotEqual(before, list(values[:start]))
                self.assertEqual(after, list(values[start:]))


def words_of(node, blocks):
    return words(node, blocks)



def gained(values, mix, channels=CHANNELS, calls=None, **extra):
    node = audioconvolve.Convolver(
        max_taps=1024, ir_channels=channels, sample_rate=SAMPLE_RATE,
        channel_count=channels, mix=mix, **extra)
    node.synthesize(**ROOM)
    node.play(audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                                  channel_count=channels))
    out = []
    for block in range(BLOCKS):
        if calls and block in calls:
            calls[block](node)
        out.extend(words(node, 1))
    return out


class WetGainTest(unittest.TestCase):
    """C12: a gain on the wet path alone, for a class that sets a room's
    level without moving its dry or its Mix law."""

    def test_the_wet_alone_is_scaled(self):
        for channels in (2, 1):
            values = noise_source(BLOCKS * PARTITION_FRAMES, channels)
            for mix in (0.5, 0.3, 0.75, 1.0):
                dry = gained(values, mix, channels, wet_gain=0.0)
                full = gained(values, mix, channels)
                for gain in (0.5, 0.25, 0.8):
                    for how in ("init", "set"):
                        if how == "init":
                            got = gained(values, mix, channels,
                                         wet_gain=gain)
                        else:
                            got = gained(values, mix, channels, calls={
                                0: lambda n, g=gain: n.set(wet_gain=g)})
                        worst = max(abs((g - d) - gain * (f - d))
                                    for g, d, f in zip(got, dry, full))
                        with self.subTest(channels=channels, mix=mix,
                                          gain=gain, how=how):
                            self.assertLessEqual(worst, 1)
                            self.assertNotEqual(got, full)

    def test_zero_leaves_the_dry_exactly(self):
        values = noise_source(BLOCKS * PARTITION_FRAMES, CHANNELS)
        self.assertEqual(gained(values, 0.5, wet_gain=0.0),
                         gained(values, 0.0))

    def test_the_default_is_one_and_moves_nothing(self):
        for channels in (2, 1):
            values = noise_source(BLOCKS * PARTITION_FRAMES, channels)
            for mix in (0.0, 0.3, 0.5, 0.8, 1.0):
                base = gained(values, mix, channels)
                with self.subTest(channels=channels, mix=mix):
                    self.assertEqual(gained(values, mix, channels,
                                            wet_gain=1.0), base)
                    self.assertEqual(gained(values, mix, channels, calls={
                        AT: lambda n: n.set(wet_gain=1.0)}), base)

    def test_a_move_acts_after_the_block_in_flight(self):
        values = noise_source(BLOCKS * PARTITION_FRAMES, CHANNELS)
        full = gained(values, 0.5)
        got = gained(values, 0.5, calls={AT: lambda n: n.set(wet_gain=0.5)})
        after = (AT + 1) * PARTITION_FRAMES * CHANNELS
        self.assertEqual(got[:after], full[:after])
        self.assertNotEqual(got[after:], full[after:])

    def test_a_gain_moved_with_the_room_fades_with_it(self):
        # set(wet_gain) and then synthesize() before one pull: the block in
        # flight runs on the line from the old room at the old gain to the
        # new room at the new, and after it the node is one built with both.
        for mix in (0.3, 0.5, 1.0):
            for channels in (2, 1):
                values = noise_source(BLOCKS * PARTITION_FRAMES, channels)
                old = gained(values, mix, channels)
                for tag, change in MOVES[1:]:
                    new = moved(change)

                    def at_new(node, new=new):
                        node.synthesize(**new)
                    fresh = gained(values, mix, channels, wet_gain=0.5,
                                   calls={0: at_new})

                    def call(node, new=new):
                        node.set(wet_gain=0.5)
                        node.synthesize(**new)
                    got = gained(values, mix, channels, calls={AT: call})
                    worst = 0
                    for k in range(PARTITION_FRAMES):
                        for c in range(channels):
                            i = (AT * PARTITION_FRAMES + k) * channels + c
                            line = old[i] + (k + 1) / 256.0 * (
                                fresh[i] - old[i])
                            worst = max(worst, abs(got[i] - line))
                    after = (AT + 1) * PARTITION_FRAMES * channels
                    with self.subTest(mix=mix, channels=channels, move=tag):
                        self.assertLessEqual(worst, 1.0)
                        self.assertEqual(got[after:], fresh[after:])

    def test_out_of_range_is_clamped(self):
        values = noise_source(BLOCKS * PARTITION_FRAMES, CHANNELS)
        zero = gained(values, 0.5, wet_gain=0.0)
        for bad in (-1.0, float("nan"), float("-inf")):
            with self.subTest(value=bad):
                self.assertEqual(gained(values, 0.5, wet_gain=bad), zero)
        self.assertEqual(gained(values, 0.5, wet_gain=1e9),
                         gained(values, 0.5, wet_gain=4.0))

def peak_of(impulse, rate):
    """C13's reading, by hand, off one side's impulse (floats): the largest
    of the step response's magnitude and, for each square from 40 to 160 Hz
    in eighth-octave steps, the steady-state peak of the room convolved with
    a +-1 square of that half-period."""
    best = 0.0
    run = 0.0
    for tap in impulse:
        run += tap
        best = max(best, abs(run))
    for k in range(17):
        half = max(1, int(rate / (2.0 * 40.0 * 2.0 ** (k / 8.0)) + 0.5))
        fold = [0.0] * half
        for index, tap in enumerate(impulse):
            fold[index % half] += -tap if (index // half) % 2 else tap
        total = sum(fold)
        run = 0.0
        for value in fold:
            run += value
            best = max(best, abs(2.0 * run - total))
    return best


#: C13's rooms: (rate, taps, ir_channels, synthesize arguments).
PEAK_ROOMS = (
    (48000, 3840, 2, {"decay": 0.08, "damping_hz": 6000.0,
                      "diffusion_ms": 10.0, "seed": 1}),
    (48000, 3840, 2, {"decay": 0.05, "damping_hz": 500.0, "seed": 29}),
    (22050, 1792, 1, {"decay": 0.06, "damping_hz": 0.0, "predelay_ms": 20.0,
                      "seed": 7}),
    (8000, 1024, 2, {"decay": 0.1, "damping_hz": 900.0,
                     "diffusion_ms": 30.0, "seed": 36}),
)


class RoomPeakGainTest(unittest.TestCase):
    """C13: the room reports its own low-frequency peak gain."""

    def test_the_reading_is_the_rooms_own(self):
        for rate, taps, sides, room in PEAK_ROOMS:
            with self.subTest(rate=rate, room=room):
                _, out = click_energies(room, rate, taps, sides)
                # The click is 32767, so each word is the tap x 32767.
                want = max(peak_of([out[i] / 32767.0 for i in
                                    range(side, sides * taps, sides)], rate)
                           for side in range(sides))
                node = audioconvolve.Convolver(
                    max_taps=taps, ir_channels=sides, sample_rate=rate,
                    channel_count=sides)
                node.synthesize(**room)
                got = node.room_peak_gain
                self.assertLessEqual(abs(got - want),
                                     max(0.005, 0.005 * want), (got, want))
                # A dark room reads high and a bright one low: it is a
                # reading of this room, not a constant.
                self.assertGreater(got, 1.0)

    def test_the_reading_before_synthesize_is_the_one_after(self):
        # room_peak_gain_for reads the room the arguments would build, with
        # nothing built: equal to room_peak_gain after the synthesis, and the
        # node it is asked on keeps the room it had.
        for rate, taps, sides, room in PEAK_ROOMS:
            with self.subTest(rate=rate, room=room):
                node = audioconvolve.Convolver(
                    max_taps=taps, ir_channels=sides, sample_rate=rate,
                    channel_count=sides)
                node.synthesize(decay=0.07, seed=5)
                before = node.room_peak_gain
                ahead = node.room_peak_gain_for(**room)
                self.assertEqual(node.room_peak_gain, before)
                node.synthesize(**room)
                self.assertEqual(node.room_peak_gain, ahead)

    def test_a_dark_room_reads_higher_than_a_bright_one(self):
        node = audioconvolve.Convolver(max_taps=3840, ir_channels=2,
                                       sample_rate=48000)
        node.synthesize(decay=0.08, damping_hz=0.0, seed=1)
        bright = node.room_peak_gain
        node.synthesize(decay=0.08, damping_hz=500.0, seed=1)
        self.assertGreater(node.room_peak_gain, 2.0 * bright)

    def test_nothing_synthesized_reads_zero(self):
        node = audioconvolve.Convolver(max_taps=256, sample_rate=SAMPLE_RATE)
        self.assertEqual(node.room_peak_gain, 0.0)
        node.synthesize(decay=0.05, seed=3)
        self.assertGreater(node.room_peak_gain, 0.0)
        node.load(UNIT_IMPULSE)
        self.assertEqual(node.room_peak_gain, 0.0)

    def test_reading_it_changes_no_byte(self):
        # The same room rendered with the reading taken and through
        # build_room's old path renders the same words: the reading only
        # reads the taps.
        values = alternating(frames=2048)
        first = audioconvolve.Convolver(max_taps=1024, ir_channels=2,
                                        sample_rate=SAMPLE_RATE, mix=0.7)
        first.synthesize(decay=0.1, damping_hz=900.0, seed=36)
        first.room_peak_gain
        first.play(values)
        second = audioconvolve.Convolver(max_taps=1024, ir_channels=2,
                                         sample_rate=SAMPLE_RATE, mix=0.7)
        second.synthesize(decay=0.1, damping_hz=900.0, seed=36)
        second.play(alternating(frames=2048))
        self.assertEqual(words(first, 8), words(second, 8))


if __name__ == "__main__":
    unittest.main()
