# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Hold audiograph's STRUCK, PICKUP, TREMOLO, DECAY, PINK, CRUSH, ACID, SPRING, ADSR2 and GLISS against the rack.

    graph_vs_rack.py [--rack PATH]

Imports vrack's own cores from an audioinstruments checkout (read only;
default the sibling ``../audioinstruments/lib``), drives each and the matching
graph node with the same blocks, and prints the worst and rms difference in
counts on the int16 wire (16384 counts to 1.0). Needs numpy. STRUCK is compared
with the rack's output shifted by ``StruckBarCore.LATENCY`` (the graph node has
no 256-frame bridge), leaving out the frames before each strike that the rack
mutes.

DECAY is held to ``OneShotEnvelopeCore`` with no burst table, to the burst
envelopes inside ``HandClapCore``, ``JingleCore`` and ``GuiroCore`` (their
``_env``, with the noise path stood aside), to ``CowbellCore`` and
``Rhythm78Cowbell``'s two-stage ``_env``, to ``MaracasCore``'s linear-attack
``_env``, and to ``BridgedTDrumCore`` and ``SweptOscillatorDrumCore``'s
amp-minus-rise strike, click envelope and pitch-bend envelope. PINK is
``NoiseGenerator.pink``. CRUSH is the quantise in
``BitDepthRateCore._hold_numpy`` — the hold and the anti-alias filter around
it are SAMPLE_HOLD's and a filter node's, so they are stood aside here and the
node is compared to the quantise alone. GLISS is ``GlideCore`` plus poly80's
once-a-block semitone step (``voice.py``'s glissando line, generalised to
``step``).
"""

import math
import sys
from array import array
from pathlib import Path

import numpy as np

import audiograph as ag

RACK = Path(__file__).resolve().parents[1].parent / "audioinstruments" / "lib"
if not RACK.is_dir():
    # a worktree sits one level further down (audiodsp-wt/<name>)
    RACK = Path(__file__).resolve().parents[3] / "audioinstruments" / "lib"
if "--rack" in sys.argv:
    RACK = Path(sys.argv[sys.argv.index("--rack") + 1])
sys.path.insert(0, str(RACK))

from audioinstruments._rack.dsp.keys import PickupCore, StruckBarCore  # noqa: E402
from audioinstruments._rack.dsp.tremolo import TremoloCore  # noqa: E402
from audioinstruments._rack.dsp.drums import (  # noqa: E402
    BitDepthRateCore,
    BridgedTDrumCore,
    CowbellCore,
    GuiroCore,
    HandClapCore,
    JingleCore,
    MaracasCore,
    OneShotEnvelopeCore,
    SweptOscillatorDrumCore,
)
from audioinstruments._rack.dsp.noise import NoiseGenerator  # noqa: E402
from audioinstruments._rack.dsp.ladder import AcidFilterCore  # noqa: E402
from audioinstruments._rack.dsp.reverb import SpringReverbCore  # noqa: E402
from audioinstruments._rack.dsp.envelope import EnvelopeCore  # noqa: E402
from audioinstruments._rack.dsp.glide import GlideCore  # noqa: E402
from audioinstruments._rack.modules.rhythm78.cowbell import (  # noqa: E402
    Rhythm78Cowbell,
)

RATE = 48000
UNIT = ag.UNIT
FRAMES = 128


def wire(block):
    """A rack float block on the int16 wire, rounded half to even."""
    return np.clip(np.rint(np.asarray(block, dtype=np.float64) * UNIT), -32768, 32767).astype(np.int64)


def stats(got, want):
    diff = np.asarray(got, dtype=np.int64) - np.asarray(want, dtype=np.int64)
    return int(np.max(np.abs(diff))), float(math.sqrt(np.mean(diff.astype(np.float64) ** 2))), diff.size


def report(name, got, want):
    worst, rms, count = stats(got, want)
    peak = int(np.max(np.abs(np.asarray(want))))
    print("%-28s worst %d  rms %.4f  over %d samples (rack peak %d)" % (name, worst, rms, count, peak))
    return worst


def tone(b, amplitude=12000, period=47.0):
    return [int(amplitude * math.sin(2.0 * math.pi * (b * FRAMES + i) / period)) for i in range(FRAMES)]


# ------------------------------------------------------------------ TREMOLO --

def tremolo(mode_name, mode):
    g = ag.Graph(sample_rate=RATE, frames=FRAMES, channel_count=1, buffers=4, nodes=2)
    a, left, right = g.buffer(), g.buffer(), g.buffer()
    node = g.add(ag.TREMOLO, ag.ALWAYS, (a,), left, right)
    g.set(node, ag.TREMOLO_MODE, mode)
    core = TremoloCore(RATE, FRAMES)
    src = np.zeros(FRAMES, dtype=np.float32)
    out_l = np.zeros(FRAMES, dtype=np.float32)
    out_r = np.zeros(FRAMES, dtype=np.float32)
    buf = array("h", bytes(2 * FRAMES))
    got_l, got_r, want_l, want_r = [], [], [], []
    for b in range(1500):
        rate = 1.5 + 0.75 * ((b // 40) % 9)
        depth = ((b // 23) % 6) * 0.2
        if b == 900:
            g.reset(node)
            core.reset()
        block = tone(b)
        g.write(a, array("h", block))
        g.set(node, ag.TREMOLO_RATE, rate)
        g.set(node, ag.TREMOLO_DEPTH, depth)
        g.run(None)
        g.read(left, buf)
        got_l += list(buf)
        g.read(right, buf)
        got_r += list(buf)
        src[:] = np.asarray(block, dtype=np.float32) / UNIT
        core.set(rate_hz=float(np.float32(rate)), depth=float(np.float32(depth)), mode=mode_name)
        core.render(src, out_l, out_r)
        want_l += list(wire(out_l))
        want_r += list(wire(out_r))
    w = report("TREMOLO %s left" % mode_name, got_l, want_l)
    return max(w, report("TREMOLO %s right" % mode_name, got_r, want_r))


# ------------------------------------------------------------------- PICKUP --

def pickup(curve, mode):
    g = ag.Graph(sample_rate=RATE, frames=FRAMES, channel_count=1, buffers=4, nodes=2)
    a, out = g.buffer(), g.buffer()
    node = g.add(ag.PICKUP, ag.ALWAYS, (a,), out)
    g.set(node, ag.PICKUP_MODE, mode)
    core = PickupCore(FRAMES, curve=curve)
    src = np.zeros(FRAMES, dtype=np.float32)
    dst = np.zeros(FRAMES, dtype=np.float32)
    tmp = np.zeros(FRAMES, dtype=np.float32)
    buf = array("h", bytes(2 * FRAMES))
    got, want = [], []
    for b in range(400):
        drive = 0.5 + 0.375 * (b % 7)
        offset = ((b % 9) - 4) * 0.0625
        block = tone(b, 14000, 53.0)
        g.write(a, array("h", block))
        g.set(node, ag.PICKUP_DRIVE, drive)
        g.set(node, ag.PICKUP_OFFSET, offset)
        g.run(None)
        g.read(out, buf)
        got += list(buf)
        src[:] = np.asarray(block, dtype=np.float32) / UNIT
        core.render(src, dst, tmp, drive=float(np.float32(drive)), offset=float(np.float32(offset)))
        want += list(wire(dst))
    return report("PICKUP %s" % curve, got, want)


# ------------------------------------------------------------------- STRUCK --

TINE = ((1.0, 1.6, 1.0), (2.0, 0.55, 0.14), (4.0, 0.09, 0.20), (6.27, 0.07, 0.30),
        (10.7, 0.025, 0.12), (15.8, 0.012, 0.05))
CLAVI = tuple((float(k), 1.2 / k, 0.9 / k) for k in range(1, 25))


def struck(name, table, f0, hardness, level, shape_name, shape, blocks=240, every=30, width=8):
    g = ag.Graph(sample_rate=RATE, frames=FRAMES, channel_count=1, buffers=4, nodes=2)
    gate_b, out = g.buffer(), g.buffer()
    node = g.add(ag.STRUCK, ag.ALWAYS, (gate_b,), out)
    g.table(node, table)
    g.set(node, ag.STRUCK_FREQUENCY, f0)
    g.set(node, ag.STRUCK_HARDNESS, hardness)
    g.set(node, ag.STRUCK_LEVEL, level)
    g.set(node, ag.STRUCK_SHAPE, shape)
    core = StruckBarCore(RATE, FRAMES, modes=24, shape=shape_name)
    # the node keeps its table in float32; give the rack the same numbers
    core.tune(float(np.float32(f0)), [tuple(float(np.float32(v)) for v in row) for row in table])
    gate = np.zeros(FRAMES, dtype=np.float32)
    dst = np.zeros(FRAMES, dtype=np.float32)
    buf = array("h", bytes(2 * FRAMES))
    got, want, strikes = [], [], []
    for b in range(blocks):
        on = (b % every) < width
        start = 17 if (b % every) == 0 else 0
        stop = 101 if (b % every) == width - 1 else FRAMES
        level_block = [UNIT if on and start <= i < stop else 0 for i in range(FRAMES)]
        if (b % every) == 0:
            strikes.append(b * FRAMES + start)
        g.write(gate_b, array("h", level_block))
        g.run(None)
        g.read(out, buf)
        got += list(buf)
        gate[:] = np.asarray(level_block, dtype=np.float32) / UNIT
        core.render(gate, dst, hardness=float(np.float32(hardness)), level=float(np.float32(level)),
                    f0=float(np.float32(f0)))
        want += list(wire(dst))
    lat = StruckBarCore.LATENCY
    keep = np.ones(len(got) - lat, dtype=bool)
    for s in strikes:
        keep[max(0, s - lat):s] = False
    g_arr = np.asarray(got[:len(got) - lat])[keep]
    w_arr = np.asarray(want[lat:])[keep]
    return report("STRUCK %s" % name, g_arr, w_arr)


# -------------------------------------------------------------------- DECAY --

# The burst cores' spike tables and their decay constants, as the rack keeps
# them. The node's times are seconds to -40 dB, so a 1/e constant of t becomes
# t * 4.60517 on the way in.
LN_100 = 4.60517


def burst_decay(name, table, burst_1e, tail_1e, tail_40db=None, blocks=40, every=13, width=5):
    """A DECAY node with a burst table against a burst core's own envelope.

    `table` is the core's spike levels at its spike times; `burst_1e` and
    `tail_1e` are the core's exp(-1/(sr*t)) constants in seconds; a `tail_40db`
    is already exp(-4.60517/(sr*t)) and is used as the node's decay directly.
    """
    g = ag.Graph(sample_rate=RATE, frames=FRAMES, channel_count=1, buffers=6, nodes=3)
    gate_b, vel_b, out = g.buffer(), g.buffer(), g.buffer()
    node = g.add(ag.DECAY, ag.ALWAYS, (gate_b, vel_b), out)
    g.table(node, table)
    restrike = burst_1e * LN_100
    decay = tail_40db if tail_40db is not None else tail_1e * LN_100
    g.set(node, ag.DECAY_DECAY, decay)
    g.set(node, ag.DECAY_RESTRIKE, restrike)
    core = {"clap": HANDCLAP, "clap909": HANDCLAP, "jingle": JINGLE,
            "guiro": GUIRO}[name]
    core._decay_burst = math.exp(-1.0 / (RATE * burst_1e))
    if tail_40db is not None:
        core._decay_tail = math.exp(-4.60517 / (RATE * tail_40db))
    else:
        core._decay_tail = math.exp(-1.0 / (RATE * tail_1e))
    core.reset()
    buf = array("h", bytes(2 * FRAMES))
    got, want = [], []
    for b in range(blocks):
        on = (b % every) < width
        # The burst cores start a hit at sample 0 of the block that sees the
        # rising edge, so the hit is put on a block boundary here. The node
        # itself starts it at the rising sample (as OneShotEnvelopeCore does).
        gate = [UNIT if on else 0 for _ in range(FRAMES)]
        vel = [int(UNIT * (0.4 + 0.1 * (b % 5))) for _ in range(FRAMES)]
        g.write(gate_b, array("h", gate))
        g.write(vel_b, array("h", vel))
        g.run(None)
        g.read(out, buf)
        got += list(buf)
        want += list(wire(rack_burst_env(
            name, np.asarray(gate, dtype=np.float32) / UNIT,
            np.asarray(vel, dtype=np.float32) / UNIT)))
    return report("DECAY %s burst" % name, got, want)


def rack_burst_env(name, gate, vel):
    """One burst core's envelope block, with its noise path stood aside.

    ``render`` builds ``_env`` and then multiplies the filtered noise into
    ``dst``, so a scratch ``dst`` leaves ``_env`` standing for the comparison.
    """
    core = {"clap": HANDCLAP, "clap909": HANDCLAP, "jingle": JINGLE,
            "guiro": GUIRO}[name]
    core.render(gate, _SCRATCH, velocity=float(vel[0]))
    return core._env


HANDCLAP = None
JINGLE = None
GUIRO = None
_SCRATCH = np.zeros(FRAMES, dtype=np.float32)


def decay_oneshot(decay_sec, blocks=30, every=11, width=4):
    """A DECAY node with no burst table against OneShotEnvelopeCore."""
    g = ag.Graph(sample_rate=RATE, frames=FRAMES, channel_count=1, buffers=6, nodes=3)
    gate_b, vel_b, out = g.buffer(), g.buffer(), g.buffer()
    node = g.add(ag.DECAY, ag.ALWAYS, (gate_b, vel_b), out)
    g.set(node, ag.DECAY_DECAY, decay_sec)
    core = OneShotEnvelopeCore(RATE, FRAMES)
    core.set_times(0.001, decay_sec)
    buf = array("h", bytes(2 * FRAMES))
    got, want = [], []
    for b in range(blocks):
        on = (b % every) < width
        start = 17 if (b % every) == 0 else 0
        gate = [UNIT if on and i >= start else 0 for i in range(FRAMES)]
        # OneShot's render(gate) starts the hit at 1.0; hold the node's
        # velocity there so the two are the same shape.
        vel = [UNIT] * FRAMES
        g.write(gate_b, array("h", gate))
        g.write(vel_b, array("h", vel))
        g.run(None)
        g.read(out, buf)
        got += list(buf)
        dst = np.zeros(FRAMES, dtype=np.float32)
        core.render(np.asarray(gate, dtype=np.float32) / UNIT, dst)
        want += list(wire(dst))
    return report("DECAY oneshot %.3fs" % decay_sec, got, want)


def decay_node(params, table=None):
    """A DECAY node driven by a gate and a velocity wire."""
    g = ag.Graph(sample_rate=RATE, frames=FRAMES, channel_count=1, buffers=6, nodes=3)
    gate_b, vel_b, out = g.buffer(), g.buffer(), g.buffer()
    node = g.add(ag.DECAY, ag.ALWAYS, (gate_b, vel_b), out)
    if table is not None:
        g.table(node, table)
    for key, val in params.items():
        g.set(node, key, val)
    return g, gate_b, vel_b, out


def one_hit(blocks):
    """Gate and velocity for one strike on sample 0 of block 0, then quiet."""
    gate = [np.zeros(FRAMES, dtype=np.float32) for _ in range(blocks)]
    gate[0][0] = 1.0
    vel = [np.full(FRAMES, 1.0, dtype=np.float32) for _ in range(blocks)]
    return gate, vel


def drive_oneshot(g, gate_b, vel_b, out, env_of, gate, vel, blocks):
    """Run the node and `env_of(gate, vel)` over the same blocks."""
    buf = array("h", bytes(2 * FRAMES))
    got, want = [], []
    for b in range(blocks):
        g.write(gate_b, array("h", (gate[b] * UNIT).astype(np.int16)))
        g.write(vel_b, array("h", (vel[b] * UNIT).astype(np.int16)))
        g.run(None)
        g.read(out, buf)
        got += list(buf)
        want += list(wire(env_of(gate[b], vel[b])))
    return got, want


def rack_silent(core, gate):
    """True when the core's next `render` will take its early return.

    Those cores clear `dst` and leave `_env` (or `_amp_buf`) holding the last
    block they wrote, so a comparison has to stand in zeros of its own.
    """
    return (not core.ringing) and not np.any(gate > 0.1)


def decay_twostage_808():
    """DECAY's two-stage path against ``CowbellCore``'s own ``_env``.

    The rack's fast stage is exp(-1/(sr*0.006)) for 0.015 s, then a tail of
    exp(-6.908/(sr*0.250)). DECAY takes seconds to -40 dB, so the fast stage is
    0.006 * 4.60517 and the tail is 0.250 * 4.60517 / 6.908.
    """
    core = CowbellCore(RATE, FRAMES)
    fast_sec = 0.006 * LN_100
    tail_sec = 0.250 * LN_100 / 6.908
    g, gate_b, vel_b, out = decay_node({
        ag.DECAY_DECAY: tail_sec,
        ag.DECAY_RESTRIKE: tail_sec,
        ag.DECAY_FAST: fast_sec,
        ag.DECAY_FAST_SEC: 0.015,
    })
    dst = np.zeros(FRAMES, dtype=np.float32)

    def env_of(gate, vel):
        early = rack_silent(core, gate)
        core.render(gate, dst, velocity=float(vel[0]))
        return np.zeros(FRAMES) if early else core._env

    gate, vel = one_hit(100)
    got, want = drive_oneshot(g, gate_b, vel_b, out, env_of, gate, vel, 100)
    return report("DECAY cowbell808 two-stage", got, want)


def decay_twostage_78():
    """DECAY's two-stage path against ``Rhythm78Cowbell``'s own ``_env``.

    The rack's fast stage is exp(-1/(sr*0.011)) for 0.012 s, then a tail of
    exp(-4.60517/(sr*0.150)).
    """
    mod = Rhythm78Cowbell(sample_rate=RATE, block_size=FRAMES)
    fast_sec = 0.011 * LN_100
    tail_sec = 0.150
    g, gate_b, vel_b, out = decay_node({
        ag.DECAY_DECAY: tail_sec,
        ag.DECAY_RESTRIKE: tail_sec,
        ag.DECAY_FAST: fast_sec,
        ag.DECAY_FAST_SEC: 0.012,
    })

    def env_of(gate, vel):
        early = (not mod.ringing) and not np.any(gate > 0.1)
        np.copyto(mod.gate_in._silent_buffer, gate)
        np.copyto(mod.vel_in._silent_buffer, vel)
        mod.process(FRAMES)
        return np.zeros(FRAMES) if early else mod._env

    gate, vel = one_hit(100)
    got, want = drive_oneshot(g, gate_b, vel_b, out, env_of, gate, vel, 100)
    return report("DECAY cowbell78 two-stage", got, want)


def decay_maracas():
    """DECAY's linear attack against ``MaracasCore``'s own ``_env``.

    The rack ramps 0 to velocity over 0.008 s, then falls with
    exp(-4.60517/(sr*0.045)).
    """
    core = MaracasCore(RATE, FRAMES)
    g, gate_b, vel_b, out = decay_node({
        ag.DECAY_DECAY: 0.045,
        ag.DECAY_RESTRIKE: 0.045,
        ag.DECAY_ATTACK: 0.008,
    })
    dst = np.zeros(FRAMES, dtype=np.float32)

    def env_of(gate, vel):
        early = rack_silent(core, gate)
        core.render(gate, dst, velocity=float(vel[0]))
        return np.zeros(FRAMES) if early else core._env

    gate, vel = one_hit(60)
    got, want = drive_oneshot(g, gate_b, vel_b, out, env_of, gate, vel, 60)
    return report("DECAY maracas attack", got, want)


def decay_bridged_t_rise():
    """DECAY's rise against ``BridgedTDrumCore``'s amp-minus-rise strike.

    The rack adds velocity * 0.20 to amp and to rise, then subtracts the rise
    (a 1/e fall over min(0.001, 0.5/f0) s) from the amp (a -40 dB fall over
    max(0.01, decay) s). One hit only: the rack adds on a retrigger and DECAY
    replaces.
    """
    core = BridgedTDrumCore(RATE, FRAMES)
    core.decay = 0.12
    core.click_level = 0.0
    core.noise_mix = 0.0
    core.pitch_drop_cents = 0.0
    f0 = max(10.0, min(RATE * 0.45, core.frequency))
    rise_sec = min(0.001, 0.5 / f0) * LN_100
    g, gate_b, vel_b, out = decay_node({
        ag.DECAY_DECAY: max(0.01, core.decay),
        ag.DECAY_RESTRIKE: max(0.01, core.decay),
        ag.DECAY_RISE: rise_sec,
    })
    dst = np.zeros(FRAMES, dtype=np.float32)

    def env_of(gate, vel):
        # The rack's step is velocity * 0.20; DECAY's level is its velocity.
        early = rack_silent(core, gate)
        core.render(gate, dst, velocity=float(vel[0]) / 0.20)
        return np.zeros(FRAMES) if early else core._amp_buf

    gate, vel = one_hit(100)
    got, want = drive_oneshot(g, gate_b, vel_b, out, env_of, gate, vel, 100)
    return report("DECAY bridged-T rise", got, want)


def decay_swept_rise():
    """DECAY's rise against ``SweptOscillatorDrumCore``'s amp-minus-rise strike."""
    core = SweptOscillatorDrumCore(RATE, FRAMES)
    core.decay = 0.12
    core.click_level = 0.0
    core.noise_mix = 0.0
    core.pitch_drop_cents = 0.0
    f0 = max(10.0, min(RATE * 0.45, core.frequency))
    rise_sec = min(0.001, 0.5 / f0) * LN_100
    g, gate_b, vel_b, out = decay_node({
        ag.DECAY_DECAY: max(0.01, core.decay),
        ag.DECAY_RESTRIKE: max(0.01, core.decay),
        ag.DECAY_RISE: rise_sec,
    })
    dst = np.zeros(FRAMES, dtype=np.float32)

    def env_of(gate, vel):
        early = rack_silent(core, gate)
        core.render(gate, dst, velocity=float(vel[0]))
        return np.zeros(FRAMES) if early else core._amp_buf

    gate, vel = one_hit(100)
    got, want = drive_oneshot(g, gate_b, vel_b, out, env_of, gate, vel, 100)
    return report("DECAY swept-osc rise", got, want)


def decay_click(label, core, core_velocity, click_sec):
    """DECAY against a rack drum's click envelope.

    The rack writes click_env into its ``_tmp`` scratch and then reuses that
    scratch, so the block is caught as its own ``exp_segment`` writes it.
    """
    import audioinstruments._rack.dsp.drums as drums_mod
    g, gate_b, vel_b, out = decay_node({
        ag.DECAY_DECAY: click_sec,
        ag.DECAY_RESTRIKE: click_sec,
    })
    caught = []
    orig = drums_mod.exp_segment

    def spy(dst, start, target, coeff, ramp):
        val = orig(dst, start, target, coeff, ramp)
        if dst is core._tmp:
            caught.append(np.array(dst, dtype=np.float64).copy())
        return val

    drums_mod.exp_segment = spy
    try:
        blocks = 20
        gate, _ = one_hit(blocks)
        buf = array("h", bytes(2 * FRAMES))
        got, want = [], []
        dst = np.zeros(FRAMES, dtype=np.float32)
        for b in range(blocks):
            g.write(gate_b, array("h", (gate[b] * UNIT).astype(np.int16)))
            g.write(vel_b, array("h", [UNIT] * FRAMES))
            g.run(None)
            g.read(out, buf)
            got += list(buf)
            before = len(caught)
            core.render(gate[b], dst, velocity=core_velocity)
            block = np.zeros(FRAMES, dtype=np.float64)
            if len(caught) > before:
                block[:len(caught[-1])] = caught[-1]
            want += list(wire(block))
    finally:
        drums_mod.exp_segment = orig
    if not caught:
        raise RuntimeError("%s: no click block captured" % label)
    return report(label, got, want)


def decay_bend(label, core, bend_sec, which):
    """DECAY against a rack drum's pitch-bend envelope."""
    g, gate_b, vel_b, out = decay_node({
        ag.DECAY_DECAY: bend_sec,
        ag.DECAY_RESTRIKE: bend_sec,
    })
    blocks = 40
    gate, vel = one_hit(blocks)
    buf = array("h", bytes(2 * FRAMES))
    got, want = [], []
    dst = np.zeros(FRAMES, dtype=np.float32)
    for b in range(blocks):
        g.write(gate_b, array("h", (gate[b] * UNIT).astype(np.int16)))
        g.write(vel_b, array("h", [UNIT] * FRAMES))
        g.run(None)
        g.read(out, buf)
        got += list(buf)
        core.render(gate[b], dst, velocity=1.0)
        want += list(wire(np.asarray(getattr(core, which), dtype=np.float64)))
    return report(label, got, want)


