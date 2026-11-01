# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Hold audiograph's STRUCK, PICKUP and TREMOLO against the rack's own Python.

    graph_vs_rack.py [--rack PATH]

Imports vrack's ``StruckBarCore``, ``PickupCore`` and ``TremoloCore`` from an
audioinstruments checkout (read only; default the sibling
``../audioinstruments/lib``), drives each and the matching graph node with the
same blocks, and prints the worst and rms difference in counts on the int16
wire (16384 counts to 1.0). Needs numpy. STRUCK is compared with the rack's
output shifted by ``StruckBarCore.LATENCY`` (the graph node has no 256-frame
bridge), leaving out the frames before each strike that the rack mutes.
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


worst = 0
worst = max(worst, tremolo("pan", ag.TREMOLO_PAN))
worst = max(worst, tremolo("amplitude", ag.TREMOLO_AMPLITUDE))
worst = max(worst, pickup("electromagnetic", ag.PICKUP_ELECTROMAGNETIC))
worst = max(worst, pickup("electrostatic", ag.PICKUP_ELECTROSTATIC))
worst = max(worst, struck("tine hammer", TINE, 261.6256, 0.5, 0.8, "hammer", ag.STRUCK_HAMMER))
worst = max(worst, struck("tine hard high", TINE, 1760.0, 0.9, 0.7, "hammer", ag.STRUCK_HAMMER))
worst = max(worst, struck("24-mode pluck", CLAVI, 330.0, 0.65, 0.85, "pluck", ag.STRUCK_PLUCK))
print("worst over all: %d" % worst)
