"""A graph pulled as a source renders what run() renders, for verify_graph.py.

    graph_source_probe.py                # prints the render, then GRAPH SOURCE PASS
    graph_source_probe.py --fault drain  # the queue drained after the render: must FAIL
    graph_source_probe.py --fault split  # commit() publishes half a batch: must FAIL

One script of changes (notes, groups switched on and off, a reset, and patch
changes of dozens of sets at once) is rendered three ways from three copies of
one graph:

``run``    ``set()`` and ``reset()``, then ``run(out, 0, enable)`` per block,
           with the enable mask passed in: the way a caller drove a graph
           before it could be pulled.
``pull``   ``hold()``, the same calls plus ``enable(mask)``, ``commit()``,
           then ``audiocore.get_buffer(graph)`` per block: the queue, drained by
           the renderer at the top of each block, with the graph's own mask.
``tiny``   ``pull`` with a queue of 16 records, so each patch change overflows
           it and is drained from the control side, splitting the batch.

The three must agree to the sample, with the same sounding mask every block.
The ``pull`` render is printed for verify_graph.py, which compares it across
interpreters. Every value is a short binary fraction or an integer, so a
single-precision MicroPython parses the same floats a double one does.

Faults: ``drain`` pulls each block before queueing its changes, which is a
renderer that drains after it renders; ``split`` commits half of each batch,
pulls, and commits the rest after, which is a commit() that publishes half.
"""

import sys
from array import array

try:
    import audiograph as ag
    import audiocore
    ag.Graph.hold
    audiocore.get_buffer
except (ImportError, AttributeError):
    print("GRAPH PARITY SKIPPED: no audiograph source here")
    sys.exit(0)

FAULTS = ("drain", "split")
FAULT = None
if "--fault" in sys.argv:
    FAULT = sys.argv[sys.argv.index("--fault") + 1]
    if FAULT not in FAULTS:
        print("unknown fault %r" % FAULT)
        sys.exit(2)

RATE = 48000
FRAMES = 128
BLOCKS = 40
UNIT = ag.UNIT
failures = []


def check(ok, what):
    if not ok:
        failures.append(what)


def make(queue=0):
    """Two voices (saw, ladder, envelope, VCA) in groups 0 and 1, mixed to a
    stereo pair through a delay. Returns the graph and its node indices."""
    kw = {"sample_rate": RATE, "frames": FRAMES, "channel_count": 2,
          "buffers": 24, "nodes": 24}
    if queue:
        kw["queue"] = queue
    g = ag.Graph(**kw)
    n = {}
    voices = []
    for v in range(2):
        gate_b, env_b, saw_b, vcf_b, amp_b = (g.buffer() for _ in range(5))
        gate = g.add(ag.CONST, v, (), gate_b)
        env = g.add(ag.ENV, v, (gate_b,), env_b)
        osc = g.add(ag.OSC, v, (None, None, None, None), saw_b)
        g.set(osc, ag.OSC_WAVEFORM, 1)
        g.set(osc, ag.OSC_LEVEL, 0.5)
        vcf = g.add(ag.LADDER, v, (saw_b, None), vcf_b)
        vca = g.add(ag.VCA, v, (vcf_b, env_b, None), amp_b)
        voices.append(amp_b)
        n["gate%d" % v] = gate
        n["env%d" % v] = env
        n["osc%d" % v] = osc
        n["vcf%d" % v] = vcf
        n["vca%d" % v] = vca
    left_b, right_b, echo_b = g.buffer(), g.buffer(), g.buffer()
    n["left"] = g.add(ag.MIX, ag.ALWAYS, (voices[0], voices[1]), left_b)
    n["right"] = g.add(ag.MIX, ag.ALWAYS, (voices[0], voices[1]), right_b)
    g.set(n["right"], 0, 0.5)
    n["echo"] = g.add(ag.FBDELAY, ag.ALWAYS, (left_b,), echo_b, 64.0)
    g.set(n["echo"], ag.DELAY_MS, 20.0)
    g.set(n["echo"], ag.DELAY_MIX, 0.25)
    g.add(ag.OUT, ag.ALWAYS, (echo_b, right_b), None)
    return g, n


def patch(n, bright):
    """A patch change: every voice's filter, envelope and oscillator."""
    ops = []
    for v in range(2):
        ops.append(("set", "vcf%d" % v, ag.LADDER_CUTOFF_HZ,
                    4096.0 if bright else 512.0))
        ops.append(("set", "vcf%d" % v, ag.LADDER_RESONANCE,
                    0.25 if bright else 0.75))
        ops.append(("set", "vcf%d" % v, ag.LADDER_DRIVE, 1.5))
        ops.append(("set", "env%d" % v, ag.ENV_ATTACK, 0.0078125))
        ops.append(("set", "env%d" % v, ag.ENV_DECAY, 0.125))
        ops.append(("set", "env%d" % v, ag.ENV_SUSTAIN, 0.5 if bright else 0.75))
        ops.append(("set", "env%d" % v, ag.ENV_RELEASE, 0.0625))
        ops.append(("set", "osc%d" % v, ag.OSC_WAVEFORM, 2 if bright else 1))
        ops.append(("set", "osc%d" % v, ag.OSC_PULSE_WIDTH, 0.25))
        ops.append(("set", "vca%d" % v, ag.VCA_GAIN, 0.75 if bright else 1.0))
    # Moved twice in one batch: only the last value may sound.
    for value in (0.125, 0.5, 0.375):
        ops.append(("set", "echo", ag.DELAY_FEEDBACK, value))
    ops.append(("set", "left", 1, 0.75 if bright else 0.5))
    return ops


