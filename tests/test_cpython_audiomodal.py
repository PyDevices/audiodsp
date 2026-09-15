"""audiomodal: the traits and the surface the renders cannot check.

`tests/parity/modal_probe.py` pins what this module renders across the three
targets. What is left for here is what a render cannot say, and the numeric
traits with their bars.

`docs/correctness-standard.md` is what this file implements for `audiomodal`:
the module is ours - upstream CircuitPython has no counterpart - so it is held
to three-target agreement plus the traits below, never to a previous version of
its own output.

## The traits, with their bars

| ID | Trait | Bar |
|---|---|---|
| M1 | With every mode silent it is silence, not a passthrough | exact |
| M2 | At `mix=0` it is a bit-exact wire, at the rails included | exact, 0 LSB |
| M3 | A mode's measured frequency is the one asked for | 0.5% |
| M4 | A mode's measured 60 dB decay is the one asked for | 5% |
| M5 | A mode's peak is its `gain` times the excitation | 2% |
| M6 | Every tail reaches **exact** zero, and stays there | exact |
| M7 | `ringing` goes false only when the last mode has arrived | exact |
| M8 | Modes sum linearly: N together is the sum of N alone | 1 LSB |
| M9 | A decay `audiobiquad` refuses (Q ~ 4000) is stable and accurate | M3/M4 |
| M10 | `clear()` leaves the node as a freshly built one | exact |
| M11 | A starved node yields a full block, and keeps ringing through it | exact |

**M2 and M6 are this module's form of the identity trait** that
`docs/correctness-standard.md` asks of every own node: an exact answer
*through* the DSP rather than around it, evaluated at full scale, because that
is where an arithmetic overflow shows and a range check does not.

M1 is worth reading twice, because the obvious expectation is wrong. The
default `mix` is 1.0 - fully resonated - and an unconfigured bank has no modes,
so what comes out is silence rather than the source. That is the consistent
answer (wet means wet) and it is the one `modal_probe.py`'s "unset" section
pins, but it is not what a reader guesses, so it is a trait rather than a
footnote.

M6 is the whole reason the node can be cheap. Because a finished mode is
*exactly* zero rather than nearly zero, the process loop can skip it on a test
that cannot be wrong by a fraction of an LSB, and a kit holding ten drums
resident pays only for the ones sounding. A tail that merely decayed toward
zero would make that skip a lie.

M7 is the same fact read from Python, and the bar is exact for the same reason.
Note what it does *not* say: `ringing` stays true far longer than the decay
time, because a 60 dB decay is nowhere near the flush threshold at 1e-20 -
about -400 dB. A mode with a 0.5 s decay is silent to any ear in half a second
and `ringing` for roughly 3.3.

M9 is the trait the module exists for at the top end. `audioif_filter_f32.c`
caps Q at 60 and says why; a 2 kHz partial ringing for four seconds is Q =
3638. The claim is not that a high Q is special-cased, it is that asking in
seconds never reaches the coefficient corner the cap was protecting against.

## The planted faults

`docs/correctness-standard.md`: "a trait without a planted fault is not a
check; it is a hope." Each fault below is a one-line change to
`src/shared/audioif_modal.c` that a reader can make by hand, with what it did
to the traits when it was made. They are recorded rather than automated
because the file they break is C: the harness that would plant them is a
rebuild, and `tests/parity/deinit_surface_probe.py --fault` is the shape to
copy if that is ever worth mechanising.

| Fault | Breaks | What was measured |
|---|---|---|
| `flush_pair()` returns without zeroing | M6, M7 | tail never arrives (`modal_probe` prints -1); `ringing` true after 600 blocks |
| `flush_pair()` flushes each word on its own | M7 | **this was the real bug, not a planted one** - see below |
| skip test drops the `x0 == 0.0f` term | M8, M11 | a ringing mode stops advancing while fed signal; second strike sums onto a stale state |
| `b0 = gain` rather than `gain * sin(w0)` | M5 | peak scales with 1/sin(w0): 21x high at 55 Hz / 8 kHz |
| `a2 = r` rather than `r * r` | M3, M4 | 2 kHz mode lands at 1147 Hz and decays in 0.11 s instead of 4.0 |
| range reduction dropped from `exp_neg` | M4 | short decays wrong: 1 ms asked, 1.8 ms measured |

The second row is the one worth reading, because it was written the wrong way
first and M7 is what found it. Flushing `s1` and `s2` independently -- which is
what `audioif_filter_f32.c` does, safely, one order of Q lower -- gave a stable
limit cycle *above* the threshold: at 220 Hz, 0.125 s decay, 8 kHz, the state
parked at ~2.1e-19 and was still there after two thousand blocks, thirty-eight
seconds, with the output silent from block 30 onward. M6 passed the whole time,
because M6 reads the *output* and the output had long since rounded to zero.
Only M7, which reads the state, could see it.

The mechanism is in `flush_pair`'s comment: `-a1` is close to 2 for a high-Q
pole, so zeroing the smaller word on its own lets the next sample nearly double
the larger one, which refills the small one, for ever. That is a failure mode a
resonator bank has and a filter effectively does not, which is exactly the kind
of thing an own-node's traits exist to catch -- there is no oracle that would
have said anything.
"""

