"""audiograph: one patch, rendered block by block, for verify_graph.py to compare.

    graph_parity_probe.py               # prints the render, then GRAPH PARITY PASS
    graph_parity_probe.py --fault lsb   # one input sample one count off
    graph_parity_probe.py --fault late  # one set() a block late

The same file runs on CPython, MicroPython and CircuitPython, and prints every
output sample of every block. ``verify_graph.py`` runs it on each and compares
the renders sample for sample; this file only renders, and checks that what
it rendered is worth comparing (sound, voices that start and park), because
three interpreters agreeing on silence would prove nothing.

The patch is two voices of a small polysynth with every node kind in it: per
voice a pitch and a gate, two envelopes, a pitch mixer taking the LFO, the
oscillator (saw and pulse, the LFO on its pulse width), noise in a group of its
own, the oscillator-and-noise mix, a cutoff mix, the ladder with its cutoff
input, and the VCA (the first voice on its envelope, the second on its gate);
then once, the LFO, two voice mixers whose gains move mid-render, a high-pass
biquad, two feedback delays with a wow table, and the stereo output. Python
feeds one buffer with ``write()`` every block, the voices are masked out of the
run as their envelopes go quiet, a delay is reset mid-tail, and a second graph
renders mono into the later blocks of one buffer.

Every value handed to the graph is a short binary fraction or an integer, and
the probe's own arithmetic is integer, so a single-precision MicroPython
parses the same floats a double one does: what is compared is the runner, not
how each interpreter rounds a decimal.

The faults are what a binding could get wrong without any node being wrong:
``lsb`` puts one count into the fed buffer that the others do not, and
``late`` applies a cutoff one block after the others do. verify_graph.py
plants each on one interpreter and must see it.
"""

import sys
from array import array

try:
    import audiograph as ag
except ImportError:
    print("GRAPH PARITY SKIPPED: no audiograph here")
    sys.exit(0)

FAULT = None
if "--fault" in sys.argv:
    FAULT = sys.argv[sys.argv.index("--fault") + 1]
    if FAULT not in ("lsb", "late"):
        print("unknown fault %r" % FAULT)
        sys.exit(2)

RATE = 48000
FRAMES = 128
BLOCKS = 160
UNIT = ag.UNIT
failures = []


def check(ok, what):
    if not ok:
        failures.append(what)


def line(tag, values):
    print(tag + " " + " ".join([str(v) for v in values]))


g = ag.Graph(sample_rate=RATE, frames=FRAMES, channel_count=2, buffers=48,
             nodes=48)

lfo_b = g.buffer()
ext_b = g.buffer()
lfo = g.add(ag.LFO, ag.ALWAYS, (), lfo_b)
g.set(lfo, ag.LFO_RATE, 5.0)
g.set(lfo, ag.LFO_AMPLITUDE, 1024.0)