def script(n):
    """{block: [op, ...]}; an op is ("set", node, index, value),
    ("reset", node) or ("enable", mask)."""
    s = {}
    s[0] = [("enable", 0b01), ("set", "osc0", ag.OSC_FREQUENCY, 220.0),
            ("set", "gate0", 0, UNIT)]
    s[3] = patch(n, True)
    s[5] = [("enable", 0b11), ("set", "osc1", ag.OSC_FREQUENCY, 330.0),
            ("set", "gate1", 0, UNIT)]
    s[8] = [("set", "gate0", 0, 0.0)]
    s[11] = [("reset", "osc1"), ("set", "osc1", ag.OSC_FREQUENCY, 440.0)]
    s[14] = patch(n, False) + [("set", "gate0", 0, UNIT),
                               ("set", "osc0", ag.OSC_FREQUENCY, 165.0)]
    s[19] = [("set", "gate1", 0, 0.0), ("reset", "echo")]
    s[24] = [("enable", 0b01)]
    s[27] = patch(n, True) + [("set", "gate0", 0, 0.0)]
    s[33] = [("enable", 0b00)]
    return s


def apply(g, n, op, queued):
    if op[0] == "set":
        g.set(n[op[1]], op[2], op[3])
    elif op[0] == "reset":
        g.reset(n[op[1]])
    elif queued:
        g.enable(op[1])


def render_run():
    g, n = make()
    events = script(n)
    pcm = bytearray(FRAMES * 2 * 2)
    mask = 0xFFFFFFFF
    out = []
    for k in range(BLOCKS):
        for op in events.get(k, ()):
            if op[0] == "enable":
                mask = op[1]
            apply(g, n, op, False)
        sounding = g.run(pcm, 0, mask)
        out.append((mask, sounding, array("h", pcm)))
    return out


def render_pull(queue=0, fault=None):
    g, n = make(queue)
    events = script(n)
    mask = 0xFFFFFFFF
    out = []
    for k in range(BLOCKS):
        ops = events.get(k, ())
        for op in ops:
            if op[0] == "enable":
                mask = op[1]
        if fault == "drain":
            data = audiocore.get_buffer(g)[1]
        g.hold()
        half = len(ops) // 2 if fault == "split" else len(ops)
        for op in ops[:half]:
            apply(g, n, op, True)
        g.commit()
        if fault == "split":
            g.hold()
            for op in ops[half:]:
                apply(g, n, op, True)
        if fault != "drain":
            data = audiocore.get_buffer(g)[1]
        if fault == "split":
            g.commit()
        sounding = g.sounding()[0]
        out.append((mask, sounding, array("h", bytes(data))))
    stats = g.stats()
    return out, stats, g, n


def same(a, b):
    worst = 0
    for x, y in zip(a, b):
        d = x - y if x > y else y - x
        if d > worst:
            worst = d
    return worst


reference = render_run()
pulled, stats, g, n = render_pull(fault=FAULT)
tiny, tiny_stats, _, _ = render_pull(queue=16, fault=FAULT)

for k in range(BLOCKS):
    mask, sounding, pcm = pulled[k]
    print("pcm %d %d %d %s" % (k, mask, sounding, " ".join(str(s) for s in pcm)))

loudest = 0
for name, render in (("pull", pulled), ("tiny", tiny)):
    for k in range(BLOCKS):
        want, got = reference[k], render[k]
        worst = same(want[2], got[2])
        check(worst == 0, "%s: block %d differs from run() by %d counts"
              % (name, k, worst))
        check(want[1] == got[1], "%s: block %d sounds %d where run() sounds %d"
              % (name, k, got[1], want[1]))
for _, _, pcm in reference:
    for s in pcm:
        if s > loudest:
            loudest = s
check(loudest > 1000, "the render is too quiet to compare (peak %d)" % loudest)

frames = BLOCKS * FRAMES
check(stats["pushed"] == stats["applied"],
      "pull: %d records queued, %d applied" % (stats["pushed"], stats["applied"]))
check(stats["frame"] == frames,
      "pull: the frame counter reads %d after %d frames" % (stats["frame"], frames))
check(stats["overflows"] == 0, "pull: the default queue overflowed")
check(tiny_stats["splits"] > 0, "tiny: no batch overflowed a 16-record queue")
check(tiny_stats["pushed"] == tiny_stats["applied"],
      "tiny: %d records queued, %d applied"
      % (tiny_stats["pushed"], tiny_stats["applied"]))
check(abs(g.get(n["echo"], ag.DELAY_FEEDBACK) - 0.375) < 1e-6,
      "get: reads %r, not the last value set" % g.get(n["echo"], ag.DELAY_FEEDBACK))
print("stats pushed %d applied %d frame %d overflows %d tiny_splits %d"
      % (stats["pushed"], stats["applied"], stats["frame"], stats["overflows"],
         tiny_stats["splits"]))

if failures:
    for what in failures[:12]:
        print("FAIL", what)
    print("GRAPH SOURCE FAIL: %d checks" % len(failures))
    sys.exit(1)
print("GRAPH SOURCE PASS")
