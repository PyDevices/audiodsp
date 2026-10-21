"""audiograph's PHASER, LAG, LFO shapes and SAMPLE_HOLD, held to what they stand in for.

| node | held to | bar |
|---|---|---|
| `PHASER` | `audiobiquad.AllPass` on one channel, fed the same frequency, feedback and mix each 256-frame block | 0 counts |
| `LAG`, linear | vrack's `dsp/glide.py` `GlideCore` (OFF, ON, AUTO with legato, a new note mid-glide, the bend on top) | 0 counts |
| `LAG`, one-pole | vrack's `dsp/utility.py` `GlideCore` | 1 count |
| `LFO` triangle | the runner's triangle before the shapes existed, arithmetic for arithmetic | 0 counts |
| `LFO` sine, saw, square | vrack's `dsp/lfo.py` `LFOCore`'s shapes, on the runner's phase, over ten seconds | sine 1 count, saw and square 0 |
| `LFO` phase | `LFOCore`'s, at 0.1 to 30 Hz over ten seconds | 0.01 of a cycle (0.0065 measured) |
| `LFO` random | the square: a new level at each of its rising edges, held between | the same samples; levels in range and spread |
| `SAMPLE_HOLD`, clocked | vrack's `dsp/utility.py` `SampleAndHoldCore`, with no lag and with one | 0 counts; 1 count |
| `SAMPLE_HOLD`, two edges in a block | latches at each (`SampleAndHoldCore` keeps one a block) | the second edge's sample |
| `SAMPLE_HOLD`, rate | `audioshaper.SampleHold` at `num` = the sample rate, `den` = the rate | 0 counts |

vrack's cores run on numpy float32 blocks, a whole block per vector operation.
They are transcribed below operation by operation, each rounded to float32 the
way numpy rounds it, so this file needs neither vrack nor numpy; where numpy
calls a transcendental (`sin`, `exp`) the transcription rounds the exact value
to float32, which numpy's own may miss by an ulp. The wire is 16384 counts to
1.0, a power of two, so a value scaled onto it rounds exactly as it did in
float32 and the comparison is in counts.
"""

from array import array
import math
import unittest

import audiobiquad
import audiocore
import audiograph as ag
import audioshaper

RATE = 48000
UNIT = ag.UNIT

_F = array("f", [0.0])


def f32(value):
    """`value` rounded to float32, as a numpy float32 array stores it."""
    _F[0] = value
    return _F[0]


def sat(value):
    """A float on the wire as the runner stores it: rounded half to even."""
    value = round(value)
    return max(-32768, min(32767, value))


class Rig:
    """A graph whose inputs Python writes each block and whose node it reads."""

    def __init__(self, frames, kind, inputs, arg=0.0, open_=()):
        self.frames = frames
        self.g = ag.Graph(sample_rate=RATE, frames=frames, channel_count=1,
                          buffers=8, nodes=4)
        self.ins = [None if i in open_ else self.g.buffer() for i in range(inputs)]
        self.out = self.g.buffer()
        self.node = self.g.add(kind, ag.ALWAYS, tuple(self.ins), self.out, arg)
        self._block = array("h", bytes(2 * frames))

    def set(self, index, value):
        self.g.set(self.node, index, value)

    def run(self, *blocks):
        for buffer, values in zip(self.ins, blocks):
            if buffer is not None and values is not None:
                self.g.write(buffer, array("h", values))
        self.g.run(None)
        self.g.read(self.out, self._block)
        return list(self._block)


def worst(one, two):
    return max(abs(a - b) for a, b in zip(one, two))


# ------------------------------------------------------------------ PHASER --

