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
| `ACID` | vrack's `dsp/ladder.py` `AcidFilterCore`: four one-pole stages, high-passed feedback and the accent capacitor, with the cutoff, resonance and env_mod moving and accent bursts | 1 count (the graph's `tanh` polynomial against libm's) |
| `SPRING` | vrack's `dsp/reverb.py` `SpringReverbCore` over `audiofreeverb.Freeverb` at mix 1.0, dry and wet mixed as the rack mixes them: two tanks, decay and decay + 0.05 | 0 counts |
| `ADSR2` | vrack's `dsp/envelope.py` `EnvelopeCore` contour remapped to start at `initial` and peak at `attack_level`, as poly80's filter envelope remaps it | 0 counts |
| `GLISS` | `GlideCore`'s linear glide, then poly80's glissando: once a block the last glided sample is rounded to the step grid and written across the block | 0 counts |
| `FOLLOW` | clavichord's auto-wah peak follower and vocoder330's band RMS follower: once a block the level is followed by a one-pole and written across the block | 0 counts |

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
import audiofreeverb
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


# ------------------------------------------------------------------ STRUCK --

STRUCK_TINE_C4 = (
    (1.0, 1.6, 1.0),
    (2.0, 0.55, 0.14),
    (4.0, 0.09, 0.20),
    (6.27, 0.07, 0.30),
    (10.7, 0.025, 0.12),
    (15.8, 0.012, 0.05),
)

STRUCK_REED_C4 = (
    (1.0, 1.4, 1.0),
    (2.756, 0.50, 0.32),
    (5.404, 0.18, 0.22),
    (8.933, 0.07, 0.10),
    (13.34, 0.030, 0.045),
    (18.64, 0.014, 0.020),
)

STRUCK_CLAVI_C4 = (
    (1.0, 0.90, 1.00),
    (2.001, 0.70, 0.72),
    (3.003, 0.52, 0.52),
    (4.006, 0.40, 0.40),
    (5.011, 0.30, 0.30),
    (6.018, 0.23, 0.23),
    (7.026, 0.17, 0.17),
    (8.036, 0.13, 0.13),
)


class StruckRef:
    """vrack's `HammerExciter` and `StruckBarCore` (`dsp/keys.py`): the hammer
    or pluck pulse rendered in float32 as numpy runs it, cast to int16 by the
    bridge's rounding, and processed by a 24-mode `audiomodal.Bank` (`mix=1.0`,
    `gain=0.0625`)."""

    def __init__(self, frames, table=STRUCK_TINE_C4):
        self.frames = frames
        self.high = False
        self.pulse_len = 0
        self.pulse_pos = 0
        self.pulse_amp = 0.0
        self.pulse_active = False
        self.pulse_shape = ag.STRUCK_HAMMER
        self.bank = audiomodal.Bank(sample_rate=RATE, modes=24, channel_count=1,
                                    mix=1.0, gain=0.0625)
        self.table = tuple((f32(r), f32(d), f32(g)) for r, d, g in table)
        self.tuned_f0 = None

    def set_table(self, table):
        self.table = tuple((f32(r), f32(d), f32(g)) for r, d, g in table)
        self.tuned_f0 = None

    def tune(self, f0):
        f0 = max(20.0, f32(f0))
        nyq = RATE * 0.45
        used = min(len(self.table), 24)
        for i in range(used):
            ratio, decay, gain = self.table[i]
            freq = f0 * ratio
            if freq >= nyq:
                freq = nyq
                gain = 0.0
            self.bank.set_mode(i, freq, max(0.001, decay), gain)
        for i in range(used, 24):
            self.bank.set_mode(i, f0, 0.001, 0.0)
        self.bank._state.finish()

    def _start_pulse(self, hardness, level, f0, shape):
        h = min(1.0, max(0.0, float(hardness)))
        scale = RATE / 48000.0
        soft = 128.0 * scale
        hard = 16.0 * scale
        length = soft * (2.0 ** (-3.0 * h))
        amp = float(level) * (hard / length)
        if f0 > 0.0:
            limit = 0.5 * (RATE / max(1.0, float(f0)))
            if length > limit:
                amp *= length / limit
                length = limit
        n = max(4, round(length))
        self.pulse_len = n
        self.pulse_pos = 0
        self.pulse_amp = f32(amp)
        self.pulse_active = True
        self.pulse_shape = shape

    def _render_pulse(self, dst, start, end):
        if not self.pulse_active or start >= end:
            return
        rem = self.pulse_len - self.pulse_pos
        span = end - start
        k = rem if rem < span else span
        if k <= 0:
            self.pulse_active = False
            return
        pos = self.pulse_pos
        n = self.pulse_len
        amp = self.pulse_amp
        if self.pulse_shape == ag.STRUCK_PLUCK:
            step = f32(1.0 / n)
            for j in range(k):
                t = f32(pos + j)
                t = f32(t * step)
                t = f32(t * amp)
                dst[start + j] = f32(dst[start + j] + t)
        else:
            step = f32(math.pi / n)
            for j in range(k):
                t = f32(pos + j)
                t = f32(t * step)
                t = f32(math.sin(t))
                t = f32(t * amp)
                dst[start + j] = f32(dst[start + j] + t)
        self.pulse_pos += k
        if self.pulse_pos >= self.pulse_len:
            self.pulse_active = False

    def render(self, strike, vel, f0, hardness, level, shape):
        f0 = f32(f0)
        hardness = f32(hardness)
        level = f32(level)
        eff_f0 = max(20.0, f0)
        if self.tuned_f0 != eff_f0:
            self.tuned_f0 = eff_f0
            self.tune(eff_f0)

        n = self.frames
        dst = [0.0] * n
        rising = -1
        if strike is not None:
            high = self.high
            for i in range(n):
                now = strike[i] / UNIT > 0.1
                if now and not high and rising < 0:
                    rising = i
                high = now
            self.high = high

        def vel_at(idx):
            if vel is None or idx < 0:
                return 1.0
            return min(1.0, max(0.0, f32(vel[idx] / UNIT)))

        if rising >= 0:
            self.bank.clear()
            self.pulse_active = False
            lv = f32(level * vel_at(rising))
            if lv > 0.0:
                self._start_pulse(hardness, lv, f0, shape)
            self._render_pulse(dst, rising, n)
        else:
            self._render_pulse(dst, 0, n)

        exc = array("h", (
            int(max(-32768.0, min(32767.0, v * UNIT)) + 0.5)
            if v >= 0.0 else
            -int(max(-32768.0, min(32767.0, -v * UNIT)) + 0.5)
            for v in dst
        ))
        self.bank._state.finish()
        return list(array("h", self.bank._state.process(bytes(exc))))


class Struck(unittest.TestCase):

    def compare(self, blocks, frames=128, strike=None, vel=None,
                settings=None, table=None):
        """`settings` per block: (f0, hardness, level, shape)."""
        rig = Rig(frames, ag.STRUCK, 2,
                  open_=((0,) if strike is None else ())
                  + ((1,) if vel is None else ()))
        ref = StruckRef(frames, table=table or STRUCK_TINE_C4)
        if table is not None:
            rig.g.table(rig.node, table)
        got, want = [], []
        for b in range(blocks):
            f0, hardness, level, shape = settings[b]
            rig.set(ag.STRUCK_FREQUENCY, f0)
            rig.set(ag.STRUCK_HARDNESS, hardness)
            rig.set(ag.STRUCK_LEVEL, level)
            rig.set(ag.STRUCK_SHAPE, shape)
            s = strike[b] if strike is not None else None
            v = vel[b] if vel is not None else None
            got += rig.run(s, v)
            want += ref.render(s, v, f0, hardness, level, shape)
        return got, want

    def test_matches_hammerexciter_and_struckbarcore(self):
        frames, blocks = 128, 80
        strikes = modal_strikes(frames, blocks, every=25, width=6)
        for table, f0, hardness, level, shape in (
            (None, 261.6256, 0.5, 0.8, ag.STRUCK_HAMMER),
            (STRUCK_REED_C4, 220.0, 0.75, 0.9, ag.STRUCK_HAMMER),
            (STRUCK_CLAVI_C4, 330.0, 0.65, 0.85, ag.STRUCK_PLUCK),
            (STRUCK_TINE_C4, 1760.0, 0.2, 0.7, ag.STRUCK_HAMMER),
        ):
            got, want = self.compare(
                blocks, frames=frames, strike=strikes, table=table,
                settings=[(f0, hardness, level, shape)] * blocks,
            )
            self.assertLessEqual(worst(got, want), 1, (f0, hardness, shape))
            self.assertGreater(max(abs(v) for v in got), 800, (f0, hardness))

    def test_velocity_and_mid_block_strike(self):
        frames, blocks = 128, 30
        strike = [[0] * frames for _ in range(blocks)]
        # Block 1: rising at sample 12, falling at sample 96 in the same block.
        for i in range(12, 96):
            strike[1][i] = UNIT
        # Block 10: rising at sample 20, held until block 12 sample 40.
        for i in range(20, frames):
            strike[10][i] = UNIT
        strike[11] = [UNIT] * frames
        for i in range(0, 40):
            strike[12][i] = UNIT
        vel = [[int(UNIT * (0.35 + 0.5 * ((b + i) % 5) / 4.0))
                for i in range(frames)] for b in range(blocks)]
        settings = [(293.66, 0.55, 0.9, ag.STRUCK_HAMMER)] * blocks
        got, want = self.compare(blocks, frames=frames, strike=strike, vel=vel,
                                 settings=settings, table=STRUCK_CLAVI_C4)
        self.assertLessEqual(worst(got, want), 1)
        self.assertGreater(max(abs(v) for v in got), 500)

    def test_table_accepts_float_buffer_and_sequences_and_carries(self):
        frames = 128
        flat = array("f", [r for mode in STRUCK_REED_C4 for r in mode])
        g1 = ag.Graph(sample_rate=RATE, frames=frames, channel_count=1,
                      buffers=8, nodes=8)
        sb1, out1 = g1.buffer(), g1.buffer()
        n1 = g1.add(ag.STRUCK, ag.ALWAYS, (sb1,), out1)
        g1.table(n1, flat)

        g2 = ag.Graph(sample_rate=RATE, frames=frames, channel_count=1,
                      buffers=8, nodes=8)
        sb2, out2 = g2.buffer(), g2.buffer()
        n2 = g2.add(ag.STRUCK, ag.ALWAYS, (sb2,), out2)
        g2.table(n2, STRUCK_REED_C4)

        trig = array("h", [UNIT] * frames)
        zero = array("h", [0] * frames)
        b1 = array("h", bytes(2 * frames))
        b2 = array("h", bytes(2 * frames))
        g1.write(sb1, trig)
        g2.write(sb2, trig)
        g1.run(None)
        g2.run(None)
        g1.read(out1, b1)
        g2.read(out2, b2)
        self.assertEqual(list(b1), list(b2))

        # Carry into a fresh graph with no table of its own: keeps reed modes.
        g3 = ag.Graph(sample_rate=RATE, frames=frames, channel_count=1,
                      buffers=8, nodes=8)
        sb3, out3 = g3.buffer(), g3.buffer()
        n3 = g3.add(ag.STRUCK, ag.ALWAYS, (sb3,), out3)
        g3.carry(n3, g1, n1)
        g1.write(sb1, zero)
        g3.write(sb3, zero)
        g1.run(None)
        g3.run(None)
        g1.read(out1, b1)
        g3.read(out3, b2)
        self.assertEqual(list(b1), list(b2))

    def test_unstruck_is_silence_and_reset_clears_state(self):
        frames, blocks = 128, 20
        got, _ = self.compare(
            blocks, frames=frames,
            settings=[(440.0, 0.5, 0.8, ag.STRUCK_HAMMER)] * blocks,
        )
        self.assertEqual(set(got), {0})
        rig = Rig(frames, ag.STRUCK, 1)
        self.assertNotEqual(set(rig.run([UNIT] * frames)), {0})
        rig.g.reset(rig.node)
        self.assertEqual(set(rig.run([0] * frames)), {0})

    def test_nan_hardness_does_not_crash_or_produce_nan(self):
        rig = Rig(128, ag.STRUCK, 1)
        rig.set(ag.STRUCK_HARDNESS, float("nan"))
        out = rig.run([UNIT] * 128)
        self.assertEqual(len(out), 128)
        self.assertTrue(all(isinstance(v, int) for v in out))

    def test_the_check_catches_a_wrong_struck(self):
        frames, blocks = 128, 40
        strikes = modal_strikes(frames, blocks, every=15, width=4)
        settings = [(330.0, 0.6, 0.85, ag.STRUCK_HAMMER)] * blocks
        got, _ = self.compare(blocks, frames=frames, strike=strikes,
                              settings=settings, table=STRUCK_CLAVI_C4)
        wrong = {
            "pluck instead of hammer": [
                (330.0, 0.6, 0.85, ag.STRUCK_PLUCK)] * blocks,
            "level off": [
                (330.0, 0.6, 0.35, ag.STRUCK_HAMMER)] * blocks,
            "hardness off": [
                (330.0, 0.2, 0.85, ag.STRUCK_HAMMER)] * blocks,
            "frequency off by 2%": [
                (330.0 * 1.02, 0.6, 0.85, ag.STRUCK_HAMMER)] * blocks,
        }
        for name, bad_settings in wrong.items():
            ref = StruckRef(frames, table=STRUCK_CLAVI_C4)
            want = []
            for b in range(blocks):
                want += ref.render(strikes[b], None, *bad_settings[b])
            self.assertGreater(worst(got, want), 10, name)