# --------------------------------------------------------------------- PINK --

def pink(blocks=40):
    g = ag.Graph(sample_rate=RATE, frames=FRAMES, channel_count=1, buffers=6, nodes=3)
    white_b, out = g.buffer(), g.buffer()
    g.add(ag.PINK, ag.ALWAYS, (white_b,), out)
    gen = NoiseGenerator(seed=0xBEEF)
    buf = array("h", bytes(2 * FRAMES))
    got, want = [], []
    white_f = np.zeros(FRAMES, dtype=np.float32)
    pink_f = np.zeros(FRAMES, dtype=np.float32)
    rng = np.random.default_rng(7)
    for _ in range(blocks):
        # the rack's white is uniform in ±0.5, which is ±8192 counts
        w = (rng.random(FRAMES, dtype=np.float64) - 0.5)
        block = [int(round(x * UNIT)) for x in w]
        g.write(white_b, array("h", block))
        g.run(None)
        g.read(out, buf)
        got += list(buf)
        white_f[:] = np.asarray(block, dtype=np.float32) / UNIT
        gen.pink(white_f, pink_f)
        want += list(wire(pink_f))
    return report("PINK kellet", got, want)


# -------------------------------------------------------------------- CRUSH --

def crush(bits, blocks=25):
    """CRUSH against BitDepthRateCore's own quantise, hold and filter stood aside."""
    g = ag.Graph(sample_rate=RATE, frames=FRAMES, channel_count=1, buffers=6, nodes=3)
    audio_b, out = g.buffer(), g.buffer()
    node = g.add(ag.CRUSH, ag.ALWAYS, (audio_b,), out)
    g.set(node, ag.CRUSH_BITS, bits)
    core = BitDepthRateCore(RATE, FRAMES)
    core.bits = bits
    core.rate = float(RATE)
    core._update_anti_alias()
    # The anti-alias filter and the hold are other nodes; leave only the
    # quantise that _hold_numpy runs on the wire.
    core.anti_alias.process = lambda src, dst: np.copyto(dst, src)
    core._hold = lambda src, dst: np.copyto(dst, src)
    buf = array("h", bytes(2 * FRAMES))
    got, want = [], []
    dst = np.zeros(FRAMES, dtype=np.float32)
    for b in range(blocks):
        block = tone(b, 15000, 31.0 + (b % 5))
        g.write(audio_b, array("h", block))
        g.run(None)
        g.read(out, buf)
        got += list(buf)
        src = np.asarray(block, dtype=np.float32) / UNIT
        core.process(src, dst)
        want += list(wire(dst))
    return report("CRUSH %g bits" % bits, got, want)


