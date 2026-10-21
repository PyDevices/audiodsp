"""audiograph: a graph renders what its nodes are wired to render, in one call.

    graph_probe.py                   # PASS, or FAIL and exit 1
    graph_probe.py --fault wiring    # a node runs before its source: must FAIL
    graph_probe.py --fault dropped   # a node left out of the chain: must FAIL
    graph_probe.py --fault ignored   # a parameter set where no node reads it: must FAIL

It runs unchanged on CPython, MicroPython and CircuitPython, and needs
nothing but the module and ``array``. verify_graph.py is the comparison
between them.

What it holds the runner to:

- a value written into a buffer reaches the output in the same run, through
  as many nodes as sit between them, so a graph in run order never lags;
- a node in the chain changes the sound (the ladder closes a sawtooth), and
  a ``set()`` after the first run reaches the node it names and ``get()``
  reads it back;
- an envelope's group reports sounding while it sounds and not after, and a
  group masked out of a run goes silent at the mixer that reads it;
- a mixer gain that moved ramps across the block rather than stepping;
- the output node writes the block it is asked for, stereo interleaved or
  mono as the mean, and refuses a buffer too small for it;
- a graph refuses what it cannot run: too many frames, a bad buffer id, a
  node with nothing to write, too many inputs, a full table.

Every planted fault is a wiring or a parameter mistake a caller could make
and the runner must not hide: ``wiring`` adds a mixer before the node it
reads, ``dropped`` wires the VCA to the oscillator instead of the filter, and
``ignored`` sends the cutoff to a filter no output reads. Each has to fail.
"""

import sys
from array import array

try:
    import audiograph as ag
except ImportError:
    print("GRAPH SKIPPED: no audiograph here")
    sys.exit(0)

FAULT = None
if "--fault" in sys.argv:
    FAULT = sys.argv[sys.argv.index("--fault") + 1]
    if FAULT not in ("wiring", "dropped", "ignored"):
        print("unknown fault %r" % FAULT)
        sys.exit(2)

RATE = 48000
FRAMES = 128
UNIT = ag.UNIT
failures = []


def check(ok, what):
    if not ok:
        failures.append(what)