voices = []
for v in range(2):
    pitch_b, gate_b, aenv_b, fenv_b, pmix_b, osc_b, noise_b, dmix_b, cmix_b, \
        vcf_b, vca_b = [g.buffer() for _ in range(11)]
    pitch = g.add(ag.CONST, v, (), pitch_b)
    gate = g.add(ag.CONST, v, (), gate_b)
    aenv = g.add(ag.ENV, v, (gate_b,), aenv_b)
    g.set(aenv, ag.ENV_ATTACK, 0.015625)
    g.set(aenv, ag.ENV_DECAY, 0.25)
    g.set(aenv, ag.ENV_SUSTAIN, 0.625)
    g.set(aenv, ag.ENV_RELEASE, 0.015625)
    fenv = g.add(ag.ENV, v, (gate_b,), fenv_b)
    g.set(fenv, ag.ENV_ATTACK, 0.0078125)
    g.set(fenv, ag.ENV_DECAY, 0.125)
    g.set(fenv, ag.ENV_SUSTAIN, 0.25)
    g.set(fenv, ag.ENV_RELEASE, 0.03125)
    pmix = g.add(ag.MIX, v, (pitch_b, lfo_b), pmix_b)
    g.set(pmix, 1, 0.125)
    osc = g.add(ag.OSC, v, (pmix_b, None, lfo_b, None), osc_b)
    g.set(osc, ag.OSC_WAVEFORM, 3)
    g.set(osc, ag.OSC_FREQUENCY, 110.0 * (v + 1))
    g.set(osc, ag.OSC_LEVEL, 0.5)
    g.set(osc, ag.OSC_PITCH_OCTAVES, 2.0)
    g.set(osc, ag.OSC_PWM_DEPTH, 0.25)
    noise = g.add(ag.NOISE, 8 + v, (), noise_b)
    g.set(noise, 0, 512.0)
    dmix = g.add(ag.MIX, v, (osc_b, noise_b), dmix_b)
    g.set(dmix, 0, 0.75)
    cmix = g.add(ag.MIX, v, (fenv_b,), cmix_b)
    g.set(cmix, 0, 0.5)
    g.set(cmix, ag.MIX_OFFSET, -2048.0)
    vcf = g.add(ag.LADDER, v, (dmix_b, cmix_b), vcf_b)
    g.set(vcf, ag.LADDER_CUTOFF_HZ, 800.0)
    g.set(vcf, ag.LADDER_RESONANCE, 2.5)
    g.set(vcf, ag.LADDER_CUTOFF_OCTAVES, 4.0)
    vca = g.add(ag.VCA, v, (vcf_b, aenv_b, gate_b), vca_b)
    g.set(vca, ag.VCA_SELECT, float(v))
    g.set(vca, ag.VCA_GAIN, 1.5)
    voices.append({"pitch": pitch, "gate": gate, "vcf": vcf, "out": vca_b,
                   "on": False, "held": False})

suml_b, sumr_b, hp_b, dlyl_b, dlyr_b = [g.buffer() for _ in range(5)]
suml = g.add(ag.MIX, ag.ALWAYS, (voices[0]["out"], voices[1]["out"], ext_b),
             suml_b)
g.set(suml, 0, 0.75)
g.set(suml, 1, 0.5)
sumr = g.add(ag.MIX, ag.ALWAYS, (voices[0]["out"], voices[1]["out"]), sumr_b)
g.set(sumr, 0, 0.5)
g.set(sumr, 1, 0.75)
hp = g.add(ag.BIQUAD, ag.ALWAYS, (suml_b,), hp_b)
g.set(hp, ag.BIQUAD_MODE, 1)
g.set(hp, ag.BIQUAD_FREQUENCY, 60.0)
g.set(hp, ag.BIQUAD_Q, 0.75)
# A triangle for the delays' wow, in integers.
wow = array("h", [0] * 256)
for i in range(256):
    wow[i] = (i if i < 128 else 255 - i) * 256 - 16384
dlys = []
for src, dst, ms in ((hp_b, dlyl_b, 7.5), (sumr_b, dlyr_b, 11.25)):
    d = g.add(ag.FBDELAY, ag.ALWAYS, (src,), dst, 20.0)
    g.set(d, ag.DELAY_MS, ms)
    g.set(d, ag.DELAY_FEEDBACK, 0.375)
    g.set(d, ag.DELAY_MIX, 0.5)
    g.set(d, ag.DELAY_DAMPING_HZ, 4000.0)
    g.set(d, ag.DELAY_WOW_HZ, 0.5)
    g.set(d, ag.DELAY_WOW_DEPTH_MS, 1.0)
    g.table(d, wow)
    dlys.append(d)
g.add(ag.OUT, ag.ALWAYS, (dlyl_b, dlyr_b), None)