import math
import unittest
from array import array

import audiocore
import audiomodal

SAMPLE_RATE = 8000
BLOCK = audiomodal.FRAMES

#: (frequency, decay, gain), exact binary fractions so nothing rounds.
KICK = ((55.0, 0.5, 1.0), (87.0, 0.25, 0.5), (117.0, 0.125, 0.25))


def impulse(frames=BLOCK, level=12000, channels=1, rate=SAMPLE_RATE):
    values = array("h", bytes(frames * channels * 2))
    for channel in range(channels):
        values[channel] = level
    return audiocore.RawSample(values, sample_rate=rate,
                               channel_count=channels)


def rails(frames=512, channels=1):
    """Full-scale alternating extremes -- where an overflow shows."""
    values = array("h", bytes(frames * channels * 2))
    for index in range(frames * channels):
        values[index] = 32767 if index % 2 else -32768
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def render(bank, blocks, channels=1):
    out = []
    for _ in range(blocks):
        data = bytes(audiocore.get_buffer(bank)[1])
        for position in range(0, len(data), 2):
            word = data[position] | (data[position + 1] << 8)
            out.append(word - 65536 if word >= 32768 else word)
    return out


def strike(bank, blocks, channels=1, rate=SAMPLE_RATE):
    """Hit it once, then let it ring into silence.

    `audiocore.RawSample` LOOPS when it is pulled directly, so simply playing
    a short impulse and reading on re-strikes the bank every buffer length --
    which looks exactly like a decay that never arrives. One block with the
    source, then `stop()`, and the starved path feeds the recursion zeros,
    which is what rings it out.
    """
    bank.play(impulse(channels=channels, rate=rate))
    out = render(bank, 1, channels)
    bank.stop()
    return out + render(bank, blocks - 1, channels)


def build(table, channels=1, rate=SAMPLE_RATE, **options):
    bank = audiomodal.Bank(modes=len(table), sample_rate=rate,
                           channel_count=channels, **options)
    bank.set_modes(table)
    return bank


def zero_crossing_hz(samples, skip=64, rate=SAMPLE_RATE):
    """Frequency from counting sign changes -- no FFT, no window, no leakage."""
    body = samples[skip:]
    crossings = 0
    first = last = None
    for index in range(1, len(body)):
        if body[index - 1] < 0 <= body[index]:
            crossings += 1
            if first is None:
                first = index
            last = index
    if first is None or last is None or crossings < 2:
        return 0.0
    return (crossings - 1) * rate / float(last - first)


def decay_seconds(samples):
    """60 dB time from the envelope's peak to a thousandth of it."""
    peak = max(abs(value) for value in samples)
    if peak == 0:
        return -1.0
    start = next(i for i, v in enumerate(samples) if abs(v) == peak)
    target = peak / 1000.0
    for index in range(start, len(samples)):
        if max(abs(v) for v in samples[index:index + 64]) < target:
            return (index - start) / float(SAMPLE_RATE)
    return -1.0