def digest(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def ints(n):
    return array("h", [0] * n)


def peak(x):
    top = 0
    for v in x:
        if v < 0:
            v = -v
        if v > top:
            top = v
    return top


def brightness(x):
    """RMS of the second difference over the RMS level: a sawtooth's resets make it
    large, and a low-pass that rounds them off makes it small. Zero for silence."""
    level = 0
    curve = 0
    for i in range(1, len(x) - 1):
        a = x[i]
        level += a * a
        d = x[i + 1] - 2 * a + x[i - 1]
        curve += d * d
    return (curve / level) ** 0.5 if level else 0.0


def new_graph(**kw):
    kw.setdefault("sample_rate", RATE)
    kw.setdefault("frames", FRAMES)
    return ag.Graph(**kw)


def read(g, buffer):
    dst = ints(FRAMES)
    g.read(buffer, dst)
    return dst


# -- 1. a value reaches the output in the run it was set --------------------------

def check_wiring():
    g = new_graph(channel_count=1, buffers=8, nodes=8)
    src, a, b = g.buffer(), g.buffer(), g.buffer()
    if FAULT == "wiring":
        # the first mixer runs before the constant it reads: one block stale
        g.add(ag.MIX, ag.ALWAYS, (src,), a)
        const = g.add(ag.CONST, ag.ALWAYS, (), src)
    else:
        const = g.add(ag.CONST, ag.ALWAYS, (), src)
        g.add(ag.MIX, ag.ALWAYS, (src,), a)
    g.add(ag.MIX, ag.ALWAYS, (a,), b)
    g.add(ag.OUT, ag.ALWAYS, (b,), None)
    out = ints(FRAMES)
    for k in range(1, 6):
        g.set(const, 0, 100.0 * k)
        g.run(out, 0)
        check(out[0] == 100 * k and out[FRAMES - 1] == 100 * k,
              "wiring: block %d carries %d, not %d" % (k, out[0], 100 * k))


# -- 2. the filter in the chain closes the tone, and set() reaches it -----------

def voice(cutoff_hz):
    """saw -> ladder -> VCA at full gain -> stereo out; (graph, ladder node, output)."""
    g = new_graph(channel_count=2, buffers=8, nodes=8)
    osc_b, vcf_b, cv_b, vca_b, spare_b = (g.buffer() for _ in range(5))
    osc = g.add(ag.OSC, ag.ALWAYS, (None, None, None, None), osc_b)
    g.set(osc, ag.OSC_WAVEFORM, 1)
    g.set(osc, ag.OSC_FREQUENCY, 220.0)
    g.set(osc, ag.OSC_LEVEL, 0.5)
    vcf = g.add(ag.LADDER, ag.ALWAYS, (osc_b, None), vcf_b)
    g.set(vcf, ag.LADDER_CUTOFF_HZ, cutoff_hz)
    cv = g.add(ag.CONST, ag.ALWAYS, (), cv_b)
    g.set(cv, 0, UNIT)
    audio = osc_b if FAULT == "dropped" else vcf_b
    g.add(ag.VCA, ag.ALWAYS, (audio, cv_b, None), vca_b)
    g.add(ag.OUT, ag.ALWAYS, (vca_b, vca_b), None)
    spare = g.add(ag.LADDER, ag.ALWAYS, (None, None), spare_b)
    if FAULT == "ignored":
        vcf = spare  # the cutoff goes to a filter nothing reads
    return g, vcf


def render(g, blocks, skip=2):
    """Blocks of the left channel after ``skip`` settling blocks, and the digest of all."""
    out = ints(FRAMES * 2 * blocks)
    for k in range(blocks):
        g.run(out, k)
    left = array("h", [out[2 * i] for i in range(FRAMES * skip, FRAMES * blocks)])
    return left, digest(bytes(out))


def check_filter():
    g_open, _ = voice(8000.0)
    g_shut, _ = voice(150.0)
    bright, d1 = render(g_open, 10)
    dark, d2 = render(g_shut, 10)
    print("graph digest open_voice", d1)
    print("graph digest shut_voice", d2)
    check(peak(bright) > 1000, "filter: the open voice is silent (peak %d)" % peak(bright))
    check(peak(dark) > 100, "filter: the shut voice is silent (peak %d)" % peak(dark))
    ratio = brightness(bright) / max(brightness(dark), 1e-9)
    check(ratio > 10.0, "filter: closing the cutoff from 8 kHz to 150 Hz changed the brightness %.2fx" % ratio)

    g, vcf = voice(8000.0)
    before, _ = render(g, 8)
    g.set(vcf, ag.LADDER_CUTOFF_HZ, 150.0)
    after, d3 = render(g, 8)
    print("graph digest moved_voice", d3)
    ratio = brightness(before) / max(brightness(after), 1e-9)
    check(ratio > 10.0, "set: the cutoff moved to 150 Hz after 8 blocks changed the brightness %.2fx" % ratio)
    _, chain = voice(8000.0)  # the node in the chain, whatever the fault did
    got = g.get(chain, ag.LADDER_CUTOFF_HZ)
    check(abs(got - 150.0) < 1e-3, "get: the chain's cutoff reads %.1f after set(150)" % got)


# -- 3. envelopes, sounding groups, and a masked group's silence ----------------

def check_groups():
    g = new_graph(channel_count=1, buffers=8, nodes=8)
    gate_b, env_b, k_b, mix_b = (g.buffer() for _ in range(4))
    gate = g.add(ag.CONST, 0, (), gate_b)
    env = g.add(ag.ENV, 0, (gate_b,), env_b)
    g.set(env, ag.ENV_ATTACK, 0.005)
    g.set(env, ag.ENV_DECAY, 0.01)
    g.set(env, ag.ENV_SUSTAIN, 0.5)
    g.set(env, ag.ENV_RELEASE, 0.02)
    k = g.add(ag.CONST, 1, (), k_b)
    g.set(k, 0, 1000.0)
    g.add(ag.MIX, ag.ALWAYS, (env_b, k_b), mix_b)
    out = ints(FRAMES)

    check(g.run(None, 0, 0b11) == 0, "groups: an idle envelope reports sounding")
    g.set(gate, 0, UNIT)
    sounding = g.run(None, 0, 0b11)
    check(sounding & 1, "groups: a gated envelope does not report its group")
    for _ in range(20):
        g.run(None, 0, 0b11)
    level = read(g, env_b)
    check(abs(level[FRAMES - 1] - 0.5 * UNIT) < 0.02 * UNIT,
          "envelope: sustain reads %d, not %d" % (level[FRAMES - 1], int(0.5 * UNIT)))
    mix = read(g, mix_b)
    check(mix[FRAMES - 1] == level[FRAMES - 1] + 1000, "mixer: env + 1000 reads %d" % mix[FRAMES - 1])

    # mask group 1 out: its constant is zeroed once, and the mixer hears silence from it
    g.run(None, 0, 0b01)
    mix = read(g, mix_b)
    level = read(g, env_b)
    check(mix[FRAMES - 1] == level[FRAMES - 1], "groups: a masked group still reaches the mixer (%d vs %d)" % (mix[FRAMES - 1], level[FRAMES - 1]))
    g.run(None, 0, 0b11)
    mix = read(g, mix_b)
    check(mix[FRAMES - 1] == level[FRAMES - 1] + 1000, "groups: a group enabled again stays silent")

    g.set(gate, 0, 0.0)
    for n in range(200):
        if not g.run(None, 0, 0b11) & 1:
            break
    else:
        n = None
    check(n is not None and 2 < n < 100, "envelope: release ended after %s blocks" % n)
    g.run(None, 0, 0b11)
    check(peak(read(g, env_b)) == 0, "envelope: not silent once idle")
    g.set(gate, 0, UNIT)
    g.run(out, 0, 0b11)
    check(g.run(None, 0, 0b11) & 1, "envelope: a second gate does not retrigger")
    g.reset(env)
    check(g.run(None, 0, 0b11) & 1, "envelope: reset with the gate held does not restart")


# -- 4. a moved mixer gain ramps across the block --------------------------------

def check_ramp():
    g = new_graph(channel_count=1, buffers=4, nodes=4)
    src, dst = g.buffer(), g.buffer()
    c = g.add(ag.CONST, ag.ALWAYS, (), src)
    g.set(c, 0, 10000.0)
    mix = g.add(ag.MIX, ag.ALWAYS, (src,), dst)
    g.run(None)
    check(read(g, dst)[FRAMES - 1] == 10000, "mixer: unity gain is not a wire")
    g.set(mix, 0, 0.0)
    g.run(None)
    ramp = read(g, dst)
    check(ramp[0] > 9000 and ramp[FRAMES - 1] == 0,
          "ramp: first %d, last %d after a gain move to 0" % (ramp[0], ramp[FRAMES - 1]))
    check(all(ramp[i] >= ramp[i + 1] for i in range(FRAMES - 1)), "ramp: not monotone")
    g.run(None)
    check(peak(read(g, dst)) == 0, "ramp: a gain of 0 still passes signal")


# -- 5. the output node -----------------------------------------------------------

def check_output():
    g = new_graph(channel_count=2, buffers=4, nodes=4)
    lb, rb = g.buffer(), g.buffer()
    g.set(g.add(ag.CONST, ag.ALWAYS, (), lb), 0, 1000.0)
    g.set(g.add(ag.CONST, ag.ALWAYS, (), rb), 0, -2000.0)
    g.add(ag.OUT, ag.ALWAYS, (lb, rb), None)
    out = ints(FRAMES * 2 * 3)
    g.run(out, 1)
    block = FRAMES * 2
    check(out[0] == 0 and out[block - 1] == 0 and out[2 * block] == 0 and out[3 * block - 1] == 0,
          "out: blocks 0 and 2 were written for a run at block 1")
    check(out[block] == 1000 and out[block + 1] == -2000 and out[2 * block - 2] == 1000
          and out[2 * block - 1] == -2000, "out: block 1 is not interleaved L/R")
    try:
        g.run(out, 3)
    except ValueError:
        pass
    else:
        check(False, "out: a block past the buffer's end was accepted")

    m = new_graph(channel_count=1, buffers=4, nodes=4)
    lb, rb = m.buffer(), m.buffer()
    m.set(m.add(ag.CONST, ag.ALWAYS, (), lb), 0, 1000.0)
    m.set(m.add(ag.CONST, ag.ALWAYS, (), rb), 0, -2000.0)
    o = m.add(ag.OUT, ag.ALWAYS, (lb, rb), None)
    out = ints(FRAMES)
    m.run(out)
    check(out[0] == -500 and out[FRAMES - 1] == -500, "out: mono is not the mean (%d)" % out[0])
    m.set(o, 0, 2.0)
    m.run(out)
    check(out[0] == -1000, "out: the scale parameter is ignored (%d)" % out[0])


# -- 6. refusals ---------------------------------------------------------------------

def refuses(what, fn):
    try:
        fn()
    except ValueError:
        return
    check(False, "accepted " + what)


def check_refusals():
    refuses("frames=300", lambda: ag.Graph(frames=300))
    refuses("channel_count=3", lambda: ag.Graph(channel_count=3))
    g = new_graph(buffers=2, nodes=2)
    b = g.buffer()
    g.buffer()
    refuses("a third buffer of two", g.buffer)
    refuses("an input id past the buffers", lambda: g.add(ag.MIX, ag.ALWAYS, (7,), b))
    refuses("an output id past the buffers", lambda: g.add(ag.CONST, ag.ALWAYS, (), 7))
    refuses("a constant with no output", lambda: g.add(ag.CONST, ag.ALWAYS, (), None))
    refuses("nine inputs", lambda: g.add(ag.MIX, ag.ALWAYS, (b,) * 9, b))
    refuses("an unknown kind", lambda: g.add(ag.KINDS if hasattr(ag, "KINDS") else 99, ag.ALWAYS, (), b))
    refuses("group 32", lambda: g.add(ag.CONST, 32, (), b))
    n = g.add(ag.CONST, ag.ALWAYS, (), b)
    g.add(ag.CONST, ag.ALWAYS, (), b)
    refuses("a third node of two", lambda: g.add(ag.CONST, ag.ALWAYS, (), b))
    refuses("parameter 16", lambda: g.set(n, 16, 0.0))
    refuses("get of node 5", lambda: g.get(5, 0))
    refuses("reset of node 5", lambda: g.reset(5))
    refuses("a table on a constant", lambda: g.table(n, array("h", [0, 1])))
    refuses("peak of buffer 9", lambda: g.peak(9))
    refuses("a short read buffer", lambda: g.read(b, ints(3)))


for step in (check_wiring, check_filter, check_groups, check_ramp, check_output, check_refusals):
    step()

if failures:
    print("GRAPH FAIL")
    for line in failures:
        print("  " + line)
    sys.exit(1)
print("GRAPH PASS")