# What happens when: (block, what, voice or node, value).
EVENTS = {
    0: [("key", 0, 0.0)],
    12: [("key", 1, 2048.0)],
    30: [("gain", suml, 0.25)],
    45: [("cutoff", 0, 2000.0), ("hp", hp, 120.0)],
    60: [("off", 0, None)],
    80: [("reset", dlys[1], None)],
    85: [("key", 0, -1024.0)],
    95: [("off", 1, None)],
    110: [("off", 0, None)],
}
if FAULT == "late":
    EVENTS[46] = [e for e in EVENTS.pop(45) if e[0] == "cutoff"]
    EVENTS[45] = [("hp", hp, 120.0)]


def apply(what, who, value):
    if what == "key":
        voice = voices[who]
        g.set(voice["pitch"], 0, value)
        g.set(voice["gate"], 0, float(UNIT))
        voice["on"] = voice["held"] = True
    elif what == "off":
        voice = voices[who]
        g.set(voice["gate"], 0, 0.0)
        voice["held"] = False
    elif what == "cutoff":
        g.set(voices[who]["vcf"], ag.LADDER_CUTOFF_HZ, value)
    elif what == "gain":
        g.set(who, 0, value)
    elif what == "hp":
        g.set(who, ag.BIQUAD_FREQUENCY, value)
    elif what == "reset":
        g.reset(who)


pcm = array("h", [0] * (FRAMES * 2))
ext = array("h", [0] * FRAMES)
loudest = 0
parked = 0
masks = []
for k in range(BLOCKS):
    for event in EVENTS.get(k, ()):
        apply(*event)
    for i in range(FRAMES):
        ext[i] = (k * FRAMES + i) * 37 % 2001 - 1000
    if FAULT == "lsb" and k == 50:
        ext[64] += 1
    g.write(ext_b, ext)
    enable = 0
    for v, voice in enumerate(voices):
        if voice["on"]:
            enable |= (1 << v) | (1 << (8 + v))
    sounding = g.run(pcm, 0, enable)
    masks.append(enable)
    for v, voice in enumerate(voices):
        # Park a voice the block its amplitude envelope goes quiet.
        if voice["on"] and not voice["held"] and not sounding & (1 << v):
            voice["on"] = False
            parked += 1
    for s in pcm:
        if s > loudest:
            loudest = s
        elif -s > loudest:
            loudest = -s
    line("pcm %d %d %d" % (k, enable, sounding), pcm)

line("peaks", [g.peak(b) for b in (suml_b, sumr_b, hp_b, dlyl_b, dlyr_b)])
line("params", [g.get(voices[0]["vcf"], ag.LADDER_CUTOFF_HZ),
                g.get(hp, ag.BIQUAD_FREQUENCY), g.get(suml, 0)])

# A mono graph, rendered into the later blocks of one buffer.
m = ag.Graph(sample_rate=RATE, frames=64, channel_count=1, buffers=4, nodes=4)
a, b = m.buffer(), m.buffer()
o = m.add(ag.OSC, ag.ALWAYS, (None, None, None, None), a)
m.set(o, ag.OSC_WAVEFORM, 1)
m.set(o, ag.OSC_FREQUENCY, 440.0)
m.set(m.add(ag.CONST, ag.ALWAYS, (), b), 0, 1000.0)
m.add(ag.OUT, ag.ALWAYS, (a, b), None)
mono = array("h", [0] * (64 * 4))
for k in range(1, 4):
    m.run(mono, k)
line("mono", mono)

check(loudest > 2000, "the render is too quiet to compare (peak %d)" % loudest)
check(loudest < 32767, "the render clips (peak %d)" % loudest)
check(parked == 3, "voices parked %d times, not 3" % parked)
check(masks[11] == 0x101 and masks[12] == 0x303, "the voices' groups did not start where the keys did")
check(masks[-1] == 0, "a voice is still running at the end (mask %#x)" % masks[-1])

if failures:
    print("GRAPH PARITY FAIL")
    for f in failures:
        print("  " + f)
    sys.exit(1)
print("GRAPH PARITY PASS")