class ModalTraits(unittest.TestCase):

    def test_m1_unset_is_silence_not_a_passthrough(self):
        bank = audiomodal.Bank(modes=4, sample_rate=SAMPLE_RATE,
                               channel_count=1)
        bank.play(impulse())
        self.assertEqual(set(render(bank, 4)), {0})

    def test_m2_mix_zero_is_a_bit_exact_wire_at_the_rails(self):
        bank = build(KICK, mix=0.0)
        source = rails()
        bank.play(source)
        got = render(bank, 2)
        expected = []
        reference = rails()
        for _ in range(2):
            data = bytes(audiocore.get_buffer(reference)[1])
            for position in range(0, len(data), 2):
                word = data[position] | (data[position + 1] << 8)
                expected.append(word - 65536 if word >= 32768 else word)
        self.assertEqual(got, expected[:len(got)])

    def test_m3_frequency_is_what_was_asked_for(self):
        for frequency in (55.0, 220.0, 1000.0):
            with self.subTest(frequency=frequency):
                bank = build(((frequency, 2.0, 1.0),))
                measured = zero_crossing_hz(strike(bank, 16))
                self.assertAlmostEqual(measured / frequency, 1.0, delta=0.005)

    def test_m4_decay_is_what_was_asked_for(self):
        for decay in (0.125, 0.5, 2.0):
            with self.subTest(decay=decay):
                bank = build(((220.0, decay, 1.0),))
                measured = decay_seconds(strike(bank, 256))
                self.assertGreater(measured, 0.0)
                self.assertAlmostEqual(measured / decay, 1.0, delta=0.05)

    def test_m5_peak_is_gain_times_the_excitation(self):
        for gain in (0.25, 0.5, 1.0):
            with self.subTest(gain=gain):
                bank = build(((220.0, 1.0, gain),))
                peak = max(abs(v) for v in strike(bank, 16))
                self.assertAlmostEqual(peak / (12000.0 * gain), 1.0,
                                       delta=0.02)

    def test_m6_the_tail_reaches_exact_zero_and_stays(self):
        bank = build(((220.0, 0.125, 1.0),))
        samples = strike(bank, 400)
        last_loud = max(i for i, v in enumerate(samples) if v != 0)
        self.assertLess(last_loud, len(samples) - BLOCK)
        self.assertEqual(set(samples[last_loud + 1:]), {0})

    def test_m7_ringing_is_false_only_once_the_tail_has_arrived(self):
        bank = build(((220.0, 0.125, 1.0),))
        self.assertFalse(bank.ringing)
        bank.play(impulse())
        render(bank, 1)
        self.assertTrue(bank.ringing)
        bank.stop()
        for _ in range(400):
            render(bank, 1)
            if not bank.ringing:
                break
        self.assertFalse(bank.ringing)
        # Once it says silent it must BE silent: one more block, all zero.
        self.assertEqual(set(render(bank, 1)), {0})

    def test_m8_modes_sum_linearly(self):
        alone = []
        for mode in KICK:
            alone.append(strike(build((mode,)), 8))
        summed = strike(build(KICK), 8)
        for index, value in enumerate(summed):
            total = sum(track[index] for track in alone)
            self.assertLessEqual(abs(value - total), 1,
                                 "sample %d: %d vs %d" % (index, value, total))

    def test_m9_a_decay_audiobiquad_would_refuse(self):
        # Q = pi * f * T60 / ln(1000) = 3638, against audiobiquad's cap of 60.
        frequency, decay = 2000.0, 4.0
        q = math.pi * frequency * decay / math.log(1000.0)
        self.assertGreater(q, 60.0)
        # At 48 kHz, not this file's 8 kHz: 2 kHz is Nyquist/2 there, and
        # counting zero crossings four samples apart measures the sample rate
        # rather than the mode.
        rate = 48000
        bank = build(((frequency, decay, 1.0),), rate=rate)
        samples = strike(bank, 380, rate=rate)
        self.assertAlmostEqual(
            zero_crossing_hz(samples, rate=rate) / frequency, 1.0, delta=0.005)
        self.assertLess(max(abs(v) for v in samples), 32768)

    def test_m10_clear_leaves_a_freshly_built_node(self):
        bank = build(KICK)
        strike(bank, 4)
        bank.clear()
        after = strike(bank, 4)
        self.assertEqual(after, strike(build(KICK), 4))

    def test_m11_a_starved_node_yields_a_full_block_and_keeps_ringing(self):
        bank = build(((220.0, 1.0, 1.0),))
        bank.play(impulse())
        render(bank, 1)
        bank.stop()
        data = bytes(audiocore.get_buffer(bank)[1])
        self.assertEqual(len(data), BLOCK * 2)
        # Still sounding with no source at all -- the opposite of what
        # audioecho.FeedbackDelay and audioverb.Tank do, deliberately.
        self.assertNotEqual(set(data), {0})

    def test_the_surface_refuses_what_it_should(self):
        with self.assertRaises(ValueError):
            audiomodal.Bank(modes=0)
        with self.assertRaises(ValueError):
            audiomodal.Bank(modes=audiomodal.MAX_MODES + 1)
        with self.assertRaises(ValueError):
            audiomodal.Bank(modes=4, channel_count=3)
        with self.assertRaises(TypeError):
            audiomodal.Bank(modes=4, nonsense=1.0)
        bank = audiomodal.Bank(modes=2)
        with self.assertRaises(IndexError):
            bank.set_mode(2, 100.0, 0.1, 1.0)
        with self.assertRaises(ValueError):
            bank.set_modes(((1.0, 2.0, 3.0),) * 3)

    def test_a_short_table_silences_the_rows_it_did_not_reach(self):
        bank = build(KICK + ((330.0, 0.5, 1.0),))
        with_four = strike(bank, 8)
        bank.clear()
        bank.set_modes(KICK)
        with_three = strike(bank, 8)
        self.assertNotEqual(with_four, with_three)
        self.assertEqual(with_three, strike(build(KICK), 8))


if __name__ == "__main__":
    unittest.main()