# --------------------------------------------------------------------- ACID --

def acid(cutoff_hz, resonance, env_mod, blocks=60):
    """ACID against AcidFilterCore, with an envelope and accent bursts."""
    g = ag.Graph(sample_rate=RATE, frames=FRAMES, channel_count=1, buffers=8, nodes=3)
    audio_b, env_b, acc_b, out = g.buffer(), g.buffer(), g.buffer(), g.buffer()
    node = g.add(ag.ACID, ag.ALWAYS, (audio_b, env_b, acc_b), out)
    g.set(node, ag.ACID_CUTOFF_HZ, cutoff_hz)
    g.set(node, ag.ACID_RESONANCE, resonance)
    g.set(node, ag.ACID_ENVMOD, env_mod)
    core = AcidFilterCore(RATE)
    buf = array("h", bytes(2 * FRAMES))
    got, want = [], []
    src = np.zeros(FRAMES, dtype=np.float64)
    dst = np.zeros(FRAMES, dtype=np.float64)
    env_f = np.zeros(FRAMES, dtype=np.float64)
    acc_f = np.zeros(FRAMES, dtype=np.float64)
    for b in range(blocks):
        block = tone(b, 9000, 47.0)
        env_block = [int(UNIT * min(1.0, (b % 30) / 15.0))] * FRAMES
        acc_block = [int(UNIT * 0.9)] * FRAMES if b % 14 == 0 else [0] * FRAMES
        g.write(audio_b, array("h", block))
        g.write(env_b, array("h", env_block))
        g.write(acc_b, array("h", acc_block))
        g.run(None)
        g.read(out, buf)
        got += list(buf)
        src[:] = np.asarray(block, dtype=np.float64) / UNIT
        env_f[:] = np.asarray(env_block, dtype=np.float64) / UNIT
        acc_f[:] = np.asarray(acc_block, dtype=np.float64) / UNIT
        core.render(FRAMES, src, dst, cutoff_hz, resonance, env_mod, env_f, acc_f)
        want += list(wire(dst))
    return report("ACID %.0fHz r%.2f e%.2f" % (cutoff_hz, resonance, env_mod), got, want)


