"""audiograph's PHASER, LAG, LFO shapes, SAMPLE_HOLD, TWOPOLE, OVERDRIVE, TANH, PAN, WAVEFOLD, LPG and MODAL, held to what they stand in for.

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
| `TWOPOLE`, CMOS low-pass, band-pass, high-pass | vrack's `dsp/twopole.py` `CMOSFilterCore`, with the cutoff CV still, stepping between blocks, running through a block, and not connected | 0 counts |
| `TWOPOLE`, Sallen-Key low-pass and high-pass | `SallenKeyLPFCore` and `SallenKeyHPFCore`, the same four CV cases | 1 count, on under 1 sample in 100 (the float `tanh`) |
| `OVERDRIVE` | vrack's `OverdriveCore`: `audiofilters.Distortion` in overdrive mode with soft clipping, then `audiofilters.Filter` with a `synthio.Biquad` high shelf, driven the way vrack's bridge drives them | 0 counts |
| `TANH` | `level * UNIT * tanh(drive * x / UNIT)` in double precision, over a ramp through the clip, a program, and the knobs moving | 1 count (a few float ulps of tanh, which moves a half-count tie the other way) |
| `PAN` | two `MIX` nodes, one a side, given the balance's gains in float32: centred, panned, past the ends, with gains and pans moving between blocks, an open input, a reset and a skipped group | 0 counts |
| `WAVEFOLD` | vrack's `dsp/wavefolder.py` `WavefolderCore`, with no CV, a fold CV, the knobs moving, a hot input, an open input and the bypass at fold 0.001 | 1 count, on about 1 sample in 1000 (the sine is a polynomial) |
| `LPG` | vrack's `dsp/lpg.py` `LowPassGateCore`, its filter `audiofilters.Filter` with a `synthio.Biquad` low-pass driven as vrack's bridge drives it: VCF, VCA and BOTH, struck, on a control, with the manual level, the knobs and level moving, a hot input and an open one | 1 count (the node's own exp and pow against libm's); 0 measured over 168192 samples |
| `MODAL` | vrack's `dsp/modal.py` `ModalResonatorCore`: its `Exciter` in float32, the sum cast to int16 as vrack's bridge casts it, and `audiomodal.Bank` pulled 256 frames at a time, lined up with its input: bar and membrane ratios, struck, rung by noise, mix 0, a mode turned off, the knobs moving, and 32 to 256 frames | 1 count (the strike's burst is a product where numpy takes an exp); 586 of 170240 samples measured |

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
import audiofilters
import audiograph as ag
import audioshaper
import synthio

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


# ---------------------------------------------------------------- TWOPOLE --
#
# dsp/twopole.py reads a numpy float32 block, and numpy 2 keeps a float32
# scalar float32 beside a Python float: so with a cutoff CV connected the
# coefficient is worked out in float32 steps (the CV times the depth, times
# ten, the power of two, the cutoff, the angle), and the loop is float32 apart
# from the Python-float coefficients it multiplies by, which are rounded to
# float32 as they meet a float32. Each operation below is rounded that way.
# `math.tanh` is the double one, rounded when it lands in a float32.

PI32 = f32(math.pi)


def m(a, b):
    return f32(f32(a) * f32(b))


def a_(a, b):
    return f32(f32(a) + f32(b))


def s_(a, b):
    return f32(f32(a) - f32(b))


def twopole_coefficients(frames, cutoff, cv, depth):
    """dsp/twopole.py `_coefficients`: (g, per-sample step), both double."""
    inv_sr = 1.0 / RATE
    top = RATE * 0.45

    def g_at(c):
        if cv is None:
            fc, narrow = cutoff * 2.0 ** (c * depth * 10.0), False
        else:
            t = m(m(c, depth), 10.0)
            fc, narrow = m(cutoff, f32(2.0 ** t)), True
        if not fc <= top:
            fc, narrow = top, False
        if not fc > 20.0:
            fc, narrow = 20.0, False
        if narrow:
            return min(math.tan(m(m(PI32, fc), f32(inv_sr))), 0.95)
        return min(math.tan(math.pi * fc * inv_sr), 0.95)

    c0 = cv[0] / UNIT if cv is not None else 0.0
    c1 = cv[frames - 1] / UNIT if cv is not None else 0.0
    g = g_at(c0)
    if c0 != c1:
        return g, (g_at(c1) - g) / float(frames)
    return g, 0.0


def diode(v):
    return math.tanh(m(v, 1.5)) / 1.5


def cmos_clip(v):
    v = f32(v)
    w = m(v, s_(1.2, m(0.2, m(v, v))))
    return max(-1.0, min(1.0, w))


class TwoPoleRef:
    """`SallenKeyLPFCore`, `SallenKeyHPFCore` and `CMOSFilterCore`, one
    sample at a time. `kind` is 0 and 1 for the Sallen-Key pair, 2, 3 and 4
    for the CMOS filter's LP, BP and HP."""

    def __init__(self, kind):
        self.kind = kind
        self.s1 = self.s2 = 0.0

    def render(self, src, cv, cutoff, depth, resonance, drive):
        frames = len(src)
        g, step = twopole_coefficients(frames, cutoff, cv, depth)
        out = []
        s1, s2 = self.s1, self.s2
        x_in = [0.0 if v is None else v / UNIT for v in src]
        if self.kind == 0:
            k = resonance * 2.15
            h = g / (1.0 + g)
            for i in range(frames):
                x = m(x_in[i], drive)
                if step != 0.0:
                    g += step
                    h = g / (1.0 + g)
                fb = diode(m(s2, k))
                v1 = m(s_(a_(x, fb), s1), h)
                node1 = a_(v1, s1)
                s1 = a_(node1, v1)
                v2 = m(s_(node1, s2), h)
                lp = a_(v2, s2)
                s2 = a_(lp, v2)
                out.append(f32(math.tanh(lp)))
        elif self.kind == 1:
            k = resonance * 2.15
            k_term = 2.0 - k * 0.5
            denom = 1.0 / (1.0 + 2.0 * g + g * g)
            for i in range(frames):
                x = m(x_in[i], drive)
                if step != 0.0:
                    g += step
                    denom = 1.0 / (1.0 + 2.0 * g + g * g)
                fb = diode(m(s2, k))
                hp = m(s_(s_(a_(x, fb), m(k_term, s1)), s2), denom)
                v1 = m(g, hp)
                node1 = a_(v1, s1)
                s1 = a_(node1, v1)
                v2 = m(g, node1)
                lp = a_(v2, s2)
                s2 = a_(lp, v2)
                out.append(f32(math.tanh(hp)))
        else:
            r = max(0.02, 1.0 - resonance * 0.96)
            denom = 1.0 / (1.0 + g * (g + r))
            for i in range(frames):
                x = cmos_clip(m(x_in[i], drive))
                if step != 0.0:
                    g += step
                    denom = 1.0 / (1.0 + g * (g + r))
                hp = cmos_clip(m(s_(s_(x, m(r + g, s1)), s2), denom))
                v1 = m(g, hp)
                bp = cmos_clip(a_(v1, s1))
                s1 = a_(bp, v1)
                v2 = m(g, bp)
                lp = cmos_clip(a_(v2, s2))
                s2 = a_(lp, v2)
                out.append(lp if self.kind == 2 else (bp if self.kind == 3 else hp))
        self.s1, self.s2 = s1, s2
        return [sat(v * UNIT) for v in out]