# ------------------------------------------------------------------ PICKUP --

def pickup_ref(block, mode=0, drive=1.0, offset=0.0, level=1.0):
    """vrack's `PickupCore.render` (`dsp/keys.py`), transcribed operation for
    operation in float32."""
    electrostatic = f32(mode) >= 0.5
    drive = f32(drive)
    offset = f32(offset)
    level = f32(level)
    inv = f32(1.0 / UNIT)
    dc = 0.0
    if offset != 0.0:
        o = float(offset)
        if electrostatic:
            o2 = o * o
            dc = f32(o / (o2 * o2 - o2 + 1.0))
        else:
            dc = f32(o / (1.0 + 0.5 * o * o))
    out = []
    for sample in block:
        dst = f32(sample * inv) if sample is not None else 0.0
        if drive != 1.0:
            dst = f32(dst * drive)
        if offset != 0.0:
            dst = f32(dst + offset)
        tmp = f32(dst * dst)
        if electrostatic:
            tmp = f32(tmp - 1.0)
            tmp = f32(tmp * dst)
            tmp = f32(tmp * dst)
            tmp = f32(tmp + 1.0)
            dst = f32(dst / tmp)
        else:
            tmp = f32(tmp * 0.5)
            tmp = f32(tmp + 1.0)
            dst = f32(dst / tmp)
        if offset != 0.0:
            dst = f32(dst - dc)
        if level != 1.0:
            dst = f32(dst * level)
        out.append(sat(f32(dst * UNIT)))
    return out


