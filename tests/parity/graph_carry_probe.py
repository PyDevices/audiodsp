"""audiograph's carry(): a graph built beside a running one takes over its nodes' state.

    graph_carry_probe.py                 # prints the render, then GRAPH CARRY PASS
    graph_carry_probe.py --fault offbeat # the carry taken one block late

The same file runs on CPython, MicroPython and CircuitPython. ``verify_graph.py``
runs it on each interpreter and compares the renders sample for sample; this
file checks on its own that a carry hands every node kind over whole.

One patch with every node kind in it (two oscillators, a ladder, a biquad,
three feedback delays with a wow table, an envelope on a gated voice group,
an LFO of each shape, noise, mixers, a VCA, a phaser, glides, a clocked and a
free sample-and-hold, a two-pole, a tanh, a pan, a wavefolder, a low-pass
gate, a modal resonator and, where the build has it, an overdrive) is built
three times:

- R, the reference, renders 140 blocks.
- S, the source, renders the first 48 of them, and its block-48 changes are
  queued (a gate, a cutoff, a delay's feedback), so they are pending when the
  carry happens.
- D, the destination, is built fresh, handed every change S was handed (as a
  patch compiled beside a running one is built from the panel as it stands),
  and then takes each of S's nodes with ``carry()``.

From block 48 on, D must render what R renders, on every node, byte for byte.
S keeps rendering for eight more blocks with other inputs, so a node that
still reads S's memory (a delay line, a phaser's stages, a modal bank) comes
out wrong; then S is dropped and the heap churned, so a wow table D borrowed
from S must have been kept alive. Of the three delays, the first shares one
wow table object across the graphs, the second has an equal copy in each, and
the third is handed a different table in D at the carry, which R is handed at
block 48 too: D must swap tables as R does, dipping the wow and coming back.

A D that takes nothing must differ from R (the comparison sees state). And
carry() must refuse a node of another kind, one built with another arg, one
whose state differs in size and one from a graph at another sample rate.

The planted faults of the C, for a build made with
``-DAUDIODSP_GRAPH_CARRY_FAULT=1`` (the delay line left pointing at the
source's) or ``=2`` (the source's pending parameter change dropped), make
this probe fail; CI builds both and runs it. ``--fault offbeat`` plants a
caller's mistake: D takes S's nodes one block after S rendered block 48, so
the carry is off the block boundary.
"""

import gc
import sys
from array import array

try:
    import audiograph as ag
    ag.MODAL
    ag.Graph.carry
except (ImportError, AttributeError):
    print("GRAPH PARITY SKIPPED: no audiograph carry here")
    sys.exit(0)

FAULTS = ("offbeat",)
FAULT = None
if "--fault" in sys.argv:
    FAULT = sys.argv[sys.argv.index("--fault") + 1]
    if FAULT not in FAULTS:
        print("unknown fault %r" % FAULT)
        sys.exit(2)

RATE = 48000
FRAMES = 128
BLOCKS = 140
SWAP = 48
CHURN = 8
UNIT = ag.UNIT
HAS_OVERDRIVE = hasattr(ag, "OVERDRIVE")
failures = []
if not HAS_OVERDRIVE:
    print("absent OVERDRIVE overdrive")


def check(ok, what):
    if not ok:
        failures.append(what)


# A triangle for the delays' wow, and a square for the third delay's new one.
WOW = [(i if i < 128 else 255 - i) * 256 - 16384 for i in range(256)]
WOW_NEW = [8192 if i < 32 else -8192 for i in range(64)]
SHARED_WOW = array("h", WOW)