def program(blocks):
    """A saw, a square and a little of each other, as integers, 0.45 FS."""
    values = array("h")
    for i in range(blocks * 256):
        saw = ((i * 211) % 4096) - 2048
        square = 2048 if (i // 37) % 2 else -2048
        values.append((saw * 3 + square * 2) * 14700 // 10240)
    return values


class Phaser(unittest.TestCase):

    def render_both(self, stages, settings):
        blocks = len(settings)
        source = program(blocks)
        ap = audiobiquad.AllPass(stages=stages, frequency=1000.0, feedback=0.0,
                                 mix=0.5, sample_rate=RATE, channel_count=1)
        ap.play(audiocore.RawSample(source, sample_rate=RATE, channel_count=1))
        rig = Rig(256, ag.PHASER, 1, float(stages))
        node_out, graph_out = [], []
        for index, (frequency, feedback, mix) in enumerate(settings):
            ap.frequency, ap.feedback, ap.mix = frequency, feedback, mix
            node_out += list(array("h", bytes(audiocore.get_buffer(ap)[1])))
            rig.set(ag.PHASER_FREQUENCY, frequency)
            rig.set(ag.PHASER_FEEDBACK, feedback)
            rig.set(ag.PHASER_MIX, mix)
            graph_out += rig.run(source[index * 256:(index + 1) * 256])
        return node_out, graph_out

    def test_matches_allpass_through_a_sweep(self):
        # Poly6's phase mode: a triangle sweep of the centre from 80 Hz up by
        # its intensity's ratio, set once a 256-frame block, feedback up to
        # 0.6 and the mix at 0.5, then a change of mind mid-render.
        settings = []
        for block in range(120):
            p = (block * 0.031) % 1.0
            tri = 2.0 * p if p < 0.5 else 2.0 * (1.0 - p)
            frequency = 80.0 * ((4200.0 / 80.0) ** tri)
            feedback = 0.6 if block < 80 else -0.35
            mix = 0.5 if block < 100 else 0.8
            settings.append((frequency, feedback, mix))
        node_out, graph_out = self.render_both(4, settings)
        self.assertGreater(max(abs(v) for v in graph_out), 4000)
        self.assertEqual(worst(node_out, graph_out), 0)

    def test_other_stage_counts(self):
        for stages in (1, 6):
            settings = [(300.0 + 40.0 * b, 0.25, 0.5) for b in range(20)]
            node_out, graph_out = self.render_both(stages, settings)
            self.assertEqual(worst(node_out, graph_out), 0, stages)

    def test_built_with_no_stages_is_four(self):
        settings = [(700.0, 0.5, 0.5)] * 6
        node_out, _ = self.render_both(4, settings)
        rig = Rig(256, ag.PHASER, 1)
        rig.set(ag.PHASER_FREQUENCY, 700.0)
        rig.set(ag.PHASER_FEEDBACK, 0.5)
        rig.set(ag.PHASER_MIX, 0.5)
        source = program(6)
        out = []
        for index in range(6):
            out += rig.run(source[index * 256:(index + 1) * 256])
        self.assertEqual(out, node_out)


# --------------------------------------------------------------------- LAG --

class GlideLinear:
    """vrack's dsp/glide.py GlideCore, in counts, numpy's float32 rounding."""

    def __init__(self, mode, time):
        self.mode, self.time = mode, time
        self.value = None
        self.target = 0.0
        self.slope = 0.0
        self.left = 0

    def process(self, note, pitch=None, legato=0.0):
        n = len(note)
        target = f32(note[0])
        if self.value is None:
            self.value = self.target = target
        if target != self.target:
            self.target = target
            steps = int(self.time * RATE)
            if steps >= 1 and (self.mode == 1 or (self.mode == 2 and legato > 0.5)):
                self.left = steps
                self.slope = (target - self.value) / steps
            else:
                self.left = 0
                self.value = target
        if self.left > 0:
            k = min(n, self.left)
            slope, start = f32(self.slope), f32(self.value)
            dst = [f32(f32((i + 1) * slope) + start) for i in range(k)]
            last = dst[k - 1]
            self.left -= k
            if self.left == 0:
                last = target
                dst[k - 1] = target
            self.value = last
            dst += [target] * (n - k)
        else:
            dst = [f32(self.value)] * n
        if pitch is not None:
            dst = [f32(f32(d + p) - m) for d, p, m in zip(dst, pitch, note)]
        return dst


class GlideOnePole:
    """vrack's dsp/utility.py GlideCore, in counts, float32 rounding."""

    def __init__(self, time):
        self.time = time
        self.value = 0.0

    def process(self, src):
        n = len(src)
        if self.time <= 0.001:
            self.value = float(src[n - 1])
            return [float(v) for v in src]
        coeff = math.exp(-1.0 / (self.time * RATE * 0.35))
        start, target = self.value, float(src[0])
        log = f32(math.log(coeff))
        dst = []
        for k in range(n):
            v = f32(math.exp(f32((k + 1) * log)))
            v = f32(f32(v * f32(start - target)) + f32(target))
            dst.append(v)
        self.value = dst[n - 1]
        return dst


def phrase(frames, blocks):
    """(note, pitch, legato) blocks: notes that step at block boundaries, one
    that steps mid-block, a bend that moves every sample, legato on some."""
    notes = [0, 0, 4096, 4096, 4096, 4096, -2731, -2731, 1365, 1365, 1365,
             9557, 9557, 9557, 9557, 9557, -8192, -8192, -8192, -8192]
    out = []
    for b in range(blocks):
        note = notes[b % len(notes)]
        block_note = [note] * frames
        if b == 13:
            block_note[frames // 2:] = [note + 2048] * (frames - frames // 2)
        bend = [((b * frames + i) * 7) % 801 - 400 for i in range(frames)]
        pitch = [n + d for n, d in zip(block_note, bend)]
        legato = [UNIT if b % 3 else 0] * frames
        out.append((block_note, pitch, legato))
    return out


class Lag(unittest.TestCase):

    def glide(self, mode, time, frames=128, blocks=60, bend=True):
        rig = Rig(frames, ag.LAG, 3)
        rig.set(ag.LAG_MODE, mode)
        rig.set(ag.LAG_TIME, time)
        ref = GlideLinear(mode, time)
        got, want = [], []
        for note, pitch, legato in phrase(frames, blocks):
            got += rig.run(note, pitch if bend else note, legato)
            want += [sat(v) for v in ref.process(
                note, pitch if bend else None, legato[0] / UNIT)]
        return got, want

    def test_glide_on_matches_glidecore(self):
        # 0.015625 s is 750 samples: a glide spans blocks and ends mid-block,
        # and several notes arrive before the last one arrived.
        got, want = self.glide(ag.LAG_ON, 0.015625)
        self.assertEqual(worst(got, want), 0)
        self.assertNotEqual(got, self.glide(ag.LAG_OFF, 0.015625)[0])

    def test_glide_off_and_auto_match_glidecore(self):
        for mode in (ag.LAG_OFF, ag.LAG_AUTO):
            for frames in (64, 128, 256):
                got, want = self.glide(mode, 0.03125, frames=frames)
                self.assertEqual(worst(got, want), 0, (mode, frames))

    def test_glide_without_a_bend(self):
        got, want = self.glide(ag.LAG_ON, 0.0078125, bend=False)
        self.assertEqual(worst(got, want), 0)

    def test_one_pole_matches_utility_glidecore(self):
        for time in (0.0, 0.004, 0.05, 0.4):
            rig = Rig(128, ag.LAG, 1)
            rig.set(ag.LAG_SHAPE, ag.LAG_ONE_POLE)
            rig.set(ag.LAG_TIME, time)
            ref = GlideOnePole(f32(time))
            got, want = [], []
            for note, _, _ in phrase(128, 80):
                got += rig.run(note)
                want += [sat(v) for v in ref.process(note)]
            self.assertLessEqual(worst(got, want), 1, time)

    def test_reset_forgets_the_pitch(self):
        rig = Rig(128, ag.LAG, 1)
        rig.set(ag.LAG_TIME, 0.5)
        rig.run([0] * 128)
        self.assertNotEqual(rig.run([8192] * 128)[-1], 8192)
        rig.g.reset(rig.node)
        self.assertEqual(rig.run([8192] * 128), [8192] * 128)


# --------------------------------------------------------------------- LFO --

class LFORef:
    """vrack's dsp/lfo.py LFOCore, numpy's float32 rounding, at +-0.5."""

    TWO_PI = f32(2.0 * math.pi)

    def __init__(self, frames):
        self.n = frames
        self.phase = 0.0

    def render(self, rate):
        dt = f32(rate / float(RATE))
        start = f32(self.phase)
        ph = [f32(f32((k + 1) * dt) + start) for k in range(self.n)]
        ph = [f32(p - math.floor(p)) for p in ph]
        self.phase = ph[-1]
        return ph

    @classmethod
    def sine(cls, p):
        return f32(f32(math.sin(f32(p * cls.TWO_PI))) * 0.5)

    @staticmethod
    def saw(p):
        return f32(f32(f32(p * -2.0) + 1.0) * 0.5)

    @staticmethod
    def square(p):
        return f32(f32(f32(math.floor(f32(p * 2.0)) * -2.0) + 1.0) * 0.5)


def runner_phases(rate, samples):
    """The runner's phase, as it has always advanced it: one step a sample,
    wrapped each sample, in float32."""
    dt = f32(f32(rate) / f32(RATE))
    phase, out = 0.0, []
    for _ in range(samples):
        phase = f32(phase + dt)
        phase = f32(phase - math.floor(phase))
        out.append(phase)
    return out


def runner_triangle(rate, amp, samples):
    """The triangle as the runner computed it before it had other shapes."""
    out = []
    for phase in runner_phases(rate, samples):
        tri = f32(f32(4.0 * phase) - 2.0)
        tri = f32(1.0 - abs(tri))
        out.append(sat(f32(tri * amp)))
    return out


class Lfo(unittest.TestCase):
    """The shapes are LFOCore's, each a function of the phase. The phase is
    the triangle's, which this change keeps to the bit, so a saw, a square and
    a triangle at one rate stay one LFO. It is not quite LFOCore's phase:
    LFOCore restarts a ramp from the phase each block and stays within 5e-5
    of a cycle of exact, where the runner adds a float32 step each sample and
    runs up to 0.06% off its rate (0.0065 of a cycle over ten seconds at
    1 Hz). Matching LFOCore's phase would move the triangle."""

    def render(self, shape, rate, frames=128, blocks=3750, amplitude=UNIT / 2):
        rig = Rig(frames, ag.LFO, 0)
        rig.set(ag.LFO_RATE, rate)
        rig.set(ag.LFO_AMPLITUDE, amplitude)
        if shape is not None:
            rig.set(ag.LFO_SHAPE, shape)
        out = []
        for _ in range(blocks):
            out += rig.run()
        return out

    def test_the_triangle_has_not_moved(self):
        for rate, amp in ((5.0, 8192.0), (0.37, 1024.0), (23.0, 16384.0)):
            got = self.render(None, rate, blocks=400, amplitude=amp)
            self.assertEqual(got, runner_triangle(rate, amp, 128 * 400), rate)
            same = self.render(ag.LFO_TRIANGLE, rate, blocks=400, amplitude=amp)
            self.assertEqual(got, same)

    def test_sine_saw_and_square_are_lfocores_shapes(self):
        for wave, shape, bar in ((LFORef.sine, ag.LFO_SINE, 1),
                                 (LFORef.saw, ag.LFO_SAW, 0),
                                 (LFORef.square, ag.LFO_SQUARE, 0)):
            for rate in (0.1, 5.5, 30.0):
                got = self.render(shape, rate)
                want = [sat(f32(wave(p) * UNIT))
                        for p in runner_phases(rate, len(got))]
                self.assertLessEqual(worst(got, want), bar, (shape, rate))

    def test_the_phase_tracks_lfocores(self):
        for rate in (0.1, 0.37, 5.5, 30.0):
            core, theirs = LFORef(128), []
            for _ in range(3750):
                theirs += core.render(rate)
            ours = runner_phases(rate, len(theirs))
            gap = max(min(abs(a - b), 1.0 - abs(a - b))
                      for a, b in zip(ours, theirs))
            self.assertLess(gap, 0.01, rate)

    def test_random_changes_at_each_square_rising_edge(self):
        rate = 29.75
        got = self.render(ag.LFO_RANDOM, rate, blocks=3000)
        square = [LFORef.square(p) for p in runner_phases(rate, len(got))]
        rises = [i for i in range(len(square))
                 if square[i] > 0 and (i == 0 or square[i - 1] < 0)]
        changes = [i for i in range(len(got)) if i == 0 or got[i] != got[i - 1]]
        self.assertEqual(changes, rises)
        levels = [got[i] for i in rises]
        self.assertTrue(all(-8192 <= v <= 8192 for v in levels))
        # 238 draws from 16385 levels: a repeat or two is the birthday odds.
        self.assertGreater(len(set(levels)), len(levels) * 95 // 100)
        self.assertLess(abs(sum(levels) / len(levels)), 1200)
        self.assertGreater(max(levels), 6000)
        self.assertLess(min(levels), -6000)

    def test_random_draws_at_once_when_chosen(self):
        rig = Rig(128, ag.LFO, 0)
        rig.set(ag.LFO_RATE, 1.0)
        rig.run()
        rig.set(ag.LFO_SHAPE, ag.LFO_RANDOM)
        first = rig.run()
        self.assertNotEqual(first[0], 0)
        self.assertEqual(set(first), {first[0]})

    def test_shapes_share_one_phase(self):
        # A saw and a square at one rate are one LFO's two outputs: the
        # square is high exactly while the saw is above zero.
        saw = self.render(ag.LFO_SAW, 3.0, blocks=200)
        square = self.render(ag.LFO_SQUARE, 3.0, blocks=200)
        for s, q in zip(saw, square):
            if abs(s) > 2:
                self.assertEqual(s > 0, q > 0)


# ------------------------------------------------------------- SAMPLE_HOLD --

class HoldRef:
    """vrack's dsp/utility.py SampleAndHoldCore with its EdgeDetector, in
    counts: one rising edge a block, the clock high above 0.1."""

    def __init__(self):
        self.high = False
        self.held = 0.0
        self.out = 0.0

    def render(self, signal, clock, lag):
        n = len(clock)
        step = [c > 0.1 * UNIT for c in clock]
        edge = -1
        if step[0] and not self.high:
            edge = 0
        else:
            for i in range(1, n):
                if step[i] and not step[i - 1]:
                    edge = i
                    break
        self.high = step[n - 1]
        dst = [0.0] * n
        if edge < 0:
            self.segment(dst, 0, n, lag)
            return dst
        self.segment(dst, 0, edge, lag)
        self.held = float(signal[edge])
        self.segment(dst, edge, n, lag)
        return dst

    def segment(self, dst, start, end, lag):
        if end <= start:
            return
        if lag <= 0.0:
            for i in range(start, end):
                dst[i] = self.held
            self.out = self.held
            return
        log = f32(math.log(lag))
        begin, target = self.out, self.held
        for k in range(end - start):
            v = f32(math.exp(f32((k + 1) * log)))
            dst[start + k] = f32(f32(v * f32(begin - target)) + f32(target))
        self.out = dst[end - 1]


def held_signal(frames, blocks):
    return [[((b * frames + i) * 389) % 30001 - 15000 for i in range(frames)]
            for b in range(blocks)]


def clock_blocks(frames, blocks, period, width):
    out = []
    for b in range(blocks):
        out.append([UNIT if ((b * frames + i) % period) < width else 0
                    for i in range(frames)])
    return out


class SampleHold(unittest.TestCase):

    def hold(self, lag, frames=128, blocks=80, period=301, width=40):
        rig = Rig(frames, ag.SAMPLE_HOLD, 2)
        rig.set(ag.SH_LAG, lag)
        ref = HoldRef()
        got, want = [], []
        for signal, clock in zip(held_signal(frames, blocks),
                                 clock_blocks(frames, blocks, period, width)):
            got += rig.run(signal, clock)
            want += [sat(v) for v in ref.render(signal, clock, f32(lag))]
        return got, want

    def test_matches_sampleandholdcore(self):
        got, want = self.hold(0.0)
        self.assertEqual(worst(got, want), 0)
        self.assertGreater(len(set(got)), 30)

    def test_with_a_lag(self):
        for lag in (0.5, 0.99, 0.999):
            got, want = self.hold(lag)
            self.assertLessEqual(worst(got, want), 1, lag)

    def test_every_edge_in_a_block_latches(self):
        rig = Rig(128, ag.SAMPLE_HOLD, 2)
        signal = list(range(0, 1280, 10))
        clock = [UNIT if 10 <= i < 20 or 70 <= i < 80 else 0 for i in range(128)]
        out = rig.run(signal, clock)
        self.assertEqual(out[9], 0)
        self.assertEqual(out[10:70], [100] * 60)
        self.assertEqual(out[70:], [700] * 58)

    def test_rate_matches_audioshaper(self):
        for rate in (26040, 9000, 48000):
            blocks = 24
            source = array("h", [v for block in held_signal(128, blocks) for v in block])
            node = audioshaper.SampleHold(
                audiocore.RawSample(source, sample_rate=RATE, channel_count=1),
                num=RATE, den=rate)
            want = []
            while len(want) < len(source):
                want += list(array("h", bytes(audiocore.get_buffer(node)[1])))
            rig = Rig(128, ag.SAMPLE_HOLD, 2, open_=(1,))
            rig.set(ag.SH_RATE, rate)
            got = []
            for b in range(blocks):
                got += rig.run(source[b * 128:(b + 1) * 128], None)
            self.assertEqual(got, want[:len(got)], rate)

    def test_open_clock_and_no_rate_holds(self):
        g = ag.Graph(sample_rate=RATE, frames=128, channel_count=1, buffers=2, nodes=1)
        src, out = g.buffer(), g.buffer()
        g.add(ag.SAMPLE_HOLD, ag.ALWAYS, (src, None), out)
        g.write(src, array("h", [999] * 128))
        g.run(None)
        block = array("h", bytes(256))
        g.read(out, block)
        self.assertEqual(list(block), [0] * 128)


if __name__ == "__main__":
    unittest.main()