# ------------------------------------------------------------------- SPRING --

def spring(mix, decay, damp=0.55, blocks=80):
    """SPRING against SpringReverbCore: two Freeverb tanks, dry/wet mixed."""
    g = ag.Graph(sample_rate=RATE, frames=FRAMES, channel_count=1, buffers=4, nodes=2)
    a, left_b, right_b = g.buffer(), g.buffer(), g.buffer()
    node = g.add(ag.SPRING, ag.ALWAYS, (a,), left_b, right_b)
    g.set(node, ag.SPRING_MIX, mix)
    g.set(node, ag.SPRING_DECAY, decay)
    g.set(node, ag.SPRING_DAMP, damp)
    core = SpringReverbCore(RATE, FRAMES)
    core.left.set(damp=damp)
    core.right.set(damp=damp)
    src = np.zeros(FRAMES, dtype=np.float64)
    out_l = np.zeros(FRAMES, dtype=np.float64)
    out_r = np.zeros(FRAMES, dtype=np.float64)
    buf = array("h", bytes(2 * FRAMES))
    got_l, got_r, want_l, want_r = [], [], [], []
    for b in range(blocks):
        block = tone(b, 6000, 41.0)
        if b == 0:
            block[0] = 16000
        g.write(a, array("h", block))
        g.run(None)
        g.read(left_b, buf)
        got_l += list(buf)
        g.read(right_b, buf)
        got_r += list(buf)
        src[:] = np.asarray(block, dtype=np.float64) / UNIT
        core.render(src, out_l, out_r, mix=mix, decay=decay)
        want_l += list(wire(out_l))
        want_r += list(wire(out_r))
    w = report("SPRING m%.2f d%.2f left" % (mix, decay), got_l, want_l)
    return max(w, report("SPRING m%.2f d%.2f right" % (mix, decay), got_r, want_r))