def twopole_input(frames, blocks, amplitude=9000):
    return [[((b * frames + i) * 211) % 4096 * amplitude // 2048 - amplitude
             + (amplitude // 3 if ((b * frames + i) // 37) % 2 else 0)
             for i in range(frames)] for b in range(blocks)]


def cv_still(frames, blocks):
    return [[(b // 6) * 700 - 1400] * frames for b in range(blocks)]


def cv_stepping(frames, blocks):
    """A new value every block, none inside one."""
    return [[((b * 977) % 5400) - 2700] * frames for b in range(blocks)]


def cv_running(frames, blocks):
    """Different at the block's two ends, and a wobble between."""
    return [[int(3000 * math.sin((b * frames + i) * 0.011)) for i in range(frames)]
            for b in range(blocks)]


class TwoPole(unittest.TestCase):

    CASES = (("still", cv_still), ("stepping", cv_stepping),
             ("running", cv_running), ("open", None))

    def compare(self, kind, cv_case, frames=128, blocks=60, cutoff=1000.0,
                depth=1.0, resonance=0.625, drive=1.5):
        rig = Rig(frames, ag.TWOPOLE, 2, open_=(1,) if cv_case is None else ())
        rig.set(ag.TWOPOLE_TYPE, kind)
        rig.set(ag.TWOPOLE_CUTOFF_HZ, cutoff)
        rig.set(ag.TWOPOLE_DEPTH, depth)
        rig.set(ag.TWOPOLE_RESONANCE, resonance)
        rig.set(ag.TWOPOLE_DRIVE, drive)
        ref = TwoPoleRef(kind)
        signal = twopole_input(frames, blocks)
        cvs = cv_case(frames, blocks) if cv_case else [None] * blocks
        got, want = [], []
        for x, cv in zip(signal, cvs):
            got += rig.run(x, cv)
            want += ref.render(x, cv, cutoff, depth, resonance, drive)
        return got, want

    def test_cmos_filter_matches_at_zero_counts(self):
        for kind in (2, 3, 4):
            for name, case in self.CASES:
                for resonance, drive in ((0.625, 1.5), (0.9375, 3.0)):
                    got, want = self.compare(kind, case, resonance=resonance,
                                             drive=drive)
                    self.assertGreater(max(abs(v) for v in got), 1000, (kind, name))
                    self.assertEqual(worst(got, want), 0, (kind, name, resonance))

    def test_sallen_key_filters_match_within_a_count(self):
        for kind in (0, 1):
            for name, case in self.CASES:
                for resonance, drive in ((0.5, 1.0), (0.875, 2.0)):
                    got, want = self.compare(kind, case, resonance=resonance,
                                             drive=drive,
                                             cutoff=1000.0 if kind == 0 else 500.0)
                    self.assertGreater(max(abs(v) for v in got), 1000, (kind, name))
                    self.assertLessEqual(worst(got, want), 1, (kind, name, resonance))
                    differ = sum(1 for a, b in zip(got, want) if a != b)
                    self.assertLess(differ, len(got) // 100, (kind, name, resonance))

    def test_other_depths_and_rates_of_cutoff(self):
        for cutoff, depth in ((20.0, 1.0), (4000.0, 0.5), (11000.0, 1.0),
                              (250.0, 1.75)):
            got, want = self.compare(2, cv_running, cutoff=cutoff, depth=depth)
            self.assertEqual(worst(got, want), 0, (cutoff, depth))

    def test_only_the_ends_of_the_cv_are_read(self):
        # the cutoff walks from the first sample's CV to the last's: what
        # lies between them does not move it
        frames = 128
        rig_a = Rig(frames, ag.TWOPOLE, 2)
        rig_b = Rig(frames, ag.TWOPOLE, 2)
        for rig in (rig_a, rig_b):
            rig.set(ag.TWOPOLE_TYPE, ag.TWOPOLE_CMOS_LOWPASS)
            rig.set(ag.TWOPOLE_RESONANCE, 0.75)
        signal = twopole_input(frames, 8)
        for x in signal:
            ramp = [i * 20 for i in range(frames)]
            wobble = [ramp[0]] + [ramp[i] + (700 if i % 5 else -700)
                                  for i in range(1, frames - 1)] + [ramp[-1]]
            self.assertEqual(rig_a.run(x, ramp), rig_b.run(x, wobble))

    def test_reset_starts_the_filter_again(self):
        frames = 128
        signal = twopole_input(frames, 4)
        cvs = cv_running(frames, 4)
        for kind in range(5):
            rig = Rig(frames, ag.TWOPOLE, 2)
            rig.set(ag.TWOPOLE_TYPE, kind)
            rig.set(ag.TWOPOLE_RESONANCE, 0.75)
            first = [rig.run(x, cv) for x, cv in zip(signal, cvs)]
            rig.g.reset(rig.node)
            again = [rig.run(x, cv) for x, cv in zip(signal, cvs)]
            self.assertEqual(first, again, kind)

    def test_an_unknown_type_is_the_sallen_key_low_pass(self):
        frames = 128
        signal = twopole_input(frames, 3)
        want = Rig(frames, ag.TWOPOLE, 2, open_=(1,))
        got = Rig(frames, ag.TWOPOLE, 2, open_=(1,))
        got.set(ag.TWOPOLE_TYPE, 9)
        for x in signal:
            self.assertEqual(want.run(x, None), got.run(x, None))

    def test_low_pass_and_high_pass_split_a_signal(self):
        frames = 128
        signal = twopole_input(frames, 30)
        lows, highs = [], []
        for kind, bucket in ((ag.TWOPOLE_CMOS_LOWPASS, lows), (ag.TWOPOLE_CMOS_HIGHPASS, highs)):
            rig = Rig(frames, ag.TWOPOLE, 2, open_=(1,))
            rig.set(ag.TWOPOLE_TYPE, kind)
            rig.set(ag.TWOPOLE_CUTOFF_HZ, 300.0)
            for x in signal:
                bucket += rig.run(x, None)
        # a 220 Hz-ish saw with its harmonics: the high-pass keeps more of the
        # top, the low-pass more of the bottom
        def edge(values):
            return sum(abs(b - a) for a, b in zip(values, values[1:]))
        self.assertGreater(edge(highs), edge(lows))


# --------------------------------------------------------------- OVERDRIVE --

class OverdriveRef:
    """vrack's `OverdriveCore` on audiodsp's own nodes, built and pulled the
    way vrack's `NodeBridge` does: a looping `RawSample` over one block, a
    pass-through `audiofilters.Filter`, `audiofilters.Distortion`, and the
    shelf's `audiofilters.Filter`, one pull a block."""

    def __init__(self, frames):
        self.frames = frames
        self.input = array("h", bytes(2 * frames))
        source = audiocore.RawSample(self.input, channel_count=1, sample_rate=RATE)
        kwargs = dict(buffer_size=frames * 2, sample_rate=RATE, bits_per_sample=16,
                      samples_signed=True, channel_count=1)
        self.dist = audiofilters.Distortion(
            drive=0.5, pre_gain=0.0, post_gain=0.0,
            mode=audiofilters.DistortionMode.OVERDRIVE, soft_clip=True, mix=1.0,
            **kwargs)
        self.shelf = synthio.Biquad(synthio.FilterMode.HIGH_SHELF, 1500.0,
                                    Q=0.7071, A=1.0)
        tone = audiofilters.Filter(filter=self.shelf, **kwargs)
        through = audiofilters.Filter(filter=None, **kwargs)
        through.play(source, loop=True)
        self.dist.play(through)
        tone.play(self.dist)
        self.tail = tone
        audiocore.get_buffer(self.tail)  # the bridge's throwaway first pull

    def render(self, block, drive, tone, level):
        self.dist.drive = max(0.0, min(1.0, drive))
        self.dist.pre_gain = 20.0 * drive
        db = (max(0.0, min(1.0, tone)) - 0.5) * 24.0
        self.shelf.A = 10.0 ** (db / 40.0)
        self.dist.post_gain = 20.0 * math.log10(max(1e-4, level))
        self.input[:] = array("h", block)
        return list(array("h", bytes(audiocore.get_buffer(self.tail)[1])))


class Overdrive(unittest.TestCase):

    def signal(self, frames, blocks, amplitude):
        return [[(((b * frames + i) * 211) % 4096 - 2048) * amplitude // 2048
                 + (amplitude // 4 if ((b * frames + i) // 37) % 2 else 0)
                 for i in range(frames)] for b in range(blocks)]

    def compare(self, settings, frames=128, amplitude=6000):
        rig = Rig(frames, ag.OVERDRIVE, 1)
        ref = OverdriveRef(frames)
        got, want = [], []
        for block, (drive, tone, level) in zip(
                self.signal(frames, len(settings), amplitude), settings):
            rig.set(ag.OVERDRIVE_DRIVE, drive)
            rig.set(ag.OVERDRIVE_TONE, tone)
            rig.set(ag.OVERDRIVE_LEVEL, level)
            got += rig.run(block)
            # The node holds tone as the wire stores it, in float32. The
            # shelf's coefficients carry up to 30 fractional bits, so a tone
            # float32 can't hold exactly moves one, and the output by a count.
            want += ref.render(block, drive, f32(tone), level)
        return got, want

    def test_matches_overdrivecore(self):
        for drive, tone, level in ((0.5, 0.5, 1.0), (0.75, 0.25, 0.5),
                                   (1.0, 1.0, 2.0), (0.0, 0.0, 0.125),
                                   (0.875, 0.75, 1.5)):
            got, want = self.compare([(drive, tone, level)] * 40)
            self.assertGreater(max(abs(v) for v in got), 200)
            self.assertEqual(worst(got, want), 0, (drive, tone, level))

    def test_matches_while_the_knobs_move(self):
        # every block a new drive, tone and level, past the ends of their
        # ranges: the clamps are OverdriveCore's and audiofilters'
        settings = [((b % 9) / 8.0 - 0.0625, (b % 13) / 12.0 + 0.0625,
                     0.0625 * (1 + b % 40)) for b in range(80)]
        got, want = self.compare(settings)
        self.assertEqual(worst(got, want), 0)

    def test_past_the_knee_the_level_is_squeezed(self):
        # a loud, hot signal reaches the mix-down's knee at 28000 and goes
        # no higher than 28521, as the Filter's last step leaves it
        got, want = self.compare([(1.0, 1.0, 2.0)] * 30, amplitude=16000)
        self.assertEqual(worst(got, want), 0)
        self.assertGreater(max(got), 28000)
        self.assertLessEqual(max(got), 28521)

    def test_other_block_sizes(self):
        for frames in (32, 64, 256):
            got, want = self.compare([(0.625, 0.375, 1.0)] * 20, frames=frames)
            self.assertEqual(worst(got, want), 0, frames)

    def test_reset_clears_the_shelf(self):
        rig = Rig(128, ag.OVERDRIVE, 1)
        rig.set(ag.OVERDRIVE_TONE, 1.0)
        blocks = self.signal(128, 4, 6000)
        first = [rig.run(b) for b in blocks]
        rig.g.reset(rig.node)
        again = [rig.run(b) for b in blocks]
        self.assertEqual(first, again)


# -------------------------------------------------------------------- TANH --

def tanh_ref(block, drive, level):
    """`level * UNIT * tanh(drive * x / UNIT)`, in double precision, rounded half to even."""
    return [sat(level * UNIT * math.tanh(drive * v / UNIT)) for v in block]


class Tanh(unittest.TestCase):

    def ramp(self, frames, blocks, amplitude):
        # a triangle from -amplitude to +amplitude over the run, so every
        # level from silence to the clip is met, with a little of a square
        total = frames * blocks
        values = []
        for i in range(total):
            p = i / total
            tri = 4.0 * p - 1.0 if p < 0.5 else 3.0 - 4.0 * p
            values.append(max(-32768, min(32767, int(tri * amplitude)
                                          + (300 if (i // 37) % 2 else -300))))
        return [values[b * frames:(b + 1) * frames] for b in range(blocks)]

    def compare(self, settings, frames=128, amplitude=32767):
        rig = Rig(frames, ag.TANH, 1)
        got, want = [], []
        for block, (drive, level) in zip(self.ramp(frames, len(settings), amplitude),
                                         settings):
            rig.set(ag.TANH_DRIVE, drive)
            rig.set(ag.TANH_LEVEL, level)
            got += rig.run(block)
            want += tanh_ref(block, drive, level)
        return got, want

    def test_matches_tanh(self):
        # vrack's MonoSynth mixer drives 1.2 to 4.8 into it at a level of 0.65
        for drive, level in ((1.0, 1.0), (1.2, 0.65), (4.8, 0.43), (0.25, 1.0),
                             (8.0, 0.5), (1.0, 1.5)):
            got, want = self.compare([(drive, level)] * 40)
            self.assertLessEqual(worst(got, want), 1, (drive, level))

    def test_it_saturates(self):
        got, _ = self.compare([(4.0, 1.0)] * 20)
        self.assertGreater(max(got), 16300)
        self.assertLessEqual(max(got), 16384)
        self.assertGreater(min(got), -16385)
        # at unity gain a small signal passes nearly unchanged
        small = Rig(128, ag.TANH, 1).run([v // 64 for v in range(-4096, 4096, 64)])
        self.assertLessEqual(worst(small, [v // 64 for v in range(-4096, 4096, 64)]), 1)

    def test_defaults_are_unity(self):
        block = [(i * 211) % 4096 - 2048 for i in range(128)]
        self.assertEqual(Rig(128, ag.TANH, 1).run(block), tanh_ref(block, 1.0, 1.0))

    def test_matches_while_the_knobs_move(self):
        settings = [(0.125 * (1 + b % 40), 0.0625 * (1 + b % 33)) for b in range(80)]
        got, want = self.compare(settings)
        self.assertLessEqual(worst(got, want), 1)

    def test_the_level_can_push_past_the_rail(self):
        got, _ = self.compare([(8.0, 4.0)] * 10)
        self.assertEqual(max(got), 32767)
        self.assertEqual(min(got), -32768)

    def test_an_open_input_is_silence(self):
        rig = Rig(64, ag.TANH, 1, open_=(0,))
        self.assertEqual(rig.run(), [0] * 64)

    def test_other_block_sizes(self):
        for frames in (32, 64, 256):
            got, want = self.compare([(1.2, 0.65)] * 20, frames=frames)
            self.assertLessEqual(worst(got, want), 1, frames)

    def test_the_check_catches_a_wrong_tanh(self):
        # planted faults in the reference, as a stand-in for them in the
        # node: each must fail the 1-count bar this file holds the node to
        frames, blocks = 128, 40
        settings = [(1.2, 0.65)] * blocks
        got, _ = self.compare(settings)
        blocks_in = self.ramp(frames, blocks, 32767)
        wrong = {
            "drive and level swapped": lambda b: tanh_ref(b, 0.65, 1.2),
            "a clip, not a tanh": lambda b: [
                sat(0.65 * UNIT * max(-1.0, min(1.0, 1.2 * v / UNIT))) for v in b],
            "the rational tanh in the rack's fast_tanh": lambda b: [
                sat(0.65 * UNIT * self.fast_tanh(1.2 * v / UNIT)) for v in b],
            "drive off by 2%": lambda b: tanh_ref(b, 1.2 * 1.02, 0.65),
            "level off by 1%": lambda b: tanh_ref(b, 1.2, 0.65 * 1.01),
        }
        for name, fault in wrong.items():
            want = []
            for block in blocks_in:
                want += fault(block)
            self.assertGreater(worst(got, want), 1, name)

    @staticmethod
    def fast_tanh(x):
        if x < -3.0:
            return -1.0
        if x > 3.0:
            return 1.0
        x2 = x * x
        return x * (27.0 + x2) / (27.0 + 9.0 * x2)

    def test_reset_is_harmless(self):
        rig = Rig(128, ag.TANH, 1)
        block = [(i * 211) % 4096 - 2048 for i in range(128)]
        first = rig.run(block)
        rig.g.reset(rig.node)
        self.assertEqual(first, rig.run(block))


# ------------------------------------------------------------- VCA curve --

class VcaCurve(unittest.TestCase):

    def run_vca(self, audio, cv, curve, gain=1.0):
        rig = Rig(len(audio), ag.VCA, 3, open_=(2,))
        rig.set(ag.VCA_CURVE, curve)
        rig.set(ag.VCA_GAIN, gain)
        return rig.run(audio, cv)

    def test_linear_is_the_default(self):
        audio = [((i * 211) % 4096 - 2048) * 6 for i in range(128)]
        cv = [(i * 257) % 32768 for i in range(128)]
        rig = Rig(128, ag.VCA, 3, open_=(2,))
        got = rig.run(audio, cv)
        self.assertEqual(got, self.run_vca(audio, cv, ag.VCA_LINEAR))
        self.assertEqual(got, [sat(a * c / UNIT) for a, c in zip(audio, cv)])

    def test_exponential_is_sixty_db_over_the_unit(self):
        audio = [((i * 211) % 4096 - 2048) * 6 for i in range(256)]
        cv = [(i * 131) % 32768 for i in range(256)]
        for gain in (1.0, 1.5, 0.25):
            got = self.run_vca(audio, cv, ag.VCA_EXPONENTIAL, gain)
            # AmplifierCore's exponential response: 10 ** (3 * (cv - 1))
            want = [sat(a * 10.0 ** (3.0 * (c / UNIT - 1.0)) * gain)
                    for a, c in zip(audio, cv)]
            self.assertLessEqual(worst(got, want), 1, gain)
            self.assertGreater(max(abs(v) for v in got), 1000)

    def test_exponential_ends(self):
        audio = [12000, -12000, 30000, -30000] * 8
        # at UNIT, unity exactly; at zero, -60 dB
        self.assertEqual(self.run_vca(audio, [UNIT] * 32, ag.VCA_EXPONENTIAL), audio)
        self.assertEqual(self.run_vca(audio, [0] * 32, ag.VCA_EXPONENTIAL),
                         [sat(a / 1000.0) for a in audio])


# --------------------------------------------------------------------- PAN --

def balance(gain, pan):
    """PAN's two gains for one input, in float32 as the node works them out."""
    gain, pan = f32(gain), f32(max(-1.0, min(1.0, f32(pan))))
    left = f32(gain * f32(1.0 - pan)) if pan > 0.0 else gain
    right = f32(gain * f32(1.0 + pan)) if pan < 0.0 else gain
    return left, right


class PanRig:
    """A PAN node and the two MIX nodes it stands in for, on the same inputs."""

    def __init__(self, frames, inputs, open_=(), group=ag.ALWAYS):
        self.frames = frames
        self.g = ag.Graph(sample_rate=RATE, frames=frames, channel_count=1,
                          buffers=16, nodes=8)
        self.ins = [None if i in open_ else self.g.buffer() for i in range(inputs)]
        self.left, self.right = self.g.buffer(), self.g.buffer()
        self.mix_left, self.mix_right = self.g.buffer(), self.g.buffer()
        self.pan = self.g.add(ag.PAN, group, tuple(self.ins), self.left, self.right)
        self.mixes = (self.g.add(ag.MIX, group, tuple(self.ins), self.mix_left),
                      self.g.add(ag.MIX, group, tuple(self.ins), self.mix_right))
        self._block = array("h", bytes(2 * frames))

    def set(self, i, gain, pan):
        self.g.set(self.pan, i, gain)
        self.g.set(self.pan, ag.PAN_PAN + i, pan)
        left, right = balance(gain, pan)
        self.g.set(self.mixes[0], i, left)
        self.g.set(self.mixes[1], i, right)

    def reset(self):
        for node in (self.pan,) + self.mixes:
            self.g.reset(node)

    def read(self, buffer):
        self.g.read(buffer, self._block)
        return list(self._block)

    def run(self, blocks, enable=None):
        for buffer, values in zip(self.ins, blocks):
            if buffer is not None:
                self.g.write(buffer, array("h", values))
        self.g.run(None, 0, 0xFFFFFFFF if enable is None else enable)
        return ((self.read(self.left), self.read(self.right)),
                (self.read(self.mix_left), self.read(self.mix_right)))


def voices(block, frames, count):
    """`count` voices of a block: saws at different rates and levels."""
    return [[((((block * frames + i) * (97 + 31 * v)) % 4096) - 2048) * (3 + v % 4)
             for i in range(frames)] for v in range(count)]


class Pan(unittest.TestCase):

    def test_matches_two_mixers_with_gains_and_pans_moving(self):
        rig = PanRig(128, 8)
        heard = 0
        for block in range(40):
            for v in range(8):
                gain = (0.0, 0.25, 0.5, 1.0, 0.75)[(block // 4 + v) % 5]
                pan = ((block * 3 + v * 5) % 9 - 4) / 4.0  # -1 .. 1 in quarters
                if block >= 30:
                    pan *= 1.5  # past the ends
                rig.set(v, gain, pan)
            (left, right), (mix_left, mix_right) = rig.run(voices(block, 128, 8))
            self.assertEqual(left, mix_left, block)
            self.assertEqual(right, mix_right, block)
            heard = max(heard, max(abs(x - y) for x, y in zip(left, right)))
        self.assertGreater(heard, 4000)  # the sides differ: the pans did something

    def test_centred_is_a_mixer_on_each_side(self):
        rig = PanRig(64, 3)
        for v, gain in enumerate((1.0, 0.5, 0.125)):
            rig.set(v, gain, 0.0)
        blocks = voices(0, 64, 3)
        (left, right), _ = rig.run(blocks)
        want = [sat(a + 0.5 * b + 0.125 * c) for a, b, c in zip(*blocks)]
        self.assertEqual(left, want)
        self.assertEqual(right, want)

    def test_hard_left_and_hard_right(self):
        rig = PanRig(64, 2)
        rig.set(0, 1.0, -1.0)
        rig.set(1, 1.0, 1.0)
        blocks = voices(0, 64, 2)
        (left, right), _ = rig.run(blocks)
        self.assertEqual(left, blocks[0])
        self.assertEqual(right, blocks[1])

    def test_a_new_pan_ramps_across_the_block(self):
        rig = PanRig(128, 1)
        rig.set(0, 1.0, 0.0)
        rig.run([[8000] * 128])
        rig.set(0, 1.0, 1.0)
        (left, right), _ = rig.run([[8000] * 128])
        self.assertEqual(right, [8000] * 128)
        self.assertEqual(left[-1], 0)
        self.assertTrue(all(a >= b for a, b in zip(left, left[1:])))
        self.assertGreater(left[63], 3000)

    def test_reset_takes_the_gains_as_they_stand(self):
        rig = PanRig(128, 1)
        rig.set(0, 1.0, 0.0)
        rig.run([[8000] * 128])
        rig.set(0, 0.5, -0.5)
        rig.reset()
        (left, right), (mix_left, mix_right) = rig.run([[8000] * 128])
        self.assertEqual(left, [4000] * 128)
        self.assertEqual(right, [2000] * 128)
        self.assertEqual((left, right), (mix_left, mix_right))

    def test_an_open_input_adds_nothing(self):
        rig = PanRig(64, 3, open_=(1,))
        for v in range(3):
            rig.set(v, 1.0, 0.5)
        blocks = voices(0, 64, 3)
        (left, right), (mix_left, mix_right) = rig.run(blocks)
        self.assertEqual((left, right), (mix_left, mix_right))
        self.assertEqual(right, [sat(a + c) for a, c in zip(blocks[0], blocks[2])])

    def test_a_skipped_group_silences_both_sides(self):
        rig = PanRig(64, 1, group=3)
        rig.set(0, 1.0, 0.25)
        (left, right), _ = rig.run([[8000] * 64], 1 << 3)
        self.assertEqual((left[0], right[0]), (6000, 8000))
        (left, right), _ = rig.run([[8000] * 64], 0)
        self.assertEqual(left, [0] * 64)
        self.assertEqual(right, [0] * 64)

    def test_the_right_side_must_be_a_buffer_of_its_own(self):
        g = ag.Graph(sample_rate=RATE, frames=64, channel_count=1, buffers=4, nodes=4)
        a, b = g.buffer(), g.buffer()
        for right in (a, 4.0, -1.0, 1.5):
            with self.assertRaises(ValueError, msg=right):
                g.add(ag.PAN, ag.ALWAYS, (b,), a, right)
        g.add(ag.PAN, ag.ALWAYS, (b,), a, 2)

    def test_defaults_are_unity_and_centred(self):
        g = ag.Graph(sample_rate=RATE, frames=64, channel_count=1, buffers=4, nodes=4)
        node = g.add(ag.PAN, ag.ALWAYS, (g.buffer(),), g.buffer(), 2)
        for i in range(8):
            self.assertEqual(g.get(node, i), 1.0)
            self.assertEqual(g.get(node, ag.PAN_PAN + i), 0.0)


if __name__ == "__main__":
    unittest.main()


# ---------------------------------------------------------------- WAVEFOLD --

HALF_PI32 = f32(0.5 * math.pi)


def sin32(value):
    return f32(math.sin(f32(value)))


def wavefold_ref(src, fold, symmetry, cv=None):
    """vrack's `WavefolderCore.render` on one block, operation for operation in
    float32, `src` and `cv` in counts. The rack's output, on the wire."""
    if fold <= 0.001 and cv is None:
        return list(src)
    out = []
    offset = f32(symmetry * 0.25)
    for i, count in enumerate(src):
        s = count / UNIT
        f = f32(fold)
        if cv is not None:
            f = a_(f, cv[i] / UNIT)
        x = a_(s, offset)
        x = m(x, a_(m(f, 7.0), 1.0))
        f1 = sin32(m(x, HALF_PI32))
        f2 = sin32(m(a_(m(f1, 1.5), m(x, 0.5)), HALF_PI32))
        f3 = m(sin32(m(a_(m(f2, 1.3), m(f1, 0.4)), HALF_PI32)), 0.5)
        out.append(sat(a_(m(s, a_(-f, 1.0)), m(f3, f)) * UNIT))
    return out


def tones(frames, blocks, amplitude=8192):
    """A sine and its fifth, the principal oscillator's range and then some."""
    return [[round(amplitude * (0.75 * math.sin(2 * math.pi * 220.0 * (b * frames + i) / RATE)
                                + 0.25 * math.sin(2 * math.pi * 330.0 * (b * frames + i) / RATE)))
             for i in range(frames)] for b in range(blocks)]


class Wavefold(unittest.TestCase):

    def compare(self, settings, cv=None, frames=128, amplitude=8192, open_=()):
        # with no CV the CV input is open, as the rack's fold_cv is None
        rig = Rig(frames, ag.WAVEFOLD, 2, open_=tuple(open_) + ((1,) if cv is None else ()))
        got, want = [], []
        blocks = tones(frames, len(settings), amplitude)
        for b, (block, (fold, symmetry)) in enumerate(zip(blocks, settings)):
            rig.set(ag.WAVEFOLD_FOLD, fold)
            rig.set(ag.WAVEFOLD_SYMMETRY, symmetry)
            cv_block = cv[b] if cv is not None else None
            src = [0] * frames if 0 in open_ else block
            got += rig.run(None if 0 in open_ else block, cv_block)
            want += wavefold_ref(src, fold, symmetry, cv_block)
        return got, want

    def test_matches_wavefoldercore(self):
        for fold, symmetry in ((0.2, 0.0), (0.5, 0.25), (1.0, -1.0), (0.75, 1.0),
                               (0.0625, -0.375), (0.35, 0.6)):
            got, want = self.compare([(fold, symmetry)] * 20)
            self.assertLessEqual(worst(got, want), 1, (fold, symmetry))
            self.assertGreater(sum(1 for g, w in zip(got, want) if g == w),
                               0.99 * len(got), (fold, symmetry))

    def test_folds(self):
        # a deep fold puts many more turns in a block than the input has
        def turns(block):
            return sum(1 for a, b, c in zip(block, block[1:], block[2:])
                       if (b - a) * (c - b) < 0)
        clean, _ = self.compare([(0.002, 0.0)] * 4)
        folded, _ = self.compare([(1.0, 0.0)] * 4)
        self.assertGreater(turns(folded), 4 * turns(clean))

    def test_the_fold_cv_adds_to_the_fold(self):
        frames, blocks = 128, 24
        cv = [[((b * frames + i) * 13) % 8192 - 2048 for i in range(frames)]
              for b in range(blocks)]
        got, want = self.compare([(0.25, 0.125)] * blocks, cv=cv)
        self.assertLessEqual(worst(got, want), 1)
        # and a CV with the knob at 0 still folds
        got, want = self.compare([(0.0, 0.0)] * blocks, cv=cv)
        self.assertLessEqual(worst(got, want), 1)
        self.assertNotEqual(got, [v for b in tones(frames, blocks) for v in b])

    def test_the_knobs_moving_and_a_hot_input(self):
        settings = [((b % 11) / 10.0, ((b % 7) - 3) / 3.0) for b in range(60)]
        got, want = self.compare(settings, amplitude=16000)
        self.assertLessEqual(worst(got, want), 1)

    def test_no_fold_passes_the_input(self):
        got, want = self.compare([(0.0, 0.5), (0.001, -1.0)] * 3)
        self.assertEqual(got, [v for b in tones(128, 6) for v in b])
        self.assertEqual(got, want)

    def test_an_open_input_is_silence_folded(self):
        # the symmetry offsets silence, so a fold of it is a constant
        got, want = self.compare([(0.5, 1.0)] * 3, open_=(0,))
        self.assertLessEqual(worst(got, want), 1)
        self.assertEqual(len(set(got)), 1)
        self.assertNotEqual(got[0], 0)
        got, _ = self.compare([(0.0, 1.0)] * 3, open_=(0,))
        self.assertEqual(set(got), {0})

    def test_other_block_sizes(self):
        for frames in (32, 64, 256):
            got, want = self.compare([(0.4, -0.25)] * 10, frames=frames)
            self.assertLessEqual(worst(got, want), 1, frames)


# --------------------------------------------------------------------- LPG --

class LpgRef:
    """vrack's `LowPassGateCore`: the vactrol in double with libm's exp and
    pow, and its filter a `synthio.Biquad` low-pass inside
    `audiofilters.Filter`, built and pulled the way vrack's `NodeBridge` and
    `BiquadFilter` do. The gains are float32 as numpy applies them."""

    def __init__(self, frames):
        self.frames = frames
        self.vactrol = 0.0
        self.cutoff = 1000.0
        self.input = array("h", bytes(2 * frames))
        source = audiocore.RawSample(self.input, channel_count=1, sample_rate=RATE)
        kwargs = dict(buffer_size=frames * 2, sample_rate=RATE, bits_per_sample=16,
                      samples_signed=True, channel_count=1)
        self.biquad = synthio.Biquad(synthio.FilterMode.LOW_PASS, 1000.0, Q=0.7071)
        lpf = audiofilters.Filter(filter=self.biquad, **kwargs)
        through = audiofilters.Filter(filter=None, **kwargs)
        through.play(source, loop=True)
        lpf.play(through)
        self.tail = lpf
        audiocore.get_buffer(self.tail)  # the bridge's throwaway first pull

    def render(self, block, control=None, strike=None, manual=0.0, decay=0.25,
               mode=2, level=1.0):
        n, sr = self.frames, float(RATE)
        target = manual
        if control is not None:
            target += control[0] / UNIT
        if strike is not None:
            target += 1.5 * (strike[0] / UNIT)
        target = max(0.0, min(1.0, target))
        vs = self.vactrol
        if target > vs:
            vs += (target - vs) * (1.0 - math.exp(-n / (0.002 * sr)))
        else:
            vs *= math.exp(-n / (max(0.01, decay) * sr))
        self.vactrol = vs
        v = max(0.0, min(1.0, vs))
        if mode == 1:
            out = [f32(x * f32(v)) for x in block]
        else:
            cutoff = max(10.0, min(sr * 0.45, 30.0 * (400.0 ** v)))
            if cutoff != self.cutoff:
                self.cutoff = cutoff
                self.biquad.frequency = cutoff
            self.input[:] = array("h", block)
            out = list(array("h", bytes(audiocore.get_buffer(self.tail)[1])))
            if mode == 2:
                g = f32(v ** 1.3)
                out = [f32(y * g) for y in out]
        return [sat(f32(y * f32(level))) for y in out]


def lpg_strikes(frames, blocks, every=24, width=2):
    """A trigger: full scale for `width` blocks out of each `every`."""
    return [[UNIT if b % every < width else 0] * frames for b in range(blocks)]


def lpg_ramp(frames, blocks, period=40):
    """A control rising 0..1 over `period` blocks and falling back."""
    out = []
    for b in range(blocks):
        p = b % period
        level = p / (period / 2) if p < period // 2 else 2 - p / (period / 2)
        out.append([int(level * UNIT)] * frames)
    return out


class Lpg(unittest.TestCase):

    def compare(self, blocks, frames=128, control=None, strike=None, settings=None,
                amplitude=12000, open_=()):
        rig = Rig(frames, ag.LPG, 3, open_=tuple(open_)
                  + ((1,) if control is None else ()) + ((2,) if strike is None else ()))
        ref = LpgRef(frames)
        settings = settings or [(0.0, 0.25, 2, 1.0)] * blocks
        got, want = [], []
        for b, block in enumerate(tones(frames, blocks, amplitude)):
            manual, decay, mode, level = settings[b]
            rig.set(ag.LPG_MANUAL, manual)
            rig.set(ag.LPG_DECAY, decay)
            rig.set(ag.LPG_MODE, mode)
            rig.set(ag.LPG_LEVEL, level)
            c = control[b] if control is not None else None
            s = strike[b] if strike is not None else None
            src = [0] * frames if 0 in open_ else block
            got += rig.run(None if 0 in open_ else block, c, s)
            want += ref.render(src, c, s, manual, decay, mode, level)
        return got, want

    def test_matches_lowpassgatecore(self):
        frames, blocks = 128, 96
        for mode in (0, 1, 2):
            for decay in (0.25, 0.0625, 1.5):
                got, want = self.compare(
                    blocks, strike=lpg_strikes(frames, blocks),
                    settings=[(0.0, decay, mode, 1.0)] * blocks)
                self.assertLessEqual(worst(got, want), 1, (mode, decay))
                self.assertGreater(sum(1 for g, w in zip(got, want) if g == w),
                                   0.999 * len(got), (mode, decay))

    def test_the_control_and_manual(self):
        frames, blocks = 128, 120
        for mode in (0, 2):
            got, want = self.compare(blocks, control=lpg_ramp(frames, blocks),
                                     settings=[(0.125, 0.375, mode, 1.0)] * blocks)
            self.assertLessEqual(worst(got, want), 1, mode)
        # the manual level alone opens the gate
        got, want = self.compare(40, settings=[(0.75, 0.25, 0, 1.0)] * 40)
        self.assertLessEqual(worst(got, want), 1)
        self.assertGreater(max(got[-1280:]), 4000)

    def test_a_struck_gate_rings_and_dies(self):
        frames, blocks = 128, 120
        got, want = self.compare(blocks, strike=lpg_strikes(frames, blocks, every=1000),
                                 settings=[(0.0, 0.0625, 2, 1.0)] * blocks)
        self.assertLessEqual(worst(got, want), 1)
        energy = [sum(abs(v) for v in got[b * frames:(b + 1) * frames])
                  for b in range(blocks)]
        self.assertGreater(energy[3], 100 * max(1, energy[-1]))

    def test_the_knobs_moving_a_hot_input_and_the_level(self):
        frames, blocks = 128, 90
        settings = [((b % 5) / 8.0, (1, 0.125, 0.5)[b % 3], (b // 7) % 3,
                     (1.0, 0.5, 1.5)[(b // 11) % 3]) for b in range(blocks)]
        got, want = self.compare(blocks, control=lpg_ramp(frames, blocks, 30),
                                 strike=lpg_strikes(frames, blocks, 17),
                                 settings=settings, amplitude=30000)
        self.assertLessEqual(worst(got, want), 1)

    def test_closed_is_silence(self):
        got, want = self.compare(10, settings=[(0.0, 0.25, 2, 1.0)] * 10)
        self.assertEqual(set(got), {0})
        self.assertEqual(got, want)

    def test_an_open_input(self):
        got, want = self.compare(20, strike=lpg_strikes(128, 20),
                                 settings=[(0.0, 0.25, 0, 1.0)] * 20, open_=(0,))
        self.assertEqual(set(got), {0})
        self.assertEqual(got, want)

    def test_other_block_sizes(self):
        for frames in (32, 64, 256):
            blocks = 40
            got, want = self.compare(blocks, frames=frames,
                                     strike=lpg_strikes(frames, blocks, 12),
                                     settings=[(0.0, 0.125, 2, 1.0)] * blocks)
            self.assertLessEqual(worst(got, want), 1, frames)


# ------------------------------------------------------------------- MODAL --

import audiomodal  # noqa: E402

MODAL_BAR = (1.0, 2.0, 3.0, 4.2, 7.9, 9.8, 11.9, 14.2)
MODAL_MEMBRANE = (1.0, 1.59, 2.14, 2.30, 2.65, 2.92, 3.16, 3.50)


class ModalRef:
    """vrack's `ModalResonatorCore`: its `Exciter` transcribed in float32 as
    numpy runs it, the sum rounded to int16 by the bridge's cast, and an
    `audiomodal.Bank` pulled 256 frames at a time behind a passthrough
    `audiofilters.Filter`, as vrack's `ChunkedBridge` pulls it. The rack hears
    each chunk 256 frames late; this lines the chunk up with its input."""

    def __init__(self, frames):
        self.frames = frames
        self.high = False
        self.level = 0.0
        self.pending = 0
        self.bank = audiomodal.Bank(sample_rate=RATE, modes=8, channel_count=1,
                                    mix=1.0, gain=1.0)
        self.input = array("h", bytes(512))
        source = audiocore.RawSample(self.input, channel_count=1, sample_rate=RATE)
        through = audiofilters.Filter(filter=None, buffer_size=512, sample_rate=RATE,
                                      bits_per_sample=16, samples_signed=True,
                                      channel_count=1)
        through.play(source, loop=True)
        self.bank.play(through)
        audiocore.get_buffer(self.bank)  # the bridge's throwaway first pull
        self.pend = array("h")
        self.out = []
        self.log = f32(math.log(0.85))

    def tune(self, freq, ratios, decay, brightness):
        top = RATE * 0.45
        for m in range(8):
            f = min(top, freq * ratios[m])
            gain = (1.0 / (m + 1)) * ((0.2 + 0.8 * brightness) ** m)
            self.bank.set_mode(m, f, decay * (1.0 - 0.5 * m / 8), gain)

    def _burst(self, dst, start, end):
        if self.pending <= 0 or end <= start:
            return
        k = min(self.pending, end - start)
        level = f32(self.level)
        for j in range(k):
            b = f32(f32(math.exp(f32((j + 1) * self.log))) * level)
            dst[start + j] = f32(dst[start + j] + b)
        self.level = f32(f32(math.exp(f32(k * self.log))) * level)
        self.pending -= k
        if self.pending <= 0:
            self.level = 0.0

    def render(self, audio, strike, level, mix=1.0):
        n = self.frames
        dst = [f32(v / UNIT) for v in audio] if audio is not None else [0.0] * n
        start = 0
        if strike is not None:
            edge = -1
            high = self.high
            for i, v in enumerate(strike):
                now = v / UNIT > 0.1
                if now and not high:
                    edge = i
                    break
                high = now
            self.high = strike[-1] / UNIT > 0.1
            if edge >= 0:
                self._burst(dst, 0, edge)
                self.level, self.pending = 1.0, 40
                start = edge
        self._burst(dst, start, n)
        lv = f32(level)
        for v in dst:
            x = f32(v * lv) * UNIT
            x = max(-32768.0, min(32767.0, x))
            self.pend.append(int(x + 0.5) if x >= 0 else -int(-x + 0.5))
        self.bank.set(mix=mix)
        while len(self.pend) >= 256:
            self.input[:] = self.pend[:256]
            del self.pend[:256]
            self.out += list(array("h", bytes(audiocore.get_buffer(self.bank)[1])))


def modal_strikes(frames, blocks, every=40, width=3):
    """A trigger: full scale for `width` blocks out of each `every`, from block 1."""
    return [[UNIT if (b - 1) % every < width and b >= 1 else 0] * frames
            for b in range(blocks)]


def modal_noise(frames, blocks, amplitude=6000, seed=7):
    """A quiet, deterministic noise excitation."""
    out, r = [], seed
    for _ in range(blocks):
        block = []
        for _ in range(frames):
            r = (r * 1103515245 + 12345) & 0x7FFFFFFF
            block.append((r >> 8) % (2 * amplitude + 1) - amplitude)
        out.append(block)
    return out


class Modal(unittest.TestCase):

    def compare(self, blocks, frames=128, audio=None, strike=None, settings=None):
        """`settings` per block: (freq, decay, brightness, input, mix, ratios)."""
        rig = Rig(frames, ag.MODAL, 2, open_=((0,) if audio is None else ())
                  + ((1,) if strike is None else ()))
        ref = ModalRef(frames)
        got = []
        last = None
        for b in range(blocks):
            freq, decay, bright, level, mix, ratios = settings[b]
            if (freq, decay, bright, ratios) != last:
                last = (freq, decay, bright, ratios)
                ref.tune(freq, ratios, decay, bright)
            rig.set(ag.MODAL_FREQUENCY, freq)
            rig.set(ag.MODAL_DECAY, decay)
            rig.set(ag.MODAL_BRIGHTNESS, bright)
            rig.set(ag.MODAL_INPUT, level)
            rig.set(ag.MODAL_MIX, mix)
            for m, r in enumerate(ratios):
                rig.set(ag.MODAL_RATIO + m, r)
            a = audio[b] if audio is not None else None
            s = strike[b] if strike is not None else None
            got += rig.run(a, s)
            ref.render(a, s, level, mix)
        return got, ref.out

    def test_matches_modalresonatorcore(self):
        frames, blocks = 128, 160
        for ratios, freq, decay, bright in (
                (MODAL_BAR, 261.63, 1.6, 0.6),
                (MODAL_MEMBRANE, 110.0, 0.5, 0.2),
                (MODAL_BAR, 880.0, 3.0, 1.0),
                (MODAL_BAR, 3000.0, 0.2, 0.9)):  # the top modes held under 0.45 * rate
            got, want = self.compare(blocks, strike=modal_strikes(frames, blocks),
                                     settings=[(freq, decay, bright, 0.05, 1.0, ratios)]
                                     * blocks)
            self.assertEqual(len(got), len(want))
            self.assertLessEqual(worst(got, want), 1, (ratios, freq))
            self.assertGreater(max(abs(v) for v in got), 2000, (ratios, freq))

    def test_audio_excites_it(self):
        frames, blocks = 128, 80
        got, want = self.compare(blocks, audio=modal_noise(frames, blocks),
                                 strike=modal_strikes(frames, blocks, 30),
                                 settings=[(440.0, 0.8, 0.5, 0.05, 1.0, MODAL_BAR)]
                                 * blocks)
        self.assertLessEqual(worst(got, want), 1)

    def test_mix_zero_is_the_excitation(self):
        frames, blocks = 128, 40
        got, want = self.compare(blocks, audio=modal_noise(frames, blocks),
                                 strike=modal_strikes(frames, blocks, 10),
                                 settings=[(440.0, 0.8, 0.5, 0.15, 0.0, (0.0,) * 8)]
                                 * blocks)
        self.assertEqual(got, want)
        self.assertNotEqual(set(got), {0})

    def test_the_knobs_moving(self):
        # at 256 frames the bank's chunk is the node's block, so a change
        # lands on the same sample in both
        frames, blocks = 256, 120
        settings = [(220.0 * (1 + (b // 30)), (1.2, 0.4)[(b // 20) % 2],
                     (0.1, 0.9)[(b // 25) % 2], 0.05, 1.0,
                     (MODAL_BAR, MODAL_MEMBRANE)[(b // 40) % 2]) for b in range(blocks)]
        got, want = self.compare(blocks, frames=frames,
                                 strike=modal_strikes(frames, blocks, 15), settings=settings)
        self.assertLessEqual(worst(got, want), 1)

    def test_a_ratio_of_zero_turns_its_mode_off(self):
        frames, blocks = 256, 30
        ratios = (1.0,) + (0.0,) * 7
        got, want = self.compare(blocks, frames=frames, strike=modal_strikes(frames, blocks),
                                 settings=[(440.0, 0.5, 1.0, 0.05, 1.0, ratios)] * blocks)
        self.assertLessEqual(worst(got, want), 1)

    def test_unstruck_is_silence_and_a_reset_stops_it(self):
        frames, blocks = 128, 30
        got, want = self.compare(blocks, settings=[(440.0, 2.0, 0.6, 0.05, 1.0, MODAL_BAR)]
                                 * blocks)
        self.assertEqual(set(got), {0})
        rig = Rig(frames, ag.MODAL, 2, open_=(0,))
        rig.run(None, [UNIT] * frames)
        self.assertNotEqual(set(rig.run(None, [UNIT] * frames)), {0})
        rig.g.reset(rig.node)
        self.assertEqual(set(rig.run(None, [0] * frames)), {0})

    def test_other_block_sizes(self):
        for frames in (32, 64, 256):
            blocks = 256 * 40 // frames
            got, want = self.compare(blocks, frames=frames,
                                     strike=modal_strikes(frames, blocks, 2048 // frames),
                                     settings=[(330.0, 1.0, 0.6, 0.05, 1.0, MODAL_BAR)]
                                     * blocks)
            self.assertLessEqual(worst(got, want), 1, frames)