class Patch:
    """One build of the patch: its graph, its nodes by name and what it watches."""

    def __init__(self, rate=RATE):
        g = self.g = ag.Graph(sample_rate=rate, frames=FRAMES, channel_count=2,
                              buffers=64, nodes=64)
        self.n = {}
        self.watched = []
        self.wow_copy = array("h", WOW)  # the second delay's table, this graph's own
        self.wow_new = array("h", WOW_NEW)

        def node(name, kind, inputs, arg=0.0, group=ag.ALWAYS):
            out = g.buffer()
            self.n[name] = g.add(kind, group, inputs, out, arg)
            self.watched.append((name, out))
            return out

        # what Python writes into the graph each block
        self.note_b, self.clock_b, self.strike_b = g.buffer(), g.buffer(), g.buffer()
        gate_b = node("gate", ag.CONST, ())
        lfo = {}
        for name, shape in (("triangle", ag.LFO_TRIANGLE), ("sine", ag.LFO_SINE),
                            ("saw", ag.LFO_SAW), ("square", ag.LFO_SQUARE),
                            ("random", ag.LFO_RANDOM)):
            lfo[name] = node("lfo_" + name, ag.LFO, ())
            g.set(self.n["lfo_" + name], ag.LFO_SHAPE, shape)
            g.set(self.n["lfo_" + name], ag.LFO_AMPLITUDE, 4096.0)
        g.set(self.n["lfo_triangle"], ag.LFO_RATE, 5.5)
        g.set(self.n["lfo_sine"], ag.LFO_RATE, 3.25)
        g.set(self.n["lfo_saw"], ag.LFO_RATE, 0.75)
        g.set(self.n["lfo_square"], ag.LFO_RATE, 37.5)
        g.set(self.n["lfo_random"], ag.LFO_RATE, 12.25)
        noise_b = node("noise", ag.NOISE, ())
        g.set(self.n["noise"], 0, 4096.0)
        glide_b = node("glide", ag.LAG, (self.note_b, lfo["sine"], None))
        g.set(self.n["glide"], ag.LAG_TIME, 0.0625)
        node("glide_pole", ag.LAG, (self.note_b,))
        g.set(self.n["glide_pole"], ag.LAG_SHAPE, ag.LAG_ONE_POLE)
        g.set(self.n["glide_pole"], ag.LAG_TIME, 0.03125)
        saw_b = node("osc", ag.OSC, (glide_b, lfo["triangle"], None, None))
        g.set(self.n["osc"], ag.OSC_FREQUENCY, 110.0)
        g.set(self.n["osc"], ag.OSC_WAVEFORM, 1)
        g.set(self.n["osc"], ag.OSC_LEVEL, 0.5)
        g.set(self.n["osc"], ag.OSC_FM_DEPTH, 0.125)
        # the voice: its own group, gated, parked for some blocks
        pulse_b = node("osc_voice", ag.OSC, (None, None, lfo["saw"], None), group=0)
        g.set(self.n["osc_voice"], ag.OSC_FREQUENCY, 330.0)
        g.set(self.n["osc_voice"], ag.OSC_WAVEFORM, 2)
        g.set(self.n["osc_voice"], ag.OSC_PWM_DEPTH, 0.25)
        env_b = node("env", ag.ENV, (gate_b,), group=0)
        g.set(self.n["env"], ag.ENV_ATTACK, 0.015625)
        g.set(self.n["env"], ag.ENV_DECAY, 0.125)
        g.set(self.n["env"], ag.ENV_SUSTAIN, 0.5)
        g.set(self.n["env"], ag.ENV_RELEASE, 0.0625)
        voice_b = node("vca", ag.VCA, (pulse_b, env_b, None), group=0)
        g.set(self.n["vca"], ag.VCA_GAIN, 0.75)
        lad_b = node("ladder", ag.LADDER, (saw_b, lfo["sine"]))
        g.set(self.n["ladder"], ag.LADDER_RESONANCE, 0.5)
        g.set(self.n["ladder"], ag.LADDER_CUTOFF_OCTAVES, 1.0)
        bq_b = node("biquad", ag.BIQUAD, (noise_b,))
        g.set(self.n["biquad"], ag.BIQUAD_MODE, 2)
        g.set(self.n["biquad"], ag.BIQUAD_FREQUENCY, 1500.0)
        g.set(self.n["biquad"], ag.BIQUAD_Q, 2.0)
        mix_b = node("mix", ag.MIX, (lad_b, voice_b, bq_b))
        g.set(self.n["mix"], 2, 0.25)
        for name, table in (("delay_shared", SHARED_WOW), ("delay_copy", self.wow_copy),
                            ("delay_new", SHARED_WOW)):
            node(name, ag.FBDELAY, (mix_b,), 40.0)
            d = self.n[name]
            g.set(d, ag.DELAY_MS, 12.5)
            g.set(d, ag.DELAY_FEEDBACK, 0.5)
            g.set(d, ag.DELAY_MIX, 0.5)
            g.set(d, ag.DELAY_DAMPING_HZ, 6000.0)
            g.set(d, ag.DELAY_WOW_HZ, 2.0)
            g.set(d, ag.DELAY_WOW_DEPTH_MS, 2.0)
            g.table(d, table)
        ph_b = node("phaser", ag.PHASER, (saw_b,), 6.0)
        g.set(self.n["phaser"], ag.PHASER_FEEDBACK, 0.5)
        g.set(self.n["phaser"], ag.PHASER_MIX, 0.5)
        node("hold_clock", ag.SAMPLE_HOLD, (noise_b, self.clock_b))
        g.set(self.n["hold_clock"], ag.SH_LAG, 0.125)
        node("hold_rate", ag.SAMPLE_HOLD, (saw_b, None))
        g.set(self.n["hold_rate"], ag.SH_RATE, 750.0)
        node("twopole", ag.TWOPOLE, (saw_b, lfo["square"]))
        g.set(self.n["twopole"], ag.TWOPOLE_RESONANCE, 0.75)
        node("tanh", ag.TANH, (ph_b,))
        g.set(self.n["tanh"], ag.TANH_DRIVE, 4.0)
        fold_b = node("fold", ag.WAVEFOLD, (saw_b, lfo["random"]))
        g.set(self.n["fold"], ag.WAVEFOLD_FOLD, 0.5)
        node("lpg", ag.LPG, (fold_b, None, self.strike_b))
        g.set(self.n["lpg"], ag.LPG_DECAY, 0.125)
        node("modal", ag.MODAL, (noise_b, self.strike_b))
        g.set(self.n["modal"], ag.MODAL_FREQUENCY, 220.0)
        g.set(self.n["modal"], ag.MODAL_MIX, 0.75)
        left_b, right_b = g.buffer(), g.buffer()
        self.n["pan"] = g.add(ag.PAN, ag.ALWAYS,
                              tuple(b for _, b in self.watched[-12:-4]), left_b, right_b)
        self.watched += [("pan_l", left_b), ("pan_r", right_b)]
        for i in range(8):
            g.set(self.n["pan"], ag.PAN_PAN + i, (i - 3.5) / 4.0)
            g.set(self.n["pan"], i, 0.25)
        self.n["out"] = g.add(ag.OUT, ag.ALWAYS, (left_b, right_b), None)
        g.set(self.n["out"], 0, 0.5)
        # last, so a build without it numbers every other node (and seeds its
        # noise) the same
        if HAS_OVERDRIVE:
            node("overdrive", ag.OVERDRIVE, (saw_b,))
            g.set(self.n["overdrive"], ag.OVERDRIVE_DRIVE, 0.75)

        self.pcm = array("h", bytes(4 * FRAMES))
        self.block = array("h", bytes(2 * FRAMES))

    def drive(self, b, role="R"):
        """Hand the graph block ``b``'s changes: sets, resets and written buffers."""
        g, n = self.g, self.n
        if b % 24 in (0, 12):
            g.set(n["gate"], 0, UNIT if b % 24 == 0 else 0.0)
        if b % 3 == 0:
            g.set(n["ladder"], ag.LADDER_CUTOFF_HZ, 250.0 + 125.0 * (b % 16))
        if b % 5 == 0:
            g.set(n["osc_voice"], ag.OSC_FREQUENCY, 220.0 + 27.5 * (b % 7))
        if b == SWAP:
            g.set(n["delay_shared"], ag.DELAY_FEEDBACK, 0.75)
            g.set(n["mix"], 0, 0.5)
            g.set(n["modal"], ag.MODAL_FREQUENCY, 330.0)
            if role != "S":  # the new table comes with the new graph
                g.table(n["delay_new"], self.wow_new)
        if b == 20 or b == SWAP + 10:
            g.reset(n["biquad"])
        if b == 30:
            g.set(n["lfo_random"], ag.LFO_SHAPE, ag.LFO_SINE)
        if b == 36:
            g.set(n["lfo_random"], ag.LFO_SHAPE, ag.LFO_RANDOM)
        note = (0, 4096, -2731, 1365, 9557, -8192)[(b // 6) % 6]
        blk = self.block
        for i in range(FRAMES):
            blk[i] = note
        g.write(self.note_b, blk)
        for i in range(FRAMES):
            blk[i] = UNIT if (i // 24) % 2 == 1 else 0
        g.write(self.clock_b, blk)
        for i in range(FRAMES):
            blk[i] = UNIT if b % 9 == 4 else 0
        g.write(self.strike_b, blk)

    def run(self, b):
        """Render block ``b``: the voice group is parked over blocks 66 to 71."""
        return self.g.run(self.pcm, 0, 0 if 66 <= b < 72 else 1)

    def outputs(self):
        """Every watched buffer's block, and the output's, as lists."""
        got = []
        for name, buf in self.watched:
            self.g.read(buf, self.block)
            got.append((name, list(self.block)))
        got.append(("out", list(self.pcm)))
        return got


def carry_all(dst, src):
    """D takes every one of S's nodes, as a recompiled patch would."""
    for name in src.n:
        dst.g.carry(dst.n[name], src.g, src.n[name])


# -- R, the reference, and S, the source, side by side up to the swap; S's
# block-48 changes queued. Each graph renders as the comparison goes, so
# nothing is kept but the counts (a whole render does not fit a small heap).
r = Patch()
s = Patch()
for b in range(SWAP):
    r.drive(b)
    r.run(b)
    s.drive(b, "S")
    s.run(b)
s.drive(SWAP, "S")

# -- D, built beside it from the same changes, then given S's nodes
d = Patch()
for b in range(SWAP + 1):
    d.drive(b, "D")
# a D that takes nothing, for the comparison's own sake
fresh = Patch()
for b in range(SWAP + 1):
    fresh.drive(b, "D")
if FAULT == "offbeat":
    s.run(SWAP)  # the carry lands a block after S rendered the swap block
carry_all(d, s)

differ = {}
untaken = None
churn = []
for k, b in enumerate(range(SWAP, BLOCKS)):
    if s is not None:
        # S goes on with other inputs: D must read none of S's memory
        s.drive(b + 500, "S")
        s.run(b + 500)
        if k == CHURN:
            s = None
            gc.collect()
            churn = [array("h", [12345] * 256) for _ in range(64)]
    r.drive(b)
    r.run(b)
    if b != SWAP:
        d.drive(b, "D")
        fresh.drive(b, "D")
    d.run(b)
    fresh.run(b)
    want = r.outputs()
    got = d.outputs()
    for (name, a), (_, c) in zip(want, got):
        print("node %d %s %s" % (b, name, " ".join([str(v) for v in c])))
        if a != c:
            differ[name] = differ.get(name, 0) + 1
    if untaken is None:
        untaken = set(name for name, _ in want)
    for (name, a), (_, c) in zip(want, fresh.outputs()):
        if a != c:
            untaken.discard(name)
    want = got = None
del churn

# -- D against R, node by node
for name in sorted(differ):
    print("differs %s %d blocks" % (name, differ[name]))
check(not differ, "the carried graph renders what the reference renders, on every node")

# -- a D that took nothing differs from R wherever state matters
for name in ("osc", "osc_voice", "env", "ladder", "biquad", "delay_shared", "delay_copy",
             "delay_new", "lfo_random", "noise", "glide", "glide_pole", "phaser",
             "hold_clock", "twopole", "lpg", "modal"):
    check(name not in untaken, "%s depends on carried state" % name)

# -- refusals
REFUSALS = (
    ("kind", lambda: _pair(ag.OSC, 0.0, ag.LADDER, 0.0), "kind"),
    ("arg", lambda: _pair(ag.FBDELAY, 20.0, ag.FBDELAY, 30.0), "arg"),
    ("size", lambda: _pair(ag.FBDELAY, 20.0, ag.FBDELAY, 20.0, 44100), "size"),
    ("rate", lambda: _pair(ag.OSC, 0.0, ag.OSC, 0.0, 44100), "rate"),
)


def _pair(kind_a, arg_a, kind_b, arg_b, rate_b=RATE):
    a = ag.Graph(sample_rate=RATE, frames=FRAMES, buffers=2, nodes=2)
    b = ag.Graph(sample_rate=rate_b, frames=FRAMES, buffers=2, nodes=2)
    na = a.add(kind_a, ag.ALWAYS, (None,), a.buffer(), arg_a)
    nb = b.add(kind_b, ag.ALWAYS, (None,), b.buffer(), arg_b)
    a.carry(na, b, nb)


for name, attempt, word in REFUSALS:
    try:
        attempt()
        check(False, "carry refuses a %s mismatch" % name)
        print("refusal %s none" % name)
    except ValueError as e:
        check(word in str(e), "the %s refusal says so: %s" % (name, e))
        print("refusal %s ValueError" % name)
g1 = ag.Graph(frames=FRAMES, buffers=2, nodes=2)
g1.add(ag.CONST, ag.ALWAYS, (), g1.buffer())
for bad in ((5, g1, 0), (0, g1, 5), (-1, g1, 0)):
    try:
        g1.carry(*bad)
        check(False, "carry refuses node %r" % (bad,))
    except ValueError:
        pass
try:
    g1.carry(0, object(), 0)
    check(False, "carry refuses a source that is no graph")
except TypeError:
    pass
g1.carry(0, g1, 0)  # a node onto itself changes nothing

if failures:
    for what in failures:
        print("FAIL " + what)
    print("GRAPH CARRY FAIL")
    sys.exit(1)
print("GRAPH CARRY PASS")