class Pickup(unittest.TestCase):

    def compare(self, settings, frames=128):
        """`settings` per block: (mode, drive, offset, level)."""
        rig = Rig(frames, ag.PICKUP, 1)
        got, want = [], []
        for b, (mode, drive, offset, level) in enumerate(settings):
            block = [int(14000 * math.sin(2.0 * math.pi * (b * frames + i) / 53.0))
                     for i in range(frames)]
            rig.set(ag.PICKUP_MODE, mode)
            rig.set(ag.PICKUP_DRIVE, drive)
            rig.set(ag.PICKUP_OFFSET, offset)
            rig.set(ag.PICKUP_LEVEL, level)
            got += rig.run(block)
            want += pickup_ref(block, mode, drive, offset, level)
        return got, want

    def test_matches_pickupcore_to_zero_counts(self):
        settings = []
        for b in range(60):
            mode = ag.PICKUP_ELECTROSTATIC if (b // 15) % 2 else ag.PICKUP_ELECTROMAGNETIC
            drive = 0.5 + 0.375 * (b % 7)
            offset = ((b % 9) - 4) * 0.0625
            level = 0.5 + 0.25 * (b % 5)
            settings.append((mode, drive, offset, level))
        got, want = self.compare(settings)
        self.assertEqual(worst(got, want), 0)
        self.assertGreater(max(abs(v) for v in got), 4000)

    def test_zero_input_has_no_dc_with_nonzero_offset(self):
        for mode in (ag.PICKUP_ELECTROMAGNETIC, ag.PICKUP_ELECTROSTATIC):
            for offset in (-0.35, -0.1, 0.0, 0.15, 0.4):
                rig = Rig(64, ag.PICKUP, 1)
                rig.set(ag.PICKUP_MODE, mode)
                rig.set(ag.PICKUP_DRIVE, 2.5)
                rig.set(ag.PICKUP_OFFSET, offset)
                rig.set(ag.PICKUP_LEVEL, 1.5)
                self.assertEqual(rig.run([0] * 64), [0] * 64)
                rig_open = Rig(64, ag.PICKUP, 1, open_=(0,))
                rig_open.set(ag.PICKUP_MODE, mode)
                rig_open.set(ag.PICKUP_OFFSET, offset)
                self.assertEqual(rig_open.run(), [0] * 64)

    def test_the_check_catches_a_wrong_pickup(self):
        settings = [
            (ag.PICKUP_ELECTROMAGNETIC, 2.25, 0.25, 1.125),
            (ag.PICKUP_ELECTROSTATIC, 1.75, 0.2, 0.875),
        ] * 10
        got, _ = self.compare(settings)
        wrong = {
            "modes swapped": [
                (1 - m, d, o, lv) for m, d, o, lv in settings],
            "offset ignored": [
                (m, d, 0.0, lv) for m, d, o, lv in settings],
            "drive off by 5%": [
                (m, d * 1.05, o, lv) for m, d, o, lv in settings],
            "level off by 5%": [
                (m, d, o, lv * 1.05) for m, d, o, lv in settings],
        }
        for name, bad in wrong.items():
            want = []
            for b, (m, d, o, lv) in enumerate(bad):
                block = [int(14000 * math.sin(2.0 * math.pi * (b * 128 + i) / 53.0))
                         for i in range(128)]
                want += pickup_ref(block, m, d, o, lv)
            self.assertGreater(worst(got, want), 100, name)


# ----------------------------------------------------------------- TREMOLO --

class TremoloRef:
    """vrack's `TremoloCore` (`dsp/tremolo.py`), transcribed operation for
    operation in float32 as numpy does it: each Python scalar rounded to
    float32 before the vector operation, the ramp `1..n`, the phase in double.
    The rate and depth CVs add to the knobs before `TremoloCore.set`."""

    def __init__(self, frames):
        self.frames = frames
        self.phase = 0.0
        self.depth = 0.0
        self.depth_now = 0.0

    def reset(self):
        self.phase = 0.0
        self.depth_now = self.depth

    def set(self, rate=3.0, depth=0.0, rate_cv=None, depth_cv=None):
        rate = float(f32(rate))
        depth = float(f32(depth))
        if rate_cv is not None:
            rate += rate_cv[0] / UNIT
        if depth_cv is not None:
            depth += depth_cv[0] / UNIT
        self.rate = max(0.01, rate)
        self.depth = max(0.0, min(1.0, depth))

    def render(self, audio, rate=3.0, depth=0.0, mode=0,
               rate_cv=None, depth_cv=None):
        self.set(rate, depth, rate_cv, depth_cv)
        n = self.frames
        d0 = self.depth_now
        d1 = self.depth
        self.depth_now = d1
        d_phase = 2.0 * math.pi * self.rate / float(RATE)
        phase = self.phase
        self.phase = (phase + n * d_phase) % (2.0 * math.pi)
        if d0 <= 0.0 and d1 <= 0.0:
            return list(audio), list(audio)
        amp_mode = f32(mode) >= 0.5
        glide = d0 != d1
        depth = 1.0 if glide else d1
        left, right = [], []
        for i in range(n):
            ramp = float(i + 1)
            lfo = a_(m(ramp, d_phase), phase - d_phase)
            lfo = f32(math.sin(lfo))
            if glide:
                lfo = m(lfo, a_(m(ramp, (d1 - d0) / n), d0))
            if amp_mode:
                lfo = m(lfo, 0.5 * depth)
                if glide:
                    lfo = a_(lfo, a_(m(ramp, -0.5 * (d1 - d0) / n), 1.0 - 0.5 * d0))
                else:
                    lfo = a_(lfo, 1.0 - 0.5 * depth)
                gl = gr = lfo
            else:
                lfo = m(lfo, (math.pi / 4.0) * depth)
                c = f32(math.cos(lfo))
                s = f32(math.sin(lfo))
                gl = s_(c, s)
                gr = a_(c, s)
            x = audio[i] / UNIT
            left.append(sat(m(x, gl) * UNIT))
            right.append(sat(m(x, gr) * UNIT))
        return left, right


class Tremolo(unittest.TestCase):

    def compare(self, settings, frames=128, rate_cv=None, depth_cv=None):
        """`settings` per block: (rate, depth, mode)."""
        g = ag.Graph(sample_rate=RATE, frames=frames, channel_count=1,
                     buffers=8, nodes=4)
        in_b = g.buffer()
        rc_b = g.buffer() if rate_cv is not None else None
        dc_b = g.buffer() if depth_cv is not None else None
        left_b, right_b = g.buffer(), g.buffer()
        node = g.add(ag.TREMOLO, ag.ALWAYS, (in_b, rc_b, dc_b), left_b, right_b)
        ref = TremoloRef(frames)
        got_l, got_r, want_l, want_r = [], [], [], []
        buf = array("h", bytes(2 * frames))
        for b, (rate, depth, mode) in enumerate(settings):
            audio = [int(12000 * math.sin(2.0 * math.pi * (b * frames + i) / 47.0))
                     for i in range(frames)]
            g.write(in_b, array("h", audio))
            rc = rate_cv[b] if rate_cv is not None else None
            dc = depth_cv[b] if depth_cv is not None else None
            if rc_b is not None:
                g.write(rc_b, array("h", rc))
            if dc_b is not None:
                g.write(dc_b, array("h", dc))
            g.set(node, ag.TREMOLO_RATE, rate)
            g.set(node, ag.TREMOLO_DEPTH, depth)
            g.set(node, ag.TREMOLO_MODE, mode)
            g.run(None)
            g.read(left_b, buf)
            got_l += list(buf)
            g.read(right_b, buf)
            got_r += list(buf)
            wl, wr = ref.render(audio, rate, depth, mode, rc, dc)
            want_l += wl
            want_r += wr
        return (got_l, got_r), (want_l, want_r)

    def test_matches_tremolocore_in_pan_and_amplitude_modes(self):
        settings = []
        for b in range(60):
            rate = 1.5 + 1.25 * (b % 6)
            depth = ((b // 5) % 5) * 0.25
            mode = ag.TREMOLO_AMPLITUDE if b >= 30 else ag.TREMOLO_PAN
            settings.append((rate, depth, mode))
        (got_l, got_r), (want_l, want_r) = self.compare(settings)
        self.assertLessEqual(worst(got_l, want_l), 1)
        self.assertLessEqual(worst(got_r, want_r), 1)
        self.assertNotEqual(got_l[:128 * 30], got_r[:128 * 30])
        self.assertEqual(got_l[128 * 30:], got_r[128 * 30:])

    def test_rate_and_depth_cv_inputs_and_skipped_group(self):
        frames, blocks = 64, 20
        rate_cv = [[int(UNIT * 1.5)] * frames for _ in range(blocks)]
        depth_cv = [[int(UNIT * 0.5)] * frames for _ in range(blocks)]
        settings = [(2.5, 0.25, ag.TREMOLO_PAN)] * blocks
        (got_l, got_r), (want_l, want_r) = self.compare(
            settings, frames=frames, rate_cv=rate_cv, depth_cv=depth_cv,
        )
        self.assertLessEqual(worst(got_l, want_l), 1)
        self.assertLessEqual(worst(got_r, want_r), 1)

        # Group disable zeroes both left and right outputs, and arg validation
        # refuses an invalid right buffer id.
        g = ag.Graph(sample_rate=RATE, frames=frames, channel_count=1,
                     buffers=6, nodes=4)
        a, left_b, right_b = g.buffer(), g.buffer(), g.buffer()
        for bad in (left_b, 6.0, -1.0, 1.5):
            with self.assertRaises(ValueError):
                g.add(ag.TREMOLO, 2, (a,), left_b, bad)
        n = g.add(ag.TREMOLO, 2, (a,), left_b, right_b)
        g.set(n, ag.TREMOLO_DEPTH, 0.8)
        g.write(a, array("h", [8000] * frames))
        g.run(None, 0, 1 << 2)
        buf = array("h", bytes(2 * frames))
        g.read(right_b, buf)
        self.assertNotEqual(set(buf), {0})
        g.run(None, 0, 0)
        g.read(left_b, buf)
        self.assertEqual(list(buf), [0] * frames)
        g.read(right_b, buf)
        self.assertEqual(list(buf), [0] * frames)

    def test_the_check_catches_a_wrong_tremolo(self):
        settings = [(4.0, 0.75, ag.TREMOLO_PAN)] * 15 + [
            (5.0, 0.6, ag.TREMOLO_AMPLITUDE)] * 15
        (got_l, got_r), _ = self.compare(settings)
        wrong = {
            "modes swapped": [
                (r, d, 1 - m) for r, d, m in settings],
            "depth zeroed": [
                (r, 0.0, m) for r, d, m in settings],
            "rate doubled": [
                (r * 2.0, d, m) for r, d, m in settings],
        }
        for name, bad in wrong.items():
            ref = TremoloRef(128)
            wl, wr = [], []
            for b, (r, d, m) in enumerate(bad):
                audio = [int(12000 * math.sin(2.0 * math.pi * (b * 128 + i) / 47.0))
                         for i in range(128)]
                l_blk, r_blk = ref.render(audio, r, d, m)
                wl += l_blk
                wr += r_blk
            self.assertGreater(max(worst(got_l, wl), worst(got_r, wr)), 100, name)

# ------------------------------------------------------------------- DECAY --

LN_100 = 4.60517


def decay_coeff(seconds, rate=RATE):
    """vrack's exp(-4.60517 / (sr * seconds)), the -40 dB time."""
    steps = max(1.0, f32(seconds) * rate * (1.0 / LN_100))
    return math.exp(-1.0 / steps)


def exp_segment(start, count, coeff):
    """`blocks.exp_segment` into a fresh list: start * coeff**(k+1)."""
    return [start * (coeff ** (k + 1)) for k in range(count)]


class DecayOneshotRef:
    """vrack's `OneShotEnvelopeCore` (`dsp/drums.py`), with the hit at 1.0
    as its `render(gate)` starts it."""

    def __init__(self, decay_sec):
        self.coeff = decay_coeff(decay_sec)
        self.level = 0.0
        self.high = False
        self.ringing = False

    def render(self, gate):
        rising = -1
        high = self.high
        for i, v in enumerate(gate):
            now = v > 0.1
            if now and not high and rising < 0:
                rising = i
            high = now
        self.high = high
        if rising >= 0:
            self.ringing = True
        if not self.ringing and rising < 0:
            return [0] * len(gate)
        out = [0] * len(gate)
        pos = 0
        level = self.level
        while pos < len(gate):
            if rising == pos:
                level = 1.0
                self.ringing = True
                rising = -1
            end = rising if 0 <= rising < len(gate) else len(gate)
            if end > pos:
                seg = exp_segment(level, end - pos, self.coeff)
                out[pos:end] = seg
                level = seg[-1] if seg else level
                pos = end
        if level < 1e-4:
            level = 0.0
            self.ringing = False
        self.level = level
        return out


class DecayBurstRef:
    """The burst envelope inside vrack's `HandClapCore` / `JingleCore` /
    `GuiroCore`: a spike at each (delay, level), a fast fall between spikes
    and a slow one after the last."""

    def __init__(self, table, burst_sec, tail_sec, tail_is_40db=False):
        self.spikes = tuple(
            (int(round(delay * RATE)), f32(level)) for delay, level in table)
        self.d_burst = math.exp(-1.0 / (RATE * burst_sec))
        self.d_tail = (
            decay_coeff(tail_sec) if tail_is_40db
            else math.exp(-1.0 / (RATE * tail_sec)))
        self.level = 0.0
        self.velocity = 1.0
        self.high = False
        self.ringing = False
        self.sample = 0

    def render(self, gate, velocity=1.0):
        n = len(gate)
        rising = -1
        high = self.high
        for i, v in enumerate(gate):
            now = v > 0.1
            if now and not high and rising < 0:
                rising = i
            high = now
        self.high = high
        out = [0] * n
        level = self.level
        v = self.velocity
        sample = self.sample
        for i in range(n):
            if rising == i:
                self.ringing = True
                v = f32(velocity)
                sample = 0
                level = v
                rising = -1
            if not self.ringing:
                continue
            which = None
            for s, (at, lv) in enumerate(self.spikes):
                if at == sample:
                    which = (s, lv)
                    break
            if which is not None:
                level = v * which[1]
                out[i] = sat(level * UNIT)
                sample += 1
                continue
            past = sample > self.spikes[-1][0]
            level *= self.d_tail if past else self.d_burst
            out[i] = sat(level * UNIT)
            sample += 1
        if self.ringing and sample > self.spikes[-1][0] and level < 1e-4:
            level = 0.0
            self.ringing = False
            sample = 0
        self.level = level
        self.velocity = v
        self.sample = sample
        return out


CLAP_TABLE = ((0.0, 0.75), (0.0105, 0.75), (0.0210, 0.75), (0.0315, 0.85))
JINGLE_TABLE = ((0.0, 0.8), (0.016, 0.8), (0.032, 0.8), (0.048, 1.0))
GUIRO_TABLE = tuple((i * 0.016, 0.6 + 0.4 * i / 5.0) for i in range(6))


class DecayTwostageRef:
    """vrack's `CowbellCore` two-stage `_env`: a fast fall for
    `fast_len` seconds, then a tail. Silence is checked once per block, as
    the core checks it."""

    def __init__(self, fast_sec, fast_len, tail_sec, tail_is_40db=True):
        self.d_fast = decay_coeff(fast_sec)
        self.d_tail = (
            decay_coeff(tail_sec) if tail_is_40db
            else math.exp(-1.0 / (RATE * tail_sec)))
        self.fast_n = max(1, int(fast_len * RATE))
        self.level = 0.0
        self.high = False
        self.ringing = False
        self.sample = 0

    def render(self, gate, velocity=1.0):
        n = len(gate)
        rising = -1
        high = self.high
        for i, v in enumerate(gate):
            now = v > 0.1
            if now and not high and rising < 0:
                rising = i
            high = now
        self.high = high
        out = [0] * n
        level = self.level
        sample = self.sample
        for i in range(n):
            if rising == i:
                self.ringing = True
                sample = 0
                level = f32(velocity)
                rising = -1
            if not self.ringing:
                continue
            level *= self.d_fast if sample < self.fast_n else self.d_tail
            out[i] = sat(level * UNIT)
            sample += 1
        if self.ringing and sample >= self.fast_n and level < 2e-4:
            level = 0.0
            self.ringing = False
            sample = 0
        self.level = level
        self.sample = sample
        return out


class DecayAttackRef:
    """vrack's `MaracasCore` `_env`: a linear ramp of `attack` seconds up to
    the velocity, then an exponential fall. Silence is checked once per
    block."""

    def __init__(self, attack_sec, decay_sec):
        self.attack_n = max(1, int(attack_sec * RATE))
        self.coeff = decay_coeff(decay_sec)
        self.level = 0.0
        self.velocity = 1.0
        self.high = False
        self.ringing = False
        self.sample = 0

    def render(self, gate, velocity=1.0):
        n = len(gate)
        rising = -1
        high = self.high
        for i, v in enumerate(gate):
            now = v > 0.1
            if now and not high and rising < 0:
                rising = i
            high = now
        self.high = high
        out = [0] * n
        level = self.level
        v = self.velocity
        sample = self.sample
        for i in range(n):
            if rising == i:
                self.ringing = True
                sample = 0
                level = -f32(velocity) / self.attack_n
                v = f32(velocity)
                rising = -1
            if not self.ringing:
                continue
            if sample < self.attack_n:
                level += v / self.attack_n
            else:
                level *= self.coeff
            out[i] = sat(level * UNIT)
            sample += 1
        if self.ringing and sample >= self.attack_n and level < 1e-4:
            level = 0.0
            self.ringing = False
            sample = 0
        self.level = level
        self.velocity = v
        self.sample = sample
        return out


class DecayRiseRef:
    """vrack's `BridgedTDrumCore` amp-minus-rise strike: both start at the
    step, the amp falls to -40 dB over `decay_sec` and the rise falls 1/e over
    `rise_sec`, and the rise is subtracted. Silence is checked once per
    block."""

    def __init__(self, decay_sec, rise_sec):
        self.d_amp = decay_coeff(decay_sec)
        self.d_rise = decay_coeff(rise_sec)
        self.level = 0.0
        self.rise = 0.0
        self.high = False
        self.ringing = False

    def render(self, gate, velocity=1.0):
        n = len(gate)
        rising = -1
        high = self.high
        for i, v in enumerate(gate):
            now = v > 0.1
            if now and not high and rising < 0:
                rising = i
            high = now
        self.high = high
        out = [0] * n
        level = self.level
        rise = self.rise
        for i in range(n):
            if rising == i:
                self.ringing = True
                step = f32(velocity)
                level = step
                rise = step
                rising = -1
            if not self.ringing:
                continue
            level *= self.d_amp
            rise *= self.d_rise
            out[i] = sat((level - rise) * UNIT)
        if self.ringing and level < 1e-4 and rise < 1e-4:
            level = 0.0
            rise = 0.0
            self.ringing = False
        self.level = level
        self.rise = rise
        return out


class DecayTest(unittest.TestCase):
    """DECAY held to `OneShotEnvelopeCore` and the burst cores' envelopes."""

    def compare_oneshot(self, decay_sec, gate):
        frames = len(gate[0])
        rig = Rig(frames, ag.DECAY, 2)
        rig.set(ag.DECAY_DECAY, decay_sec)
        ref = DecayOneshotRef(decay_sec)
        got, want = [], []
        for block in gate:
            got += rig.run(block, [UNIT] * frames)
            want += [sat(v * UNIT) for v in ref.render([x / UNIT for x in block])]
        return got, want

    def compare_burst(self, table, burst_sec, tail_sec, gate, vels,
                      tail_is_40db=False):
        frames = len(gate[0])
        rig = Rig(frames, ag.DECAY, 2)
        rig.set(ag.DECAY_DECAY, tail_sec if tail_is_40db else tail_sec * LN_100)
        rig.set(ag.DECAY_RESTRIKE, burst_sec * LN_100)
        rig.g.table(rig.node, table)
        ref = DecayBurstRef(table, burst_sec, tail_sec, tail_is_40db)
        got, want = [], []
        for block, vel in zip(gate, vels):
            got += rig.run(block, vel)
            want += ref.render([x / UNIT for x in block],
                               velocity=vel[0] / UNIT)
        return got, want

    def test_matches_oneshotenvelopecore(self):
        frames = 128
        for decay_sec in (0.055, 0.2, 1.2):
            gate = []
            for b in range(30):
                on = (b % 11) < 4
                start = 17 if b % 11 == 0 else 0
                gate.append([UNIT if on and i >= start else 0
                             for i in range(frames)])
            got, want = self.compare_oneshot(decay_sec, gate)
            self.assertLessEqual(worst(got, want), 1, decay_sec)
            self.assertGreater(max(abs(v) for v in got), 8000, decay_sec)

    def test_matches_handclap_jingle_and_guiro_envelopes(self):
        frames = 128
        for name, table, burst, tail, is40 in (
            ("clap", CLAP_TABLE, 0.0028, 0.065, False),
            ("clap909", CLAP_TABLE, 0.0028, 0.090, False),
            ("jingle", JINGLE_TABLE, 0.005, 0.220, True),
            ("guiro", GUIRO_TABLE, 0.0035, 0.0080, False),
        ):
            gate, vels = [], []
            for b in range(40):
                on = (b % 13) < 5
                gate.append([UNIT if on else 0 for _ in range(frames)])
                vels.append([int(UNIT * (0.4 + 0.1 * (b % 5)))] * frames)
            got, want = self.compare_burst(table, burst, tail, gate, vels, is40)
            self.assertLessEqual(worst(got, want), 1, name)
            self.assertGreater(max(abs(v) for v in got), 4000, name)

    def test_the_decay_seconds_scale(self):
        """A decay read as milliseconds must not land on the same envelope."""
        frames, blocks = 128, 20
        gate = [[UNIT if b % 8 < 2 else 0 for _ in range(frames)]
                for b in range(blocks)]
        got, want = self.compare_oneshot(0.2, gate)
        wrong = DecayOneshotRef(0.2 / 1000.0)
        wl = []
        for block in gate:
            wl += [sat(v * UNIT) for v in wrong.render([x / UNIT for x in block])]
        self.assertLessEqual(worst(got, want), 1)
        self.assertGreater(worst(got, wl), 100)

    def test_the_restrike_seconds_scale(self):
        frames, blocks = 128, 30
        gate, vels = [], []
        for b in range(blocks):
            gate.append([UNIT if b % 10 < 4 else 0 for _ in range(frames)])
            vels.append([int(UNIT * 0.7)] * frames)
        got, want = self.compare_burst(CLAP_TABLE, 0.0028, 0.065, gate, vels)
        wrong = DecayBurstRef(CLAP_TABLE, 0.0028 / 1000.0, 0.065)
        wl = []
        for block, vel in zip(gate, vels):
            wl += wrong.render([x / UNIT for x in block], velocity=vel[0] / UNIT)
        self.assertLessEqual(worst(got, want), 1)
        self.assertGreater(worst(got, wl), 100)

    def test_the_velocity_scales_the_hit(self):
        frames, blocks = 128, 12
        gate = [[UNIT if b % 6 < 2 else 0 for _ in range(frames)]
                for b in range(blocks)]
        vels = [[int(UNIT * 0.25)] * frames for _ in range(blocks)]
        got, want = self.compare_burst(CLAP_TABLE, 0.0028, 0.065, gate, vels)
        wrong_vel = [[UNIT] * frames for _ in range(blocks)]
        _, wl = self.compare_burst(CLAP_TABLE, 0.0028, 0.065, gate, wrong_vel)
        self.assertLessEqual(worst(got, want), 1)
        self.assertGreater(worst(got, wl), 100)

    def test_the_check_catches_a_wrong_decay(self):
        frames, blocks = 128, 20
        gate = [[UNIT if b % 8 < 2 else 0 for _ in range(frames)]
                for b in range(blocks)]
        got, _ = self.compare_oneshot(0.2, gate)
        for name, bad in (
            ("decay halved", DecayOneshotRef(0.1)),
            ("decay doubled", DecayOneshotRef(0.4)),
            ("coeff as 1/e not -40 dB", DecayOneshotRef(0.2 / LN_100)),
        ):
            wl = []
            for block in gate:
                wl += [sat(v * UNIT) for v in bad.render([x / UNIT for x in block])]
            self.assertGreater(worst(got, wl), 100, name)

    def test_table_accepts_float_buffer_and_sequences_and_carries(self):
        frames = 128
        flat = array("f", [v for pair in CLAP_TABLE for v in pair])
        g1 = ag.Graph(sample_rate=RATE, frames=frames, channel_count=1,
                      buffers=8, nodes=8)
        t1, v1, o1 = g1.buffer(), g1.buffer(), g1.buffer()
        n1 = g1.add(ag.DECAY, ag.ALWAYS, (t1, v1), o1)
        g1.table(n1, flat)
        g1.set(n1, ag.DECAY_DECAY, 0.3)
        g1.set(n1, ag.DECAY_RESTRIKE, 0.013)

        g2 = ag.Graph(sample_rate=RATE, frames=frames, channel_count=1,
                      buffers=8, nodes=8)
        t2, v2, o2 = g2.buffer(), g2.buffer(), g2.buffer()
        n2 = g2.add(ag.DECAY, ag.ALWAYS, (t2, v2), o2)
        g2.table(n2, CLAP_TABLE)
        g2.set(n2, ag.DECAY_DECAY, 0.3)
        g2.set(n2, ag.DECAY_RESTRIKE, 0.013)

        trig = array("h", [UNIT] * frames)
        zero = array("h", [0] * frames)
        b1 = array("h", bytes(2 * frames))
        b2 = array("h", bytes(2 * frames))
        g1.write(t1, trig)
        g2.write(t2, trig)
        g1.run(None)
        g2.run(None)
        g1.read(o1, b1)
        g2.read(o2, b2)
        self.assertEqual(list(b1), list(b2))

        g3 = ag.Graph(sample_rate=RATE, frames=frames, channel_count=1,
                      buffers=8, nodes=8)
        t3, v3, o3 = g3.buffer(), g3.buffer(), g3.buffer()
        n3 = g3.add(ag.DECAY, ag.ALWAYS, (t3, v3), o3)
        g3.carry(n3, g1, n1)
        g1.write(t1, zero)
        g3.write(t3, zero)
        g1.run(None)
        g3.run(None)
        g1.read(o1, b1)
        g3.read(o3, b2)
        self.assertEqual(list(b1), list(b2))

    def test_a_table_of_pairs_and_a_flat_run(self):
        frames = 64
        rig = Rig(frames, ag.DECAY, 2)
        rig.g.table(rig.node, [(0.0, 1.0), (0.01, 0.5)])
        rig.set(ag.DECAY_DECAY, 0.2)
        rig.set(ag.DECAY_RESTRIKE, 0.02)
        out = rig.run([UNIT] * frames, [UNIT] * frames)
        self.assertGreater(max(abs(v) for v in out), 1000)

    def test_unstruck_is_silence_and_reset_clears_state(self):
        frames = 128
        rig = Rig(frames, ag.DECAY, 2)
        self.assertEqual(set(rig.run([0] * frames, [UNIT] * frames)), {0})
        rig.run([UNIT] * frames, [UNIT] * frames)
        self.assertNotEqual(set(rig.run([0] * frames, [UNIT] * frames)), {0})
        rig.g.reset(rig.node)
        self.assertEqual(set(rig.run([0] * frames, [UNIT] * frames)), {0})

    def test_an_open_velocity_is_one(self):
        frames = 128
        rig = Rig(frames, ag.DECAY, 2, open_=(1,))
        rig.set(ag.DECAY_DECAY, 0.1)
        out = rig.run([UNIT] * frames, None)
        self.assertGreater(max(out), 8000)

    def test_other_block_sizes(self):
        for frames in (32, 64, 256):
            gate = [[UNIT if b % 5 < 2 else 0 for _ in range(frames)]
                    for b in range(12)]
            got, want = self.compare_oneshot(0.15, gate)
            self.assertLessEqual(worst(got, want), 1, frames)

    def compare_twostage(self, fast_sec, fast_len, tail_sec, gate, vels):
        frames = len(gate[0])
        rig = Rig(frames, ag.DECAY, 2)
        rig.set(ag.DECAY_DECAY, tail_sec)
        rig.set(ag.DECAY_RESTRIKE, tail_sec)
        rig.set(ag.DECAY_FAST, fast_sec)
        rig.set(ag.DECAY_FAST_SEC, fast_len)
        ref = DecayTwostageRef(fast_sec, fast_len, tail_sec)
        got, want = [], []
        for block, vel in zip(gate, vels):
            got += rig.run(block, vel)
            want += ref.render([x / UNIT for x in block],
                               velocity=vel[0] / UNIT)
        return got, want

    def compare_attack(self, attack_sec, decay_sec, gate, vels):
        frames = len(gate[0])
        rig = Rig(frames, ag.DECAY, 2)
        rig.set(ag.DECAY_DECAY, decay_sec)
        rig.set(ag.DECAY_RESTRIKE, decay_sec)
        rig.set(ag.DECAY_ATTACK, attack_sec)
        ref = DecayAttackRef(attack_sec, decay_sec)
        got, want = [], []
        for block, vel in zip(gate, vels):
            got += rig.run(block, vel)
            want += ref.render([x / UNIT for x in block],
                               velocity=vel[0] / UNIT)
        return got, want

    def compare_rise(self, decay_sec, rise_sec, gate, vels):
        frames = len(gate[0])
        rig = Rig(frames, ag.DECAY, 2)
        rig.set(ag.DECAY_DECAY, decay_sec)
        rig.set(ag.DECAY_RESTRIKE, decay_sec)
        rig.set(ag.DECAY_RISE, rise_sec)
        ref = DecayRiseRef(decay_sec, rise_sec)
        got, want = [], []
        for block, vel in zip(gate, vels):
            got += rig.run(block, vel)
            want += ref.render([x / UNIT for x in block],
                               velocity=vel[0] / UNIT)
        return got, want

    def test_matches_cowbell_twostage(self):
        """CowbellCore's fast stage then its tail, including the death."""
        frames, blocks = 128, 100
        gate = [[UNIT if b == 0 and i == 0 else 0 for i in range(frames)]
                for b in range(blocks)]
        vels = [[UNIT] * frames for _ in range(blocks)]
        # rhythm808: exp(-1/(sr*0.006)) for 0.015 s, then exp(-6.908/(sr*0.250)).
        got, want = self.compare_twostage(
            0.006 * LN_100, 0.015, 0.250 * LN_100 / 6.908, gate, vels)
        self.assertLessEqual(worst(got, want), 1)
        self.assertGreater(max(abs(v) for v in got), 8000)
        # The tail must die away; a one-stage fall is a different shape.
        self.assertEqual(got[-1], 0)
        self.assertEqual(want[-1], 0)

    def test_matches_rhythm78_twostage(self):
        """Rhythm78Cowbell's faster knock and shorter tail."""
        frames, blocks = 128, 100
        gate = [[UNIT if b == 0 and i == 0 else 0 for i in range(frames)]
                for b in range(blocks)]
        vels = [[UNIT] * frames for _ in range(blocks)]
        # exp(-1/(sr*0.011)) for 0.012 s, then exp(-4.60517/(sr*0.150)).
        got, want = self.compare_twostage(0.011 * LN_100, 0.012, 0.150,
                                          gate, vels)
        self.assertLessEqual(worst(got, want), 1)
        self.assertGreater(max(abs(v) for v in got), 8000)

    def test_matches_maracas_linear_attack(self):
        """MaracasCore's 8 ms ramp into its 0.045 s fall."""
        frames, blocks = 128, 60
        gate = [[UNIT if b == 0 and i == 0 else 0 for i in range(frames)]
                for b in range(blocks)]
        vels = [[UNIT] * frames for _ in range(blocks)]
        got, want = self.compare_attack(0.008, 0.045, gate, vels)
        self.assertLessEqual(worst(got, want), 1)
        self.assertGreater(max(abs(v) for v in got), 8000)
        # The ramp starts at zero, not at the velocity.
        self.assertEqual(got[0], 0)

    def test_matches_bridged_t_amp_minus_rise(self):
        """BridgedTDrumCore's 1 ms rise subtracted from its amp."""
        frames, blocks = 128, 80
        gate = [[UNIT if b == 0 and i == 0 else 0 for i in range(frames)]
                for b in range(blocks)]
        vels = [[UNIT] * frames for _ in range(blocks)]
        # f0 = 52.7 Hz, so the rise is min(0.001, 0.5/f0) = 0.001 s at 1/e.
        got, want = self.compare_rise(0.12, 0.001 * LN_100, gate, vels)
        self.assertLessEqual(worst(got, want), 1)
        self.assertGreater(max(abs(v) for v in got), 8000)
        # The rise bites: the first sample is the step less the rise, not the
        # step itself.
        self.assertEqual(
            got[0], sat(UNIT * (decay_coeff(0.12) - decay_coeff(0.001 * LN_100))))

    def test_only_the_first_rising_edge_in_a_block_retriggers(self):
        """A second gate pulse in the same block does not restart the envelope."""
        frames = 128
        rig = Rig(frames, ag.DECAY, 2)
        rig.set(ag.DECAY_DECAY, 0.2)
        gate = [0] * frames
        gate[10] = UNIT
        gate[20] = 0
        gate[30] = UNIT
        first = rig.run(gate, [UNIT] * frames)
        rig.g.reset(rig.node)
        gate_one = [0] * frames
        gate_one[10] = UNIT
        only = rig.run(gate_one, [UNIT] * frames)
        self.assertEqual(first, only)

    def test_the_gate_threshold_is_a_tenth_of_the_unit(self):
        """A gate at 0.2 UNIT must strike; a threshold raised to 0.3 would not."""
        frames = 128
        rig = Rig(frames, ag.DECAY, 2)
        rig.set(ag.DECAY_DECAY, 0.2)
        out = rig.run([0] * frames, [UNIT] * frames)
        self.assertEqual(set(out), {0})
        out = rig.run([int(0.2 * UNIT)] * frames, [UNIT] * frames)
        self.assertGreater(max(out), 8000)

    def test_a_burst_delay_is_rounded_to_the_nearest_sample(self):
        """A spike whose delay lands past a half sample must not arrive early."""
        frames, blocks = 128, 4
        # 5.6 samples at 48 kHz: rounds to 6, truncates to 5.
        delay = 5.6 / RATE
        table = ((delay, 1.0),)
        gate = [[UNIT if b == 0 else 0 for _ in range(frames)]
                for b in range(blocks)]
        vels = [[UNIT] * frames for _ in range(blocks)]
        got, want = self.compare_burst(table, 0.0028, 0.065, gate, vels)
        self.assertLessEqual(worst(got, want), 1)
        # The spike lands on sample 6: sample 6 writes the level, sample 5 is
        # still the slow fall between spikes. Truncating the delay would put
        # the spike on sample 5 instead.
        self.assertEqual(got[6], sat(UNIT))
        self.assertLess(got[5], sat(UNIT))

    def test_the_spike_on_the_trigger_sample_writes_its_level(self):
        """A table spike at delay 0 writes velocity * level on sample 0."""
        frames = 128
        table = ((0.0, 0.75), (0.0105, 0.75))
        rig = Rig(frames, ag.DECAY, 2)
        rig.set(ag.DECAY_DECAY, 0.065 * LN_100)
        rig.set(ag.DECAY_RESTRIKE, 0.0028 * LN_100)
        rig.g.table(rig.node, table)
        out = rig.run([UNIT] * frames, [int(0.5 * UNIT)] * frames)
        self.assertEqual(out[0], sat(0.5 * 0.75 * UNIT))

    def test_an_unsorted_table_fires_every_spike(self):
        """The rack fires a spike whatever its place in the table."""
        frames = 256
        rows = ((0.0, 1.0), (0.004, 0.9), (0.002, 0.8))
        outs = []
        for table in (rows, tuple(sorted(rows))):
            rig = Rig(frames, ag.DECAY, 2)
            rig.set(ag.DECAY_DECAY, 0.065 * LN_100)
            rig.set(ag.DECAY_RESTRIKE, 0.0028 * LN_100)
            rig.g.table(rig.node, table)
            outs.append(rig.run([UNIT] * frames, [UNIT] * frames))
        self.assertEqual(outs[0], outs[1])

    def test_an_infinite_attack_holds_the_ramp(self):
        """An attack too long to count stays at the start of its ramp."""
        frames = 256
        rig = Rig(frames, ag.DECAY, 2)
        rig.set(ag.DECAY_DECAY, 0.2)
        rig.set(ag.DECAY_ATTACK, float("inf"))
        out = rig.run([UNIT] * frames, [UNIT] * frames)
        self.assertLess(max(abs(v) for v in out), UNIT // 100)

    def test_the_check_catches_a_missing_first_spike(self):
        """A ref that drops the spike-at-zero write must not match the node."""
        frames, blocks = 128, 4
        table = ((0.0, 0.75), (0.0105, 0.75))
        gate = [[UNIT if b == 0 else 0 for _ in range(frames)]
                for b in range(blocks)]
        vels = [[UNIT] * frames for _ in range(blocks)]
        got, want = self.compare_burst(table, 0.0028, 0.065, gate, vels)
        self.assertLessEqual(worst(got, want), 1)
        wrong = DecayBurstRef(table[1:], 0.0028, 0.065)
        wl = []
        for block, vel in zip(gate, vels):
            wl += wrong.render([x / UNIT for x in block],
                               velocity=vel[0] / UNIT)
        self.assertGreater(worst(got, wl), 100)


# -------------------------------------------------------------------- PINK --

class PinkRef:
    """vrack's `NoiseGenerator.pink` (`dsp/noise.py`), Paul Kellet's
    seven-state pinking filter on a white block."""

    def __init__(self):
        self.b = [0.0] * 7

    def render(self, white):
        b0, b1, b2, b3, b4, b5, b6 = self.b
        out = []
        for w in white:
            w = f32(w)
            b0 = f32(0.99886 * b0 + w * 0.0555179)
            b1 = f32(0.99332 * b1 + w * 0.0750759)
            b2 = f32(0.96900 * b2 + w * 0.1538520)
            b3 = f32(0.86650 * b3 + w * 0.3104856)
            b4 = f32(0.55000 * b4 + w * 0.5329522)
            b5 = f32(-0.7616 * b5 - w * 0.0168980)
            y = f32((b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362) * 0.15)
            b6 = f32(w * 0.115926)
            out.append(y)
        self.b = [b0, b1, b2, b3, b4, b5, b6]
        return out


class PinkTest(unittest.TestCase):
    """PINK held to `NoiseGenerator.pink`."""

    def compare(self, white, level=1.0):
        frames = len(white[0])
        rig = Rig(frames, ag.PINK, 1)
        rig.set(ag.PINK_LEVEL, level)
        ref = PinkRef()
        got, want = [], []
        for block in white:
            got += rig.run(block)
            want += [sat(v * level * UNIT) for v in
                     ref.render([x / UNIT for x in block])]
        return got, want

    def test_matches_noisegenerator_pink(self):
        frames, blocks = 128, 40
        white = []
        state = 1
        for _ in range(blocks):
            block = []
            for _ in range(frames):
                state = (1103515245 * state + 12345) & 0x7FFFFFFF
                block.append(int((state % 16384) - 8192))
            white.append(block)
        got, want = self.compare(white)
        self.assertLessEqual(worst(got, want), 1)
        self.assertGreater(max(abs(v) for v in got), 1000)

    def test_the_level_scales_the_output(self):
        frames, blocks = 128, 10
        white = [[int(4000 * math.sin(2.0 * math.pi * i / 37.0))
                  for i in range(frames)] for _ in range(blocks)]
        got, want = self.compare(white, level=0.5)
        _, full = self.compare(white, level=1.0)
        self.assertLessEqual(worst(got, want), 1)
        self.assertGreater(worst(got, full), 100)

    def test_the_check_catches_a_wrong_pink(self):
        frames, blocks = 128, 12
        white = [[int(6000 * math.sin(2.0 * math.pi * (b * frames + i) / 29.0))
                  for i in range(frames)] for b in range(blocks)]
        got, _ = self.compare(white)
        for name, scale in (("level doubled", 2.0), ("level halved", 0.5)):
            wrong = PinkRef()
            wl = []
            for block in white:
                wl += [sat(v * scale * UNIT)
                       for v in wrong.render([x / UNIT for x in block])]
            self.assertGreater(worst(got, wl), 100, name)

    def test_an_open_input_is_silence(self):
        frames = 64
        rig = Rig(frames, ag.PINK, 1, open_=(0,))
        self.assertEqual(set(rig.run(None)), {0})

    def test_other_block_sizes(self):
        for frames in (32, 256):
            white = [[int(5000 * math.sin(2.0 * math.pi * i / 23.0))
                      for i in range(frames)] for _ in range(8)]
            got, want = self.compare(white)
            self.assertLessEqual(worst(got, want), 1, frames)


# ------------------------------------------------------------------- CRUSH --

class CrushRef:
    """The quantise in vrack's `BitDepthRateCore._hold_numpy`
    (`dsp/drums.py`), with the hold and the anti-alias filter stood aside."""

    def __init__(self, bits):
        self.bits = bits

    def render(self, src):
        bits = self.bits
        if bits >= 16:
            return [sat(v * UNIT) for v in src]
        ibits = max(1, min(15, int(bits)))
        step = float(1 << (15 - ibits)) / UNIT
        out = []
        for x in src:
            x = f32(x)
            q = x / step
            q = q + (0.5 if x >= 0 else -0.5)
            q = math.trunc(q)
            q = q * step
            q = max(-2.0, min(2.0, q))
            out.append(sat(q * UNIT))
        return out


class CrushTest(unittest.TestCase):
    """CRUSH held to `BitDepthRateCore`'s quantise."""

    def compare(self, bits, audio):
        frames = len(audio[0])
        rig = Rig(frames, ag.CRUSH, 1)
        rig.set(ag.CRUSH_BITS, bits)
        ref = CrushRef(bits)
        got, want = [], []
        for block in audio:
            got += rig.run(block)
            want += ref.render([x / UNIT for x in block])
        return got, want

    def tone_blocks(self, frames, blocks):
        return [[int(14000 * math.sin(2.0 * math.pi * (b * frames + i) / 41.0))
                 for i in range(frames)] for b in range(blocks)]

    def test_matches_bitdepthratecore_quantise(self):
        frames, blocks = 128, 20
        audio = self.tone_blocks(frames, blocks)
        for bits in (4, 6, 8, 12, 15, 16):
            got, want = self.compare(bits, audio)
            self.assertLessEqual(worst(got, want), 0, bits)

    def test_the_bits_scale(self):
        """Bits is a count, not a fraction and not a step size."""
        frames, blocks = 128, 12
        audio = self.tone_blocks(frames, blocks)
        got, want = self.compare(6, audio)
        self.assertLessEqual(worst(got, want), 0)
        for name, bad in (("5 bits", 5), ("7 bits", 7), ("8 bits", 8)):
            _, wl = self.compare(bad, audio)
            self.assertGreater(worst(got, wl), 20, name)

    def test_the_check_catches_a_wrong_crush(self):
        frames, blocks = 128, 12
        audio = self.tone_blocks(frames, blocks)
        got, want = self.compare(6, audio)
        self.assertLessEqual(worst(got, want), 0)
        for name, bad in (("bits minus one", 5), ("bits plus one", 7),
                          ("bits as step 64 not 6", 10)):
            _, wl = self.compare(bad, audio)
            self.assertGreater(worst(got, wl), 20, name)

    def test_sixteen_bits_passes_the_audio_through(self):
        frames = 64
        audio = [[int(9000 * math.sin(2.0 * math.pi * i / 17.0))
                  for i in range(frames)]]
        got, want = self.compare(16, audio)
        self.assertEqual(got, want)
        self.assertEqual(got, audio[0])

    def test_nan_bits_passes_the_audio_through(self):
        frames = 64
        audio = [[int(9000 * math.sin(2.0 * math.pi * i / 17.0))
                  for i in range(frames)]]
        rig = Rig(frames, ag.CRUSH, 1)
        rig.set(ag.CRUSH_BITS, float("nan"))
        self.assertEqual(rig.run(audio[0]), audio[0])

    def test_minus_infinite_bits_is_one_bit(self):
        frames = 64
        audio = [int(9000 * math.sin(2.0 * math.pi * i / 17.0))
                 for i in range(frames)]
        outs = []
        for bits in (float("-inf"), 1.0):
            rig = Rig(frames, ag.CRUSH, 1)
            rig.set(ag.CRUSH_BITS, bits)
            outs.append(rig.run(audio))
        self.assertEqual(outs[0], outs[1])

    def test_an_open_input_is_silence(self):
        frames = 64
        rig = Rig(frames, ag.CRUSH, 1, open_=(0,))
        self.assertEqual(set(rig.run(None)), {0})

    def test_other_block_sizes(self):
        for frames in (32, 256):
            audio = self.tone_blocks(frames, 8)
            got, want = self.compare(6, audio)
            self.assertLessEqual(worst(got, want), 0, frames)


# -------------------------------------------------------------------- ACID --

class AcidRef:
    """vrack's `dsp/ladder.py` `AcidFilterCore`: four one-pole stages with
    the feedback high-passed, and an accent capacitor."""

    def __init__(self, sample_rate=RATE):
        self.sample_rate = sample_rate
        self.inv_sr = 1.0 / sample_rate
        self.s0 = self.s1 = self.s2 = self.s3 = 0.0
        self.hp_state = 0.0
        self.accent_env = 0.0

    def render(self, block_size, src, dst, cutoff_hz, resonance, env_mod, env, accent):
        decay = math.exp(-self.inv_sr / 0.15)
        s0, s1, s2, s3 = self.s0, self.s1, self.s2, self.s3
        hp = self.hp_state
        acc_env = self.accent_env
        res_gain = resonance * 4.2
        g = 0.5
        for i in range(block_size):
            x = src[i] if src is not None else 0.0
            e = env[i] if env is not None else 0.0
            acc = accent[i] if accent is not None else 0.0
            if acc > 0.3:
                acc_env = 1.0
            else:
                acc_env *= decay
            if (i & 3) == 0:
                total_env = e + acc_env * 0.4
                f = max(30.0, min(14000.0, cutoff_hz * (2.0 ** (total_env * env_mod * 4.0))))
                g = max(0.001, min(0.95, math.tan(math.pi * f * self.inv_sr)))
            res_hp = s3 - hp
            hp += 0.95 * res_hp
            u = math.tanh(x - math.tanh(res_hp * res_gain))
            s0 += g * (u - s0)
            s1 += g * (s0 - s1)
            s2 += g * (s1 - s2)
            s3 += g * (s2 - s3)
            y = s3
            if acc_env > 0.01:
                y = math.tanh(y * (1.0 + acc_env * 1.5))
            dst[i] = y
        self.s0, self.s1, self.s2, self.s3 = s0, s1, s2, s3
        self.hp_state = hp
        self.accent_env = acc_env


class AcidTest(unittest.TestCase):
    """ACID held to `AcidFilterCore`."""

    def compare(self, audio, env, accent, cutoff_hz=400.0, resonance=0.7, env_mod=0.6):
        frames = len(audio[0])
        rig = Rig(frames, ag.ACID, 3)
        rig.set(ag.ACID_CUTOFF_HZ, cutoff_hz)
        rig.set(ag.ACID_RESONANCE, resonance)
        rig.set(ag.ACID_ENVMOD, env_mod)
        ref = AcidRef()
        got, want = [], []
        for a, e, c in zip(audio, env, accent):
            got += rig.run(a, e, c)
            dst = [0.0] * frames
            ref.render(frames, [x / UNIT for x in a], dst, cutoff_hz, resonance,
                       env_mod, [x / UNIT for x in e], [x / UNIT for x in c])
            want += [sat(v * UNIT) for v in dst]
        return got, want

    def tone_blocks(self, frames, blocks, amp=9000):
        return [[int(amp * math.sin(2.0 * math.pi * (b * frames + i) / 47.0))
                 for i in range(frames)] for b in range(blocks)]

    def flat_blocks(self, frames, blocks, value):
        return [[value] * frames for _ in range(blocks)]

    def test_matches_acidfiltercore(self):
        frames, blocks = 128, 40
        audio = self.tone_blocks(frames, blocks)
        env = []
        accent = []
        for b in range(blocks):
            env.append([int(UNIT * min(1.0, (b % 30) / 15.0))] * frames)
            accent.append([int(UNIT * (0.9 if b % 14 == 0 else 0.0))] * frames)
        got, want = self.compare(audio, env, accent)
        self.assertLessEqual(worst(got, want), 1)
        self.assertGreater(max(abs(v) for v in got), 500)

    def test_the_cutoff_hz_moves_the_filter(self):
        """Cutoff is in Hz before the envelope opens it, not a 0..1 lever."""
        frames, blocks = 128, 16
        audio = self.tone_blocks(frames, blocks)
        env = self.flat_blocks(frames, blocks, 0)
        accent = self.flat_blocks(frames, blocks, 0)
        low, _ = self.compare(audio, env, accent, cutoff_hz=180.0)
        high, want = self.compare(audio, env, accent, cutoff_hz=1200.0)
        self.assertLessEqual(worst(high, want), 1)
        self.assertGreater(worst(low, high), 200)

    def test_the_resonance_scales_the_feedback(self):
        """Resonance is 0..1 into a gain of 4.2, not already that gain."""
        frames, blocks = 128, 16
        audio = self.tone_blocks(frames, blocks, 12000)
        env = self.flat_blocks(frames, blocks, 0)
        accent = self.flat_blocks(frames, blocks, 0)
        # held to the rack at 0.85, so a C that scales 0..1 wrong fails here
        wet, want = self.compare(audio, env, accent, resonance=0.85)
        self.assertLessEqual(worst(wet, want), 1)
        # and the parameter is live: turning it down changes the render
        dry, _ = self.compare(audio, env, accent, resonance=0.0)
        self.assertGreater(worst(dry, wet), 0)

    def test_the_env_mod_scales_the_envelope(self):
        """Env_mod is 0..1, times 4 octaves of cutoff travel."""
        frames, blocks = 128, 24
        audio = self.tone_blocks(frames, blocks)
        env = [[int(UNIT * min(1.0, (b % 12) / 6.0))] * frames for b in range(blocks)]
        accent = self.flat_blocks(frames, blocks, 0)
        none, _ = self.compare(audio, env, accent, env_mod=0.0)
        full, want = self.compare(audio, env, accent, env_mod=0.9)
        self.assertLessEqual(worst(full, want), 1)
        self.assertGreater(worst(none, full), 200)

    def test_the_accent_pushes_cutoff_and_drive(self):
        """An accent above 0.3 charges the capacitor; it then decays over 0.15 s."""
        frames, blocks = 128, 30
        audio = self.tone_blocks(frames, blocks)
        env = self.flat_blocks(frames, blocks, 0)
        quiet = self.flat_blocks(frames, blocks, 0)
        loud = []
        for b in range(blocks):
            loud.append([int(UNIT * 0.9)] * frames if b == 4 else [0] * frames)
        plain, _ = self.compare(audio, env, quiet)
        accented, want = self.compare(audio, env, loud)
        self.assertLessEqual(worst(accented, want), 1)
        self.assertGreater(worst(plain, accented), 100)

    def test_the_check_catches_a_wrong_acid(self):
        frames, blocks = 128, 20
        audio = self.tone_blocks(frames, blocks)
        env = [[int(UNIT * min(1.0, (b % 10) / 5.0))] * frames for b in range(blocks)]
        accent = [[int(UNIT * (0.9 if b == 3 else 0.0))] * frames for b in range(blocks)]
        got, want = self.compare(audio, env, accent)
        self.assertLessEqual(worst(got, want), 1)
        for name, bad in (("cutoff doubled", 800.0), ("cutoff halved", 200.0),
                          ("resonance as 0.35", 0.35), ("env_mod as 0.3", 0.3)):
            if name.startswith("cutoff"):
                _, wl = self.compare(audio, env, accent, cutoff_hz=bad)
            elif name.startswith("resonance"):
                _, wl = self.compare(audio, env, accent, resonance=bad)
            else:
                _, wl = self.compare(audio, env, accent, env_mod=bad)
            self.assertGreater(worst(got, wl), 50, name)

    def test_an_open_input_is_silence(self):
        frames = 64
        rig = Rig(frames, ag.ACID, 3, open_=(0,))
        self.assertEqual(set(rig.run(None, [UNIT // 2] * frames, [0] * frames)), {0})

    def test_other_block_sizes(self):
        for frames in (32, 256):
            audio = self.tone_blocks(frames, 12)
            env = [[int(UNIT * min(1.0, (b % 8) / 4.0))] * frames for b in range(12)]
            accent = [[int(UNIT * (0.9 if b == 2 else 0.0))] * frames for b in range(12)]
            got, want = self.compare(audio, env, accent)
            self.assertLessEqual(worst(got, want), 1, frames)

    def test_a_knob_past_its_range_plays_as_its_end(self):
        # resonance and env_mod are held to 0..1 and a NaN cutoff takes the
        # 30 Hz floor: an infinite gain times a still stage would be a NaN
        # the stages kept until a reset.
        frames, blocks = 128, 12
        audio = self.tone_blocks(frames, blocks)
        env = [[int(UNIT * min(1.0, (b % 8) / 4.0))] * frames for b in range(blocks)]
        accent = [[int(UNIT * (0.9 if b == 2 else 0.0))] * frames for b in range(blocks)]

        def render(cutoff_hz=400.0, resonance=0.7, env_mod=0.6):
            rig = Rig(frames, ag.ACID, 3)
            rig.set(ag.ACID_CUTOFF_HZ, cutoff_hz)
            rig.set(ag.ACID_RESONANCE, resonance)
            rig.set(ag.ACID_ENVMOD, env_mod)
            out = []
            for a, e, c in zip(audio, env, accent):
                out += rig.run(a, e, c)
            return out

        inf, nan = float("inf"), float("nan")
        self.assertEqual(worst(render(resonance=inf), render(resonance=1.0)), 0)
        self.assertEqual(worst(render(resonance=nan), render(resonance=0.0)), 0)
        self.assertEqual(worst(render(resonance=-inf), render(resonance=0.0)), 0)
        self.assertEqual(worst(render(env_mod=inf), render(env_mod=1.0)), 0)
        self.assertEqual(worst(render(env_mod=nan), render(env_mod=0.0)), 0)
        self.assertEqual(worst(render(cutoff_hz=nan), render(cutoff_hz=0.0)), 0)
        self.assertGreater(max(abs(v) for v in render(resonance=inf)), 500)


# ----------------------------------------------------------------- SPRING --

class SpringRef:
    """vrack's `dsp/reverb.py` `SpringReverbCore`: two mono Freeverbs at
    roomsize decay and decay + 0.05, wet-only, mixed against the dry signal.
    The tanks are `audiofreeverb.Freeverb` at the rack's block size."""

    def __init__(self, frames, sample_rate=RATE):
        self.frames = frames
        self.left = self._tank(frames, sample_rate)
        self.right = self._tank(frames, sample_rate)

    def _tank(self, frames, sample_rate):
        return audiofreeverb.Freeverb(
            roomsize=0.5, damp=0.55, mix=1.0, buffer_size=frames * 2,
            sample_rate=sample_rate, bits_per_sample=16, samples_signed=True,
            channel_count=1)

    def _wet(self, verb, block):
        verb.play(audiocore.RawSample(array("h", block), sample_rate=RATE,
                                      channel_count=1))
        out = array("h", bytes(audiocore.get_buffer(verb)[1]))
        if len(out) != self.frames:
            out = array("h", bytes(audiocore.get_buffer(verb)[1]))
        return list(out)

    def render(self, block, mix, decay, damp=0.55):
        """(left, right) on the wire, as SpringReverbCore.render leaves them."""
        room_l = max(0.1, min(0.98, decay))
        room_r = max(0.1, min(0.98, decay + 0.05))
        self.left.roomsize = room_l
        self.left.damp = damp
        self.right.roomsize = room_r
        self.right.damp = damp
        wet_l = self._wet(self.left, block)
        wet_r = self._wet(self.right, block)
        left = [sat((1.0 - mix) * x + mix * w) for x, w in zip(block, wet_l)]
        right = [sat((1.0 - mix) * x + mix * w) for x, w in zip(block, wet_r)]
        return left, right


class SpringTest(unittest.TestCase):
    """SPRING held to `SpringReverbCore`."""

    def compare(self, audio, mix=0.25, decay=0.75, damp=0.55):
        frames = len(audio[0])
        g = ag.Graph(sample_rate=RATE, frames=frames, channel_count=1,
                     buffers=8, nodes=3)
        in_b, left_b, right_b = g.buffer(), g.buffer(), g.buffer()
        node = g.add(ag.SPRING, ag.ALWAYS, (in_b,), left_b, right_b)
        g.set(node, ag.SPRING_MIX, mix)
        g.set(node, ag.SPRING_DECAY, decay)
        g.set(node, ag.SPRING_DAMP, damp)
        ref = SpringRef(frames)
        buf = array("h", bytes(2 * frames))
        got_l, got_r, want_l, want_r = [], [], [], []
        for block in audio:
            g.write(in_b, array("h", block))
            g.run(None)
            g.read(left_b, buf)
            got_l += list(buf)
            g.read(right_b, buf)
            got_r += list(buf)
            wl, wr = ref.render(block, mix, decay, damp)
            want_l += wl
            want_r += wr
        return (got_l, want_l), (got_r, want_r)

    def impulse_blocks(self, frames, blocks, amp=12000):
        out = [[0] * frames for _ in range(blocks)]
        out[0][0] = amp
        for b in range(1, blocks):
            out[b] = [int(3000 * math.sin(2.0 * math.pi * (b * frames + i) / 41.0))
                      for i in range(frames)]
        return out

    def test_matches_springreverbcore(self):
        frames, blocks = 128, 60
        audio = self.impulse_blocks(frames, blocks)
        (gl, wl), (gr, wr) = self.compare(audio)
        self.assertLessEqual(worst(gl, wl), 0)
        self.assertLessEqual(worst(gr, wr), 0)
        self.assertGreater(max(abs(v) for v in gl), 100)

    def test_the_tanks_differ(self):
        """The right tank is decay + 0.05, so the two sides are not the same."""
        frames, blocks = 128, 40
        audio = self.impulse_blocks(frames, blocks)
        (gl, _), (gr, _) = self.compare(audio)
        self.assertGreater(worst(gl, gr), 10)

    def test_the_mix_scales_dry_against_wet(self):
        """Mix is 0..1: 0 is dry, 1 is wet only."""
        frames, blocks = 128, 30
        audio = self.impulse_blocks(frames, blocks)
        (dry, dw), _ = self.compare(audio, mix=0.0)
        (half, hw), _ = self.compare(audio, mix=0.5)
        (wet, ww), _ = self.compare(audio, mix=1.0)
        self.assertLessEqual(worst(dry, dw), 0)
        self.assertLessEqual(worst(half, hw), 0)
        self.assertLessEqual(worst(wet, ww), 0)
        self.assertEqual(dry, [x for block in audio for x in block])
        self.assertGreater(worst(dry, half), 50)
        self.assertGreater(worst(half, wet), 50)

    def test_the_decay_scales_the_roomsize(self):
        """Decay is 0..1 room size, clamped 0.1..0.98."""
        frames, blocks = 128, 40
        audio = self.impulse_blocks(frames, blocks)
        (_, _), (short_r, sw) = self.compare(audio, decay=0.2)
        (_, _), (long_r, lw) = self.compare(audio, decay=0.9)
        self.assertLessEqual(worst(short_r, sw), 0)
        self.assertLessEqual(worst(long_r, lw), 0)
        self.assertGreater(worst(short_r, long_r), 50)

    def test_the_damp_scales_the_tank(self):
        """Damp is 0..1, 0.55 as SpringReverbCore builds it."""
        frames, blocks = 128, 30
        audio = self.impulse_blocks(frames, blocks)
        (bright, bw), _ = self.compare(audio, damp=0.1)
        (dark, dw), _ = self.compare(audio, damp=0.9)
        self.assertLessEqual(worst(bright, bw), 0)
        self.assertLessEqual(worst(dark, dw), 0)
        self.assertGreater(worst(bright, dark), 10)

    def test_the_check_catches_a_wrong_spring(self):
        frames, blocks = 128, 30
        audio = self.impulse_blocks(frames, blocks)
        (gl, wl), _ = self.compare(audio)
        self.assertLessEqual(worst(gl, wl), 0)
        for name, kw in (("mix as 0.5", {"mix": 0.5}),
                         ("decay as 0.4", {"decay": 0.4}),
                         ("damp as 0.2", {"damp": 0.2})):
            wrong = SpringRef(frames)
            wl2 = []
            for block in audio:
                left, _ = wrong.render(block, mix=kw.get("mix", 0.25),
                                       decay=kw.get("decay", 0.75),
                                       damp=kw.get("damp", 0.55))
                wl2 += left
            self.assertGreater(worst(gl, wl2), 20, name)

    def test_an_open_input_is_silence(self):
        frames = 64
        rig = Rig(frames, ag.SPRING, 1, float(1), open_=(0,))
        self.assertEqual(set(rig.run(None)), {0})

    def test_other_block_sizes(self):
        for frames in (32, 256):
            audio = self.impulse_blocks(frames, 20)
            (gl, wl), (gr, wr) = self.compare(audio)
            self.assertLessEqual(worst(gl, wl), 0, frames)
            self.assertLessEqual(worst(gr, wr), 0, frames)

    def test_the_right_side_must_be_a_buffer_of_its_own(self):
        g = ag.Graph(sample_rate=RATE, frames=64, channel_count=1, buffers=4, nodes=4)
        a, b = g.buffer(), g.buffer()
        for right in (a, 4.0, -1.0, 1.5, 40000.0):
            with self.assertRaises(ValueError, msg=right):
                g.add(ag.SPRING, ag.ALWAYS, (b,), a, right)
        g.add(ag.SPRING, ag.ALWAYS, (b,), a, 2)

    def test_a_skipped_group_silences_both_sides(self):
        frames = 64
        g = ag.Graph(sample_rate=RATE, frames=frames, channel_count=1, buffers=4, nodes=2)
        in_b, left_b, right_b = g.buffer(), g.buffer(), g.buffer()
        g.add(ag.SPRING, 3, (in_b,), left_b, right_b)
        g.write(in_b, array("h", [8000] * frames))
        buf = array("h", bytes(2 * frames))
        g.run(None, 0, 1 << 3)
        g.read(right_b, buf)
        self.assertEqual(buf[0], 6000)
        g.run(None, 0, 0)
        g.read(left_b, buf)
        self.assertEqual(set(buf), {0})
        g.read(right_b, buf)
        self.assertEqual(set(buf), {0})

    def test_a_carried_tank_keeps_its_own_right_side(self):
        # A recompiled patch may put the right side on another buffer id; the
        # tanks carry over and each graph keeps writing its own right side.
        frames = 64

        def build(spare):
            g = ag.Graph(sample_rate=RATE, frames=frames, channel_count=1,
                         buffers=8, nodes=2)
            for _ in range(spare):
                g.buffer()
            in_b, left_b, right_b = g.buffer(), g.buffer(), g.buffer()
            return g, g.add(ag.SPRING, ag.ALWAYS, (in_b,), left_b, right_b), in_b, left_b, right_b

        src, src_n, src_in, src_l, src_r = build(0)
        dst, dst_n, dst_in, dst_l, dst_r = build(2)
        for block in self.impulse_blocks(frames, 30):
            src.write(src_in, array("h", block))
            src.run(None)
        dst.carry(dst_n, src, src_n)
        buf = array("h", bytes(2 * frames))
        tails = []
        for _ in range(4):
            sides = []
            for g, in_b, left_b, right_b in ((src, src_in, src_l, src_r),
                                             (dst, dst_in, dst_l, dst_r)):
                g.write(in_b, array("h", bytes(2 * frames)))
                g.run(None)
                g.read(left_b, buf)
                left = list(buf)
                g.read(right_b, buf)
                sides.append((left, list(buf)))
            self.assertEqual(sides[1], sides[0])
            tails += sides[0][1]
        self.assertGreater(max(abs(v) for v in tails), 50)


# ------------------------------------------------------------------ ADSR2 --

ATTACK_TARGET = 1.3
DECAY_TOLERANCE = 0.001
RELEASE_TOLERANCE = 0.0005


class Adsr2Ref:
    """vrack's `dsp/envelope.py` `EnvelopeCore` contour, sample by sample as
    the runner walks it, then poly80's remap: out = il + e * (al - il).
    `sustain` is the EnvelopeCore sustain level; poly80 sets it to
    clamp(-il / (al - il), 0, 1) so the sustain step lands on 0."""

    def __init__(self, attack, decay, sustain, release, initial, attack_level,
                 sample_rate=RATE):
        self.a = math.exp(-1.0 / max(1.0, attack * sample_rate * 0.35))
        self.d = math.exp(-1.0 / max(1.0, decay * sample_rate * 0.25))
        self.r = math.exp(-1.0 / max(1.0, release * sample_rate * 0.25))
        self.sus = sustain
        self.il = initial
        self.span = attack_level - initial
        self.stage = 0
        self.val = 0.0
        self.high = False

    def render(self, gate):
        out = []
        for g0 in gate:
            g = g0 > 0.1 * UNIT
            if g != self.high:
                self.high = g
                if g:
                    self.stage = 1
                elif self.stage != 0:
                    self.stage = 4
            if self.stage == 1:
                self.val = ATTACK_TARGET + (self.val - ATTACK_TARGET) * self.a
                if self.val >= 1.0:
                    self.val = 1.0
                    self.stage = 2
            elif self.stage == 2:
                self.val = self.sus + (self.val - self.sus) * self.d
                if abs(self.val - self.sus) <= DECAY_TOLERANCE:
                    self.val = self.sus
                    self.stage = 3
            elif self.stage == 3:
                self.val = self.sus
            elif self.stage == 4:
                self.val *= self.r
                if self.val <= RELEASE_TOLERANCE:
                    self.val = 0.0
                    self.stage = 0
            else:
                self.val = 0.0
            out.append(sat((self.il + self.val * self.span) * UNIT))
        return out


class Adsr2Test(unittest.TestCase):
    """ADSR2 held to `EnvelopeCore`'s contour remapped by IL and AL."""

    def compare(self, gate, attack=0.01, decay=0.2, sustain=0.7, release=0.3,
                initial=0.0, attack_level=1.0):
        frames = len(gate[0])
        rig = Rig(frames, ag.ADSR2, 1)
        rig.set(ag.ADSR2_ATTACK, attack)
        rig.set(ag.ADSR2_DECAY, decay)
        rig.set(ag.ADSR2_SUSTAIN, sustain)
        rig.set(ag.ADSR2_RELEASE, release)
        rig.set(ag.ADSR2_INITIAL, initial)
        rig.set(ag.ADSR2_ATTACK_LEVEL, attack_level)
        ref = Adsr2Ref(attack, decay, sustain, release, initial, attack_level)
        got, want = [], []
        for block in gate:
            got += rig.run(block)
            want += ref.render(block)
        return got, want

    def gate_blocks(self, frames, blocks, period=10, high=4):
        return [[UNIT if (b % period) < high else 0 for _ in range(frames)]
                for b in range(blocks)]

    def test_matches_envelopecore(self):
        frames, blocks = 128, 50
        gate = self.gate_blocks(frames, blocks)
        got, want = self.compare(gate, initial=-0.5, attack_level=0.5)
        self.assertLessEqual(worst(got, want), 1)
        self.assertGreater(max(abs(v) for v in got), 1000)

    def test_poly80_filter_envelope(self):
        """poly80's IL/AL: rest at IL, peak at AL, sustain at 0."""
        frames, blocks = 128, 80
        # a long hold so the envelope reaches its sustain step
        gate = [[0] * frames]
        gate += [[UNIT] * frames for _ in range(30)]
        gate += [[0] * frames for _ in range(blocks - 31)]
        il, al = -0.4, 0.8
        span = al - il
        sus = max(0.0, min(1.0, -il / span))
        got, want = self.compare(gate, attack=0.002, decay=0.005, sustain=sus,
                                 release=0.01, initial=il, attack_level=al)
        self.assertLessEqual(worst(got, want), 1)
        # the rest level is IL and the sustain step is 0
        self.assertEqual(got[0], sat(il * UNIT))
        held = got[20 * frames:28 * frames]
        self.assertLessEqual(max(abs(v) for v in held), 1)

    def test_the_initial_level_is_where_it_rests(self):
        """Initial is a lever, 1.0 = UNIT; the rest level is initial * UNIT."""
        frames, blocks = 128, 20
        # the gate starts low, so the first samples are the rest level
        gate = [[0] * frames] + self.gate_blocks(frames, blocks - 1)
        got, want = self.compare(gate, initial=-0.5, attack_level=0.5)
        self.assertLessEqual(worst(got, want), 1)
        self.assertEqual(got[0], sat(-0.5 * UNIT))
        self.assertEqual(want[0], sat(-0.5 * UNIT))
        # a C that scaled initial wrong (counts, or doubled) fails the bar above
        for name, bad in (("initial doubled", -1.0), ("initial as 0", 0.0)):
            _, wl = self.compare(gate, initial=bad, attack_level=0.5)
            self.assertGreater(worst(got, wl), 100, name)

    def test_the_attack_level_is_the_peak(self):
        """Attack_level is the peak lever; the peak is attack_level * UNIT."""
        frames, blocks = 128, 20
        gate = self.gate_blocks(frames, blocks)
        got, want = self.compare(gate, initial=-0.5, attack_level=0.5)
        self.assertLessEqual(worst(got, want), 1)
        self.assertGreater(max(got), sat(0.4 * UNIT))
        for name, bad in (("attack_level doubled", 1.0),
                          ("attack_level as 0.25", 0.25)):
            _, wl = self.compare(gate, initial=-0.5, attack_level=bad)
            self.assertGreater(worst(got, wl), 100, name)

    def test_the_attack_seconds_scale(self):
        frames, blocks = 128, 20
        gate = self.gate_blocks(frames, blocks)
        got, want = self.compare(gate, attack=0.05)
        self.assertLessEqual(worst(got, want), 1)
        _, wl = self.compare(gate, attack=0.05 / 1000.0)
        self.assertGreater(worst(got, wl), 100)

    def test_the_decay_seconds_scale(self):
        frames, blocks = 128, 30
        gate = self.gate_blocks(frames, blocks)
        got, want = self.compare(gate, decay=0.3)
        self.assertLessEqual(worst(got, want), 1)
        _, wl = self.compare(gate, decay=0.3 / 1000.0)
        self.assertGreater(worst(got, wl), 100)

    def test_the_sustain_level_scales(self):
        frames, blocks = 128, 30
        gate = self.gate_blocks(frames, blocks)
        got, want = self.compare(gate, sustain=0.6, initial=0.0, attack_level=1.0)
        self.assertLessEqual(worst(got, want), 1)
        _, wl = self.compare(gate, sustain=0.3)
        self.assertGreater(worst(got, wl), 100)

    def test_the_release_seconds_scale(self):
        frames, blocks = 128, 30
        gate = self.gate_blocks(frames, blocks, period=8, high=2)
        got, want = self.compare(gate, release=0.4)
        self.assertLessEqual(worst(got, want), 1)
        _, wl = self.compare(gate, release=0.4 / 1000.0)
        self.assertGreater(worst(got, wl), 100)

    def test_the_check_catches_a_wrong_adsr2(self):
        frames, blocks = 128, 30
        gate = self.gate_blocks(frames, blocks)
        got, want = self.compare(gate, initial=-0.5, attack_level=0.5)
        self.assertLessEqual(worst(got, want), 1)
        for name, kw in (("initial as -0.25", {"initial": -0.25}),
                         ("attack_level as 0.25", {"attack_level": 0.25}),
                         ("attack as 0.005", {"attack": 0.005}),
                         ("sustain as 0.4", {"sustain": 0.4})):
            args = dict(initial=-0.5, attack_level=0.5)
            args.update(kw)
            _, wl = self.compare(gate, **args)
            self.assertGreater(worst(got, wl), 50, name)

    def test_an_open_input_is_silence(self):
        frames = 64
        rig = Rig(frames, ag.ADSR2, 1, open_=(0,))
        self.assertEqual(set(rig.run(None)), {0})

    def test_other_block_sizes(self):
        for frames in (32, 256):
            gate = self.gate_blocks(frames, 16)
            got, want = self.compare(gate, initial=-0.25, attack_level=0.75)
            self.assertLessEqual(worst(got, want), 1, frames)

    def test_a_nan_knob_plays_as_the_end_it_is_past(self):
        # A NaN time is one sample and a NaN sustain is 0: a NaN level never
        # reaches the release's floor, so the voice would never go idle.
        frames, blocks = 128, 24
        gate = self.gate_blocks(frames, blocks, period=8, high=3)

        def render(**kw):
            rig = Rig(frames, ag.ADSR2, 1)
            knobs = dict(attack=0.01, decay=0.05, sustain=0.6, release=0.005)
            knobs.update(kw)
            rig.set(ag.ADSR2_ATTACK, knobs["attack"])
            rig.set(ag.ADSR2_DECAY, knobs["decay"])
            rig.set(ag.ADSR2_SUSTAIN, knobs["sustain"])
            rig.set(ag.ADSR2_RELEASE, knobs["release"])
            rig.set(ag.ADSR2_INITIAL, -0.5)
            rig.set(ag.ADSR2_ATTACK_LEVEL, 0.5)
            out = []
            for block in gate:
                out += rig.run(block)
            return out

        nan = float("nan")
        self.assertEqual(worst(render(sustain=nan), render(sustain=0.0)), 0)
        self.assertEqual(worst(render(sustain=-float("inf")), render(sustain=0.0)), 0)
        for name in ("attack", "decay", "release"):
            self.assertEqual(worst(render(**{name: nan}), render(**{name: 0.0})), 0, name)
        # after the last release it rests at the initial level again
        self.assertEqual(render(sustain=nan)[-1], -UNIT // 2)


# ------------------------------------------------------------------- GLISS --

class GlissRef:
    """`GlideCore`'s linear glide (dsp/glide.py) then poly80's glissando:
    once a block the last glided sample is rounded to the step grid and that
    one value is written across the block. The glide's own state stays
    unquantised, so the next block starts from where the sweep really was.
    A semitone is UNIT/12 on the wire, so the grid is step * UNIT / 12."""

    def __init__(self, mode, time, step):
        self.glide = GlideLinear(mode, time)
        self.grid = step * UNIT / 12.0

    def process(self, note, pitch=None, legato=0.0):
        glided = self.glide.process(note, None, legato)
        held = round(glided[-1] / self.grid) * self.grid
        if pitch is not None:
            return [sat(held + p - m) for p, m in zip(pitch, note)]
        return [sat(held)] * len(note)


class GlissTest(unittest.TestCase):
    """GLISS held to `GlideCore` plus poly80's once-a-block semitone step."""

    def compare(self, phrase_blocks, mode=ag.LAG_ON, time=0.015625, step=1.0,
                bend=True, frames=None):
        if frames is None:
            frames = len(phrase_blocks[0][0])
        rig = Rig(frames, ag.GLISS, 3)
        rig.set(ag.GLISS_MODE, mode)
        rig.set(ag.GLISS_TIME, time)
        rig.set(ag.GLISS_STEP, step)
        ref = GlissRef(mode, time, step)
        got, want = [], []
        for note, pitch, legato in phrase_blocks:
            got += rig.run(note, pitch if bend else note, legato)
            want += ref.process(note, pitch if bend else None, legato[0] / UNIT)
        return got, want

    def test_matches_glidecore_and_poly80_glissando(self):
        frames, blocks = 128, 60
        got, want = self.compare(phrase(frames, blocks))
        self.assertEqual(worst(got, want), 0)
        # without a bend it really steps: whole semitones and nothing else
        flat, _ = self.compare(phrase(frames, blocks), bend=False)
        grid = UNIT / 12.0
        for v in flat:
            self.assertEqual(v, sat(round(v / grid) * grid))

    def test_poly80_steps_in_semitones(self):
        """poly80's glissando is one semitone a step: step 1, grid UNIT/12."""
        frames, blocks = 128, 40
        got, want = self.compare(phrase(frames, blocks), step=1.0)
        self.assertEqual(worst(got, want), 0)
        self.assertEqual(GlissRef(ag.LAG_ON, 0.015625, 1.0).grid, UNIT / 12.0)
        # a C that took the step as octaves (12) or as half a semitone fails
        for name, bad in (("step as octaves", 12.0), ("step as half", 0.5)):
            _, wl = self.compare(phrase(frames, blocks), step=bad)
            self.assertGreater(worst(got, wl), 100, name)

    def test_the_time_seconds_scale(self):
        frames, blocks = 128, 40
        got, want = self.compare(phrase(frames, blocks), time=0.03125)
        self.assertEqual(worst(got, want), 0)
        _, wl = self.compare(phrase(frames, blocks), time=0.03125 / 1000.0)
        self.assertGreater(worst(got, wl), 100)

    def test_the_mode_selects_when_it_glides(self):
        frames, blocks = 128, 40
        phrase_blocks = phrase(frames, blocks)
        on, _ = self.compare(phrase_blocks, mode=ag.LAG_ON, time=0.03125)
        off, _ = self.compare(phrase_blocks, mode=ag.LAG_OFF, time=0.03125)
        auto, auto_want = self.compare(phrase_blocks, mode=ag.LAG_AUTO, time=0.03125)
        self.assertNotEqual(on, off)
        self.assertNotEqual(on, auto)
        # AUTO with legato held glides as GlideCore does, so it is not OFF
        self.assertEqual(worst(auto, auto_want), 0)
        self.assertNotEqual(auto, off)
        # AUTO glides only on a held note: with no legato it is OFF
        no_legato = [(n, p, [0] * frames) for n, p, _ in phrase_blocks]
        auto_free, _ = self.compare(no_legato, mode=ag.LAG_AUTO, time=0.03125)
        off_free, _ = self.compare(no_legato, mode=ag.LAG_OFF, time=0.03125)
        self.assertEqual(auto_free, off_free)
        # a C that always glided would match ON here, not OFF
        self.assertNotEqual(auto_free, on)

    def test_the_step_scales_the_grid(self):
        frames, blocks = 128, 40
        one, _ = self.compare(phrase(frames, blocks), step=1.0, bend=False)
        two, _ = self.compare(phrase(frames, blocks), step=2.0, bend=False)
        self.assertNotEqual(one, two)
        grid_one, grid_two = UNIT / 12.0, UNIT / 6.0
        for v in two:
            self.assertEqual(v, sat(round(v / grid_two) * grid_two))
        for v in one:
            self.assertEqual(v, sat(round(v / grid_one) * grid_one))

    def test_the_bend_rides_on_top(self):
        """The step is the note; the bend moves every sample on top of it."""
        frames, blocks = 128, 30
        got, want = self.compare(phrase(frames, blocks), bend=True)
        self.assertEqual(worst(got, want), 0)
        flat, _ = self.compare(phrase(frames, blocks), bend=False)
        self.assertNotEqual(got, flat)
        # with a bend the output is not on the grid, but the note under it is
        grid = UNIT / 12.0
        self.assertGreater(sum(1 for v in got if v != sat(round(v / grid) * grid)),
                           100)

    def test_the_check_catches_a_wrong_gliss(self):
        frames, blocks = 128, 40
        got, want = self.compare(phrase(frames, blocks), time=0.03125, step=1.0)
        self.assertEqual(worst(got, want), 0)
        for name, kw in (("time as 0.003", {"time": 0.003125}),
                         ("step as 2", {"step": 2.0}),
                         ("mode off", {"mode": ag.LAG_OFF})):
            args = dict(time=0.03125, step=1.0)
            args.update(kw)
            _, wl = self.compare(phrase(frames, blocks), **args)
            self.assertGreater(worst(got, wl), 50, name)

    def test_reset_forgets_the_pitch(self):
        frames = 128
        rig = Rig(frames, ag.GLISS, 3)
        rig.set(ag.GLISS_TIME, 0.5)
        rig.set(ag.GLISS_STEP, 1.0)
        note, pitch, legato = [0] * frames, [0] * frames, [0] * frames
        rig.run(note, pitch, legato)
        note = [8192] * frames
        pitch = [8192] * frames
        self.assertNotEqual(rig.run(note, pitch, legato)[-1], 8192)
        rig.g.reset(rig.node)
        self.assertEqual(rig.run(note, pitch, legato), [8192] * frames)

    def test_an_open_note_is_zero(self):
        frames = 64
        rig = Rig(frames, ag.GLISS, 3, open_=(0,))
        self.assertEqual(set(rig.run(None, [0] * frames, [0] * frames)), {0})

    def test_other_block_sizes(self):
        for frames in (32, 256):
            got, want = self.compare(phrase(frames, 24), frames=frames)
            self.assertEqual(worst(got, want), 0, frames)

    def test_a_step_past_its_range_leaves_the_glide_unstepped(self):
        # An infinite step would round to inf * 0, and a tiny one to a
        # quotient past an int32: both play the glide as it is, unstepped.
        frames, blocks = 128, 30

        def render(step):
            rig = Rig(frames, ag.GLISS, 3)
            rig.set(ag.GLISS_MODE, ag.LAG_ON)
            rig.set(ag.GLISS_TIME, 0.0625)
            rig.set(ag.GLISS_STEP, step)
            out = []
            for note, _pitch, legato in phrase(frames, blocks):
                out += rig.run(note, note, legato)
            return out

        plain = render(0.0)
        self.assertEqual(worst(render(float("inf")), plain), 0)
        self.assertEqual(worst(render(1e-30), plain), 0)
        self.assertEqual(worst(render(float("nan")), plain), 0)
        self.assertGreater(max(abs(v) for v in plain), 1000)
        self.assertNotEqual(render(1.0), plain)


# ----------------------------------------------------------------- FOLLOW --

class FollowRef:
    """clavichord's auto-wah peak follower (output.py: the block's peak
    absolute sample, moved toward by 0.4 up and 0.06 down) and vocoder330's
    band follower (bank.py `_follow`: the block's RMS about its own mean,
    moved toward by 1 - exp(-dt/t)). Once a block, that one value is written
    across the block. The node takes times, so at a given block size the two
    are the same one-pole: clavichord's 0.4 and 0.06 are times of
    -dt/ln(0.6) and -dt/ln(0.94)."""

    def __init__(self, attack, release, mode, sample_rate=RATE):
        self.attack = attack
        self.release = release
        self.mode = mode
        self.sample_rate = sample_rate
        self.env = 0.0

    def render(self, block):
        n = len(block)
        dt = n / float(self.sample_rate)
        if self.mode == ag.FOLLOW_RMS:
            mean = sum(block) / float(n)
            var = sum(v * v for v in block) / float(n) - mean * mean
            level = math.sqrt(var) if var > 0.0 else 0.0
        else:
            level = max(abs(v) for v in block) if n else 0.0
        t = self.attack if level > self.env else self.release
        coef = math.exp(-dt / t) if t > 0.0 else 0.0
        self.env = level + (self.env - level) * coef
        return [sat(self.env)] * n


class ClavichordRef:
    """clavichord's own coefficients, not times: 0.4 toward a peak, 0.06 away."""

    def __init__(self):
        self.env = 0.0

    def render(self, block):
        peak = max(abs(v) for v in block) if block else 0.0
        if peak > self.env:
            self.env += (peak - self.env) * 0.4
        else:
            self.env += (peak - self.env) * 0.06
        return [sat(self.env)] * len(block)


class FollowTest(unittest.TestCase):
    """FOLLOW held to clavichord's peak follower and vocoder330's RMS one."""

    def compare(self, blocks, attack=0.005, release=0.030,
                mode=ag.FOLLOW_PEAK, frames=None):
        if frames is None:
            frames = len(blocks[0])
        rig = Rig(frames, ag.FOLLOW, 1)
        rig.set(ag.FOLLOW_ATTACK, attack)
        rig.set(ag.FOLLOW_RELEASE, release)
        rig.set(ag.FOLLOW_MODE, mode)
        ref = FollowRef(attack, release, mode)
        got, want = [], []
        for block in blocks:
            got += rig.run(block)
            want += ref.render(block)
        return got, want

    def program(self, frames, blocks):
        """A burst, a quiet stretch and another burst, in counts."""
        out = []
        for b in range(blocks):
            if 5 <= b < 15 or 25 <= b < 35:
                amp = 9000 + 2000 * (b % 5)
            else:
                amp = 300
            out.append([int(amp * math.sin(2.0 * math.pi * (b * frames + i) / 37.0))
                        for i in range(frames)])
        return out

    def test_clavichords_auto_wah_peak_follower(self):
        """clavichord's 0.4 and 0.06 are -dt/ln(0.6) and -dt/ln(0.94) at 128."""
        frames = 128
        dt = frames / float(RATE)
        attack = -dt / math.log(0.6)
        release = -dt / math.log(0.94)
        blocks = self.program(frames, 50)
        got, want = self.compare(blocks, attack=attack, release=release,
                                 mode=ag.FOLLOW_PEAK)
        self.assertEqual(worst(got, want), 0)
        # and against clavichord's own per-block coefficients
        ref = ClavichordRef()
        want2 = []
        for block in blocks:
            want2 += ref.render(block)
        self.assertEqual(worst(got, want2), 0)
        self.assertGreater(max(got), 8000)

    def test_vocoder330_band_rms_follower(self):
        """vocoder330's FOLLOW_ATTACK_S=0.005 and FOLLOW_RELEASE_S=0.030."""
        frames = 128
        blocks = self.program(frames, 50)
        got, want = self.compare(blocks, attack=0.005, release=0.030,
                                 mode=ag.FOLLOW_RMS)
        self.assertEqual(worst(got, want), 0)
        self.assertGreater(max(got), 4000)

    def test_the_peak_and_rms_levels_differ(self):
        frames = 128
        blocks = self.program(frames, 40)
        peak, _ = self.compare(blocks, mode=ag.FOLLOW_PEAK)
        rms, _ = self.compare(blocks, mode=ag.FOLLOW_RMS)
        self.assertNotEqual(peak, rms)
        # a C that always took the peak would match PEAK here, not RMS
        self.assertGreater(worst(peak, rms), 100)

    def test_the_attack_seconds_scale(self):
        frames = 128
        blocks = self.program(frames, 40)
        got, want = self.compare(blocks, attack=0.005, release=0.030)
        self.assertEqual(worst(got, want), 0)
        _, wl = self.compare(blocks, attack=0.005 / 1000.0, release=0.030)
        self.assertGreater(worst(got, wl), 100)

    def test_the_release_seconds_scale(self):
        frames = 128
        blocks = self.program(frames, 40)
        got, want = self.compare(blocks, attack=0.005, release=0.030)
        self.assertEqual(worst(got, want), 0)
        _, wl = self.compare(blocks, attack=0.005, release=0.030 * 1000.0)
        self.assertGreater(worst(got, wl), 100)

    def test_the_check_catches_a_wrong_follow(self):
        frames = 128
        blocks = self.program(frames, 40)
        got, want = self.compare(blocks, attack=0.01, release=0.04)
        self.assertEqual(worst(got, want), 0)
        for name, kw in (("attack as 0.001", {"attack": 0.001}),
                         ("release as 0.4", {"release": 0.4}),
                         ("mode as rms", {"mode": ag.FOLLOW_RMS})):
            args = dict(attack=0.01, release=0.04)
            args.update(kw)
            _, wl = self.compare(blocks, **args)
            self.assertGreater(worst(got, wl), 50, name)

    def test_an_open_input_decays_to_zero(self):
        frames = 64
        rig = Rig(frames, ag.FOLLOW, 1, open_=(0,))
        out = rig.run(None)
        self.assertEqual(set(out), {0})
        # after a burst the open input walks the level down, not to zero at once
        rig2 = Rig(frames, ag.FOLLOW, 1)
        rig2.set(ag.FOLLOW_ATTACK, 0.005)
        rig2.set(ag.FOLLOW_RELEASE, 0.030)
        loud = [12000] * frames
        for _ in range(10):
            rig2.run(loud)
        self.assertGreater(rig2.run(loud)[-1], 5000)
        quiet = [0] * frames
        first = rig2.run(quiet)
        self.assertGreater(first[-1], 0)
        self.assertLess(first[-1], 12000)

    def test_reset_forgets_the_level(self):
        frames = 64
        rig = Rig(frames, ag.FOLLOW, 1)
        rig.set(ag.FOLLOW_ATTACK, 0.005)
        rig.set(ag.FOLLOW_RELEASE, 0.030)
        for _ in range(10):
            rig.run([12000] * frames)
        self.assertGreater(rig.run([12000] * frames)[-1], 5000)
        rig.g.reset(rig.node)
        self.assertEqual(rig.run([0] * frames), [0] * frames)

    def test_other_block_sizes(self):
        for frames in (32, 256):
            blocks = self.program(frames, 40)
            got, want = self.compare(blocks, frames=frames)
            # the node's level is a float32 state and the rack keeps a Python
            # float; at 256 frames that is one count
            self.assertLessEqual(worst(got, want), 1, frames)