# ------------------------------------------------------------------- ADSR2 --

def adsr2(initial, attack_level, sustain, attack=0.01, decay=0.2, release=0.3,
          blocks=60):
    """ADSR2 against EnvelopeCore, remapped the way poly80 remaps its fenv."""
    g = ag.Graph(sample_rate=RATE, frames=FRAMES, channel_count=1, buffers=4, nodes=2)
    gate_b, out = g.buffer(), g.buffer()
    node = g.add(ag.ADSR2, ag.ALWAYS, (gate_b,), out)
    g.set(node, ag.ADSR2_ATTACK, attack)
    g.set(node, ag.ADSR2_DECAY, decay)
    g.set(node, ag.ADSR2_SUSTAIN, sustain)
    g.set(node, ag.ADSR2_RELEASE, release)
    g.set(node, ag.ADSR2_INITIAL, initial)
    g.set(node, ag.ADSR2_ATTACK_LEVEL, attack_level)
    core = EnvelopeCore(RATE, FRAMES)
    core.set_times(attack, decay, sustain, release)
    span = attack_level - initial
    dst = np.zeros(FRAMES, dtype=np.float64)
    gate_f = np.zeros(FRAMES, dtype=np.float64)
    buf = array("h", bytes(2 * FRAMES))
    got, want = [], []
    for b in range(blocks):
        gate = [UNIT if (b % 10) < 4 else 0] * FRAMES
        g.write(gate_b, array("h", gate))
        g.run(None)
        g.read(out, buf)
        got += list(buf)
        gate_f[:] = np.asarray(gate, dtype=np.float64) / UNIT
        core.render(gate_f, dst)
        want += list(wire(initial + dst * span))
    return report("ADSR2 il%.2f al%.2f s%.2f" % (initial, attack_level, sustain),
                  got, want)


# ------------------------------------------------------------------- GLISS --

def gliss(time, step=1.0, blocks=60):
    """GLISS against GlideCore plus poly80's once-a-block semitone step.

    GlideCore runs in octaves (the graph's own pitch unit, UNIT counts to
    one). poly80 quantises `round(glided[-1] * 120.0) / 120.0` in its CV,
    where a unit is 120 semitones; in octaves that is a grid of `step / 12`,
    which at one semitone a step is voice.py's own line. The bend rides on
    top unslid, as GlideCore documents.
    """
    g = ag.Graph(sample_rate=RATE, frames=FRAMES, channel_count=1, buffers=5, nodes=2)
    note_b, pitch_b, legato_b, out = g.buffer(), g.buffer(), g.buffer(), g.buffer()
    node = g.add(ag.GLISS, ag.ALWAYS, (note_b, pitch_b, legato_b), out)
    g.set(node, ag.GLISS_TIME, time)
    g.set(node, ag.GLISS_MODE, ag.LAG_ON)
    g.set(node, ag.GLISS_STEP, step)
    core = GlideCore(RATE, FRAMES)
    core.mode = GlideCore.ON
    core.time = time
    buf = array("h", bytes(2 * FRAMES))
    got, want = [], []
    notes = (0, 0, 4096, 4096, 4096, -2731, -2731, 1365, 6827, 6827, 6827, -8192)
    grid = step / 12.0
    dst = np.zeros(FRAMES, dtype=np.float32)
    note_f = np.zeros(FRAMES, dtype=np.float32)
    pitch_f = np.zeros(FRAMES, dtype=np.float32)
    for b in range(blocks):
        n = notes[(b // 3) % len(notes)]
        note = [n] * FRAMES
        bend = [((b * FRAMES + i) * 7) % 801 - 400 for i in range(FRAMES)]
        pitch = [v + d for v, d in zip(note, bend)]
        legato = [UNIT if b % 3 else 0] * FRAMES
        g.write(note_b, array("h", note))
        g.write(pitch_b, array("h", pitch))
        g.write(legato_b, array("h", legato))
        g.run(None)
        g.read(out, buf)
        got += list(buf)
        note_f[:] = np.asarray(note, dtype=np.float32) / UNIT
        pitch_f[:] = np.asarray(pitch, dtype=np.float32) / UNIT
        core.process(dst, note_f, legato=float(legato[0]) / UNIT)
        # poly80's glissando at one semitone a step, generalised to `step`
        held = round(float(dst[-1]) / grid) * grid
        want += list(wire(held + pitch_f - note_f))
    return report("GLISS t%.4f s%.2f" % (time, step), got, want)


worst = 0
worst = max(worst, tremolo("pan", ag.TREMOLO_PAN))
worst = max(worst, tremolo("amplitude", ag.TREMOLO_AMPLITUDE))
worst = max(worst, pickup("electromagnetic", ag.PICKUP_ELECTROMAGNETIC))
worst = max(worst, pickup("electrostatic", ag.PICKUP_ELECTROSTATIC))
worst = max(worst, struck("tine hammer", TINE, 261.6256, 0.5, 0.8, "hammer", ag.STRUCK_HAMMER))
worst = max(worst, struck("tine hard high", TINE, 1760.0, 0.9, 0.7, "hammer", ag.STRUCK_HAMMER))
worst = max(worst, struck("24-mode pluck", CLAVI, 330.0, 0.65, 0.85, "pluck", ag.STRUCK_PLUCK))

HANDCLAP = HandClapCore(RATE, FRAMES)
JINGLE = JingleCore(RATE, FRAMES)
GUIRO = GuiroCore("short", RATE, FRAMES)
worst = max(worst, decay_oneshot(0.055))
worst = max(worst, decay_oneshot(0.2))
worst = max(worst, decay_oneshot(1.2))
# HandClapCore's spikes at int(0.0105*sr) and its exp(-1/(sr*t)) constants.
CLAP_SPIKES = ((0.0, 0.75), (0.0105, 0.75), (0.0210, 0.75), (0.0315, 0.85))
worst = max(worst, burst_decay("clap", CLAP_SPIKES, 0.0028, 0.065))
worst = max(worst, burst_decay("clap909", CLAP_SPIKES, 0.0028, 0.090))
JINGLE_SPIKES = ((0.0, 0.8), (0.016, 0.8), (0.032, 0.8), (0.048, 1.0))
worst = max(worst, burst_decay("jingle", JINGLE_SPIKES, 0.005, 0.220, tail_40db=0.220))
# GuiroCore short: six spikes at round(i*0.016*sr), levels 0.6..1.0.
GUIRO_SPIKES = tuple((i * 0.080 / 5.0, 0.6 + 0.4 * i / 5.0) for i in range(6))
worst = max(worst, burst_decay("guiro", GUIRO_SPIKES, 0.0035, 0.0080))
worst = max(worst, decay_twostage_808())
worst = max(worst, decay_twostage_78())
worst = max(worst, decay_maracas())
worst = max(worst, decay_bridged_t_rise())
worst = max(worst, decay_swept_rise())
_bridged_t = BridgedTDrumCore(RATE, FRAMES)
_bridged_t.click_level = 0.25
worst = max(worst, decay_click("DECAY bridged-T click", _bridged_t, 1.0,
                               0.002 * LN_100))
_swept = SweptOscillatorDrumCore(RATE, FRAMES)
_swept.accent = 0.0
_swept.click_level = 0.35
worst = max(worst, decay_click("DECAY swept-osc click", _swept, 1.0,
                               0.002 * LN_100))
_bend = BridgedTDrumCore(RATE, FRAMES)
_bend.pitch_drop_cents = 232.0
_bend.click_level = 0.0
_bend.noise_mix = 0.0
worst = max(worst, decay_bend("DECAY bridged-T bend", _bend,
                              _bend.bend_time_sec * LN_100, "_bend_buf"))
_sweep = SweptOscillatorDrumCore(RATE, FRAMES)
_sweep.pitch_drop_cents = 2400.0
_sweep.click_level = 0.0
_sweep.noise_mix = 0.0
worst = max(worst, decay_bend("DECAY swept-osc sweep", _sweep,
                              _sweep.sweep_time_sec * LN_100, "_sweep_buf"))
worst = max(worst, pink())
worst = max(worst, crush(4))
worst = max(worst, crush(6))
worst = max(worst, crush(8))
worst = max(worst, crush(15))
worst = max(worst, crush(16))
worst = max(worst, acid(400.0, 0.7, 0.6))
worst = max(worst, acid(180.0, 0.0, 0.0))
worst = max(worst, acid(1200.0, 0.9, 0.9))
worst = max(worst, spring(0.25, 0.75))
worst = max(worst, spring(0.5, 0.4))
worst = max(worst, spring(0.0, 0.9))
worst = max(worst, adsr2(0.0, 1.0, 0.7))
worst = max(worst, adsr2(-0.5, 0.5, 0.5))
# poly80's own fenv: sustain level -il/(al-il) puts the output at 0
worst = max(worst, adsr2(-0.4, 0.8, 0.4 / 1.2, attack=0.05, decay=0.3, release=0.4))
worst = max(worst, gliss(0.015625, 1.0))
worst = max(worst, gliss(0.03125, 1.0))
worst = max(worst, gliss(0.0625, 2.0))
print("worst over all: %d" % worst)
