"""audiograph's PHASER, LAG, LFO shapes, SAMPLE_HOLD, TWOPOLE, OVERDRIVE, PAN, WAVEFOLD, LPG and MODAL, rendered for verify_graph.py.

    graph_nodes_probe.py                 # prints the render, then GRAPH NODES PASS
    graph_nodes_probe.py --fault phaser  # one phaser frequency a block late
    graph_nodes_probe.py --fault lag     # one legato flag flipped
    graph_nodes_probe.py --fault lfo     # one shape switch a block late
    graph_nodes_probe.py --fault hold    # one clock edge a sample late
    graph_nodes_probe.py --fault twopole    # one block's cutoff CV a block late
    graph_nodes_probe.py --fault overdrive  # one tone change a block late
    graph_nodes_probe.py --fault pan        # one pan change a block late
    graph_nodes_probe.py --fault wavefold   # one symmetry change a block late
    graph_nodes_probe.py --fault lpg        # one strike a block late
    graph_nodes_probe.py --fault modal      # one frequency change a block late

The same file runs on CPython, MicroPython and CircuitPython and prints every
sample each of these nodes writes, every block. ``verify_graph.py`` runs it on
each interpreter and compares the renders sample for sample. This file only
renders, and checks that what it rendered is worth comparing: a phaser that
moves the sound, glides that pass through pitches between notes, LFOs that
swing, holds that step.

The patch: a sawtooth through two phasers (four stages with the centre swept
each block, six with a negative feedback); an LFO of each shape, one switched
to random and back mid-render and one reset; three glides on one written note
line with a bend that moves every sample (linear always, linear on legato
only, one-pole); and four sample-and-holds (noise clocked by the square LFO,
the same with a lag, a written clock with two edges in a block, and the
oscillator held at a rate with no clock); the five two-pole types on a
sawtooth and on noise, with a cutoff CV that holds still for some blocks and
runs through others, and one with no CV connected; three overdrives (the
sawtooth, hot, with the tone swept); and a stereo PAN of four of these and an
open input, its gains and pans moving every few blocks, past the ends once,
and reset once, both sides printed; and three wavefolders (the sawtooth folded
deep, the sawtooth with the cutoff CV on its fold and the symmetry swept, and a
phaser folded at full depth); and three low-pass gates (the deep fold struck
every twenty blocks in BOTH mode and reset once, the sawtooth filtered on the
triangle LFO, and a phaser on the cutoff CV with a manual level and the level
turned down, in VCA mode and then BOTH); and three modal resonators (a bar
struck with the low-pass gates and reset once, a membrane rung by the noise
with its frequency stepped and its mix at three quarters, and the sawtooth
with its own strikes at mix 0, the excitation alone).

Every value handed to the graph is a short binary fraction or an integer, so a
single-precision MicroPython parses the same floats a double one does.

A build without one of these kinds prints ``absent KIND name ...`` first, naming
the nodes it left out; verify_graph.py accepts that only where the kind is
left out by design (OVERDRIVE on CircuitPython).

Each fault is a mistake a binding or a caller could make in one of these nodes
without the node itself being wrong; verify_graph.py plants each on one
interpreter and must see it.
"""

import sys
from array import array

try:
    import audiograph as ag
    ag.PHASER
except (ImportError, AttributeError):
    print("GRAPH PARITY SKIPPED: no audiograph nodes here")
    sys.exit(0)

FAULTS = ("phaser", "lag", "lfo", "hold", "twopole", "overdrive", "pan", "wavefold", "lpg",
          "modal")
FAULT = None
if "--fault" in sys.argv:
    FAULT = sys.argv[sys.argv.index("--fault") + 1]
    if FAULT not in FAULTS:
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


g = ag.Graph(sample_rate=RATE, frames=FRAMES, channel_count=1, buffers=60,
             nodes=60)
watched = []


def node(name, kind, inputs, arg=0.0):
    out = g.buffer()
    index = g.add(kind, ag.ALWAYS, inputs, out, arg)
    watched.append((name, out))
    return index, out


# -- phasers on a sawtooth
saw_b = g.buffer()
saw = g.add(ag.OSC, ag.ALWAYS, (None, None, None, None), saw_b)
g.set(saw, ag.OSC_FREQUENCY, 220.0)
g.set(saw, ag.OSC_WAVEFORM, 1)
g.set(saw, ag.OSC_LEVEL, 0.5)
watched.append(("saw", saw_b))
ph4, _ = node("phaser4", ag.PHASER, (saw_b,), 4.0)
g.set(ph4, ag.PHASER_FEEDBACK, 0.5)
g.set(ph4, ag.PHASER_MIX, 0.5)
ph6, _ = node("phaser6", ag.PHASER, (saw_b,), 6.0)
g.set(ph6, ag.PHASER_FREQUENCY, 750.0)
g.set(ph6, ag.PHASER_FEEDBACK, -0.375)
g.set(ph6, ag.PHASER_MIX, 0.75)

# -- one LFO of each shape
lfos = []
for name, shape, rate in (("triangle", ag.LFO_TRIANGLE, 5.5),
                          ("sine", ag.LFO_SINE, 3.25),
                          ("saw", ag.LFO_SAW, 0.75),
                          ("square", ag.LFO_SQUARE, 37.5),
                          ("random", ag.LFO_RANDOM, 12.25)):
    index, out = node("lfo_" + name, ag.LFO, ())
    g.set(index, ag.LFO_RATE, rate)
    g.set(index, ag.LFO_AMPLITUDE, 8192.0)
    g.set(index, ag.LFO_SHAPE, shape)
    lfos.append((index, out))
lfo_sine = lfos[1][0]
square_b = lfos[3][1]

# -- glides on a written note line
note_b, pitch_b, legato_b = g.buffer(), g.buffer(), g.buffer()
lag_on, _ = node("lag_on", ag.LAG, (note_b, pitch_b, legato_b))
g.set(lag_on, ag.LAG_TIME, 0.015625)
lag_auto, _ = node("lag_auto", ag.LAG, (note_b, pitch_b, legato_b))
g.set(lag_auto, ag.LAG_TIME, 0.03125)
g.set(lag_auto, ag.LAG_MODE, ag.LAG_AUTO)
lag_pole, _ = node("lag_pole", ag.LAG, (note_b,))
g.set(lag_pole, ag.LAG_TIME, 0.0625)
g.set(lag_pole, ag.LAG_SHAPE, ag.LAG_ONE_POLE)

# -- sample-and-holds
noise_b = g.buffer()
g.add(ag.NOISE, ag.ALWAYS, (), noise_b)
clock_b = g.buffer()
sh_clock, _ = node("hold_clock", ag.SAMPLE_HOLD, (noise_b, square_b))
sh_lag, _ = node("hold_lag", ag.SAMPLE_HOLD, (noise_b, square_b))
g.set(sh_lag, ag.SH_LAG, 0.9375)
sh_written, _ = node("hold_written", ag.SAMPLE_HOLD, (saw_b, clock_b))
sh_rate, _ = node("hold_rate", ag.SAMPLE_HOLD, (saw_b, None))
g.set(sh_rate, ag.SH_RATE, 2000.0)

# -- two-pole filters: each type on the sawtooth, two on noise, one with no CV
cv_b = g.buffer()
tp_sk_lp, _ = node("tp_sk_lp", ag.TWOPOLE, (saw_b, cv_b))
g.set(tp_sk_lp, ag.TWOPOLE_TYPE, ag.TWOPOLE_SK_LOWPASS)
g.set(tp_sk_lp, ag.TWOPOLE_CUTOFF_HZ, 1000.0)
g.set(tp_sk_lp, ag.TWOPOLE_RESONANCE, 0.625)
g.set(tp_sk_lp, ag.TWOPOLE_DRIVE, 2.0)
tp_sk_hp, _ = node("tp_sk_hp", ag.TWOPOLE, (saw_b, cv_b))
g.set(tp_sk_hp, ag.TWOPOLE_TYPE, ag.TWOPOLE_SK_HIGHPASS)
g.set(tp_sk_hp, ag.TWOPOLE_CUTOFF_HZ, 500.0)
g.set(tp_sk_hp, ag.TWOPOLE_DEPTH, 0.5)
g.set(tp_sk_hp, ag.TWOPOLE_RESONANCE, 0.875)
tp_cmos_lp, _ = node("tp_cmos_lp", ag.TWOPOLE, (saw_b, cv_b))
g.set(tp_cmos_lp, ag.TWOPOLE_TYPE, ag.TWOPOLE_CMOS_LOWPASS)
g.set(tp_cmos_lp, ag.TWOPOLE_RESONANCE, 0.75)
g.set(tp_cmos_lp, ag.TWOPOLE_DRIVE, 3.0)
tp_cmos_bp, _ = node("tp_cmos_bp", ag.TWOPOLE, (noise_b, cv_b))
g.set(tp_cmos_bp, ag.TWOPOLE_TYPE, ag.TWOPOLE_CMOS_BANDPASS)
g.set(tp_cmos_bp, ag.TWOPOLE_CUTOFF_HZ, 750.0)
g.set(tp_cmos_bp, ag.TWOPOLE_RESONANCE, 0.5)
tp_cmos_hp, _ = node("tp_cmos_hp", ag.TWOPOLE, (noise_b, cv_b))
g.set(tp_cmos_hp, ag.TWOPOLE_TYPE, ag.TWOPOLE_CMOS_HIGHPASS)
g.set(tp_cmos_hp, ag.TWOPOLE_CUTOFF_HZ, 2000.0)
g.set(tp_cmos_hp, ag.TWOPOLE_RESONANCE, 0.25)
g.set(tp_cmos_hp, ag.TWOPOLE_DRIVE, 1.5)
tp_open, _ = node("tp_open", ag.TWOPOLE, (saw_b, None))
g.set(tp_open, ag.TWOPOLE_TYPE, ag.TWOPOLE_SK_LOWPASS)
g.set(tp_open, ag.TWOPOLE_CUTOFF_HZ, 1250.0)
g.set(tp_open, ag.TWOPOLE_RESONANCE, 0.5)

# -- overdrives: the sawtooth hot, a quiet one and one with the tone swept.
# CircuitPython's audiograph has no OVERDRIVE (it needs a double-precision libm
# its RP2040 build can't link), so there the probe says which nodes it left out
# and verify_graph.py compares the rest.
OVERDRIVES = ("od_a", "od_b", "od_c")
if hasattr(ag, "OVERDRIVE"):
    od_a, _ = node("od_a", ag.OVERDRIVE, (saw_b,))
    g.set(od_a, ag.OVERDRIVE_DRIVE, 0.75)
    g.set(od_a, ag.OVERDRIVE_TONE, 0.25)
    g.set(od_a, ag.OVERDRIVE_LEVEL, 0.5)
    od_b, _ = node("od_b", ag.OVERDRIVE, (saw_b,))
    g.set(od_b, ag.OVERDRIVE_DRIVE, 1.0)
    g.set(od_b, ag.OVERDRIVE_TONE, 1.0)
    g.set(od_b, ag.OVERDRIVE_LEVEL, 2.0)
    od_c, _ = node("od_c", ag.OVERDRIVE, (noise_b,))
else:
    od_a = od_b = od_c = None
    print("absent OVERDRIVE " + " ".join(OVERDRIVES))

# -- a stereo pan of four of the above and an open input
pan_left, pan_right = g.buffer(), g.buffer()
pan_ins = (saw_b, noise_b, None, lfos[0][1], dict(watched)["phaser6"])
pan = g.add(ag.PAN, ag.ALWAYS, pan_ins, pan_left, pan_right)
watched.append(("pan_l", pan_left))
watched.append(("pan_r", pan_right))

# -- wavefolders: the sawtooth deep, the sawtooth on a CV, a phaser at full depth
wf_a, _ = node("wf_a", ag.WAVEFOLD, (saw_b, None))
g.set(wf_a, ag.WAVEFOLD_FOLD, 0.625)
wf_b, _ = node("wf_b", ag.WAVEFOLD, (saw_b, cv_b))
g.set(wf_b, ag.WAVEFOLD_FOLD, 0.25)
wf_c, _ = node("wf_c", ag.WAVEFOLD, (dict(watched)["phaser6"],))
g.set(wf_c, ag.WAVEFOLD_FOLD, 1.0)
g.set(wf_c, ag.WAVEFOLD_SYMMETRY, -0.75)

# -- low-pass gates: the deep fold struck, the sawtooth on an LFO, a phaser on
# the cutoff CV with a manual level
strike_b = g.buffer()
lpg_a, _ = node("lpg_a", ag.LPG, (dict(watched)["wf_a"], None, strike_b))
g.set(lpg_a, ag.LPG_DECAY, 0.125)
lpg_b, _ = node("lpg_b", ag.LPG, (saw_b, lfos[0][1], None))
g.set(lpg_b, ag.LPG_MODE, ag.LPG_VCF)
g.set(lpg_b, ag.LPG_DECAY, 0.0625)
lpg_c, _ = node("lpg_c", ag.LPG, (dict(watched)["phaser6"], cv_b))
g.set(lpg_c, ag.LPG_MODE, ag.LPG_VCA)
g.set(lpg_c, ag.LPG_MANUAL, 0.25)
g.set(lpg_c, ag.LPG_DECAY, 0.5)
g.set(lpg_c, ag.LPG_LEVEL, 0.75)

# -- modal resonators: a bar struck, a membrane rung by noise, the excitation alone
modal_a, _ = node("modal_a", ag.MODAL, (None, strike_b))
g.set(modal_a, ag.MODAL_FREQUENCY, 440.0)
g.set(modal_a, ag.MODAL_DECAY, 1.5)
g.set(modal_a, ag.MODAL_BRIGHTNESS, 0.625)
g.set(modal_a, ag.MODAL_INPUT, 0.0625)
for m, r in enumerate((1.0, 2.0, 3.0, 4.25, 7.875, 9.75, 12.0, 14.25)):
    g.set(modal_a, ag.MODAL_RATIO + m, r)
modal_b, _ = node("modal_b", ag.MODAL, (noise_b,))
g.set(modal_b, ag.MODAL_DECAY, 0.5)
g.set(modal_b, ag.MODAL_BRIGHTNESS, 0.25)
g.set(modal_b, ag.MODAL_INPUT, 0.015625)
g.set(modal_b, ag.MODAL_MIX, 0.75)
for m, r in enumerate((1.0, 1.59375, 2.125, 2.3125, 2.625, 2.875, 3.125, 3.5)):
    g.set(modal_b, ag.MODAL_RATIO + m, r)
modal_strike_b = g.buffer()
modal_c, _ = node("modal_c", ag.MODAL, (saw_b, modal_strike_b))
g.set(modal_c, ag.MODAL_INPUT, 0.25)
g.set(modal_c, ag.MODAL_MIX, 0.0)
for m in range(8):
    g.set(modal_c, ag.MODAL_RATIO + m, 0.0)

NOTES = (0, 0, 4096, 4096, 4096, -2731, -2731, 1365, 9557, 9557, 9557,
         9557, -8192, -8192, 683, 683)
note = array("h", bytes(2 * FRAMES))
pitch = array("h", bytes(2 * FRAMES))
legato = array("h", bytes(2 * FRAMES))
clock = array("h", bytes(2 * FRAMES))
block = array("h", bytes(2 * FRAMES))
#: Per node: samples that differ from the one before, the lowest, the highest.
seen = {}
saw_block = array("h", bytes(2 * FRAMES))
cv = array("h", bytes(2 * FRAMES))
last_cv = array("h", bytes(2 * FRAMES))
strike = array("h", bytes(2 * FRAMES))
modal_strike = array("h", bytes(2 * FRAMES))
moved = False
pan_sides_differ = False
left_block = None

for b in range(BLOCKS):
    # the phaser's centre: a triangle sweep over 64 blocks, in whole hertz
    centre = b if FAULT != "phaser" or b != 40 else b - 1
    step = centre % 64
    tri = step if step < 32 else 64 - step
    g.set(ph4, ag.PHASER_FREQUENCY, 100.0 + 125.0 * tri)

    # the note line: a new note every few blocks, one mid-block; a bend that
    # moves every sample; legato on two blocks in three
    n = NOTES[(b // 3) % len(NOTES)]
    for i in range(FRAMES):
        note[i] = n + (2048 if b == 37 and i >= FRAMES // 2 else 0)
        pitch[i] = note[i] + ((b * FRAMES + i) * 7) % 801 - 400
    flag = UNIT if b % 3 else 0
    if FAULT == "lag" and b == 21:
        flag = UNIT - flag
    for i in range(FRAMES):
        legato[i] = flag
    g.write(note_b, note)
    g.write(pitch_b, pitch)
    g.write(legato_b, legato)

    # a written clock: two edges in block 50, one in some others
    late = 1 if FAULT == "hold" and b == 50 else 0
    for i in range(FRAMES):
        high = False
        if b == 50:
            high = 10 + late <= i < 20 or 90 <= i < 100
        elif b % 9 == 0:
            high = 30 <= i < 60
        clock[i] = UNIT if high else 0
    g.write(clock_b, clock)

    # the sine LFO turns random and back; the triangle starts again
    switch = 60 if FAULT != "lfo" else 61
    if b == switch:
        g.set(lfo_sine, ag.LFO_SHAPE, ag.LFO_RANDOM)
    if b == 100:
        g.set(lfo_sine, ag.LFO_SHAPE, ag.LFO_SAW)
    if b == 80:
        g.reset(lfos[0][0])
    if b == 120:
        g.set(ph6, ag.PHASER_FEEDBACK, 0.625)
        g.reset(sh_lag)

    # the cutoff CV: still for a few blocks, then a ramp through the block, a
    # fast wobble, and a jump between blocks; the two-poles read its first and
    # last samples
    for i in range(FRAMES):
        phase = (b // 10) % 4
        if phase == 0:
            cv[i] = 1500
        elif phase == 1:
            cv[i] = -1200 + (b % 10) * 240 + (i * 3) // 4
        elif phase == 2:
            cv[i] = 2400 if i % 8 < 4 else -900
        else:
            cv[i] = (b % 10) * 400 - 2000
    if FAULT == "twopole" and b == 57:
        cv[:] = last_cv
    last_cv[:] = cv
    g.write(cv_b, cv)
    if b == 100:
        g.set(tp_sk_lp, ag.TWOPOLE_RESONANCE, 0.875)
        g.set(tp_cmos_lp, ag.TWOPOLE_TYPE, ag.TWOPOLE_CMOS_BANDPASS)
    if b == 125:
        g.reset(tp_cmos_bp)

    # the tone of one overdrive swept in steps, a level moved mid-render
    if od_c is not None:
        tone_at = b if FAULT != "overdrive" or b != 72 else b - 1
        g.set(od_c, ag.OVERDRIVE_TONE, ((tone_at // 8) % 17) / 16.0)
        g.set(od_c, ag.OVERDRIVE_DRIVE, 0.625)
        if b == 90:
            g.set(od_c, ag.OVERDRIVE_LEVEL, 0.25)
        if b == 140:
            g.reset(od_a)

    # the pan: each input's gain and pan move every few blocks, in quarters,
    # past the ends from block 110; the node is reset at 130
    pan_at = b if FAULT != "pan" or b != 36 else b - 1
    for i in range(len(pan_ins)):
        g.set(pan, i, ((pan_at // 6 + i) % 5) / 4.0)
        position = (((pan_at // 4) * 3 + i * 5) % 9 - 4) / 4.0
        g.set(pan, ag.PAN_PAN + i, position * 1.5 if b >= 110 else position)
    if b == 130:
        g.reset(pan)

    # the wavefolder's symmetry swept in eighths; the deep one's fold moved
    sym_at = b if FAULT != "wavefold" or b != 44 else b - 1
    g.set(wf_b, ag.WAVEFOLD_SYMMETRY, ((sym_at // 4) % 17 - 8) / 8.0)
    if b == 96:
        g.set(wf_a, ag.WAVEFOLD_FOLD, 0.875)

    # a strike every twenty blocks, two blocks long; the VCA gate turns BOTH
    struck_at = b if FAULT != "lpg" or b != 60 else b - 1
    for i in range(FRAMES):
        strike[i] = UNIT if struck_at % 20 < 2 else 0
    g.write(strike_b, strike)
    if b == 100:
        g.set(lpg_c, ag.LPG_MODE, ag.LPG_BOTH)
    if b == 140:
        g.reset(lpg_a)

    # the membrane's frequency steps every ten blocks; the bar is reset once;
    # the sawtooth's own strike rises twice in some blocks
    tuned_at = b if FAULT != "modal" or b != 80 else b - 1
    g.set(modal_b, ag.MODAL_FREQUENCY, 110.0 * (1 + (tuned_at // 10) % 4))
    if b == 150:
        g.reset(modal_a)
    for i in range(FRAMES):
        modal_strike[i] = UNIT if (b % 7 == 3 and (i // 16) % 2 == 1) else 0
    g.write(modal_strike_b, modal_strike)

    g.run(None)
    g.read(saw_b, saw_block)
    for name, buffer in watched:
        g.read(buffer, block)
        print("node %d %s %s" % (b, name, " ".join([str(v) for v in block])))
        stats = seen.setdefault(name, [0, block[0], block[0], block[0]])
        for v in block:
            if v != stats[3]:
                stats[0] += 1
            stats[1] = min(stats[1], v)
            stats[2] = max(stats[2], v)
            stats[3] = v
        if name == "phaser4" and block != saw_block:
            moved = True
        if name == "pan_l":
            left_block = list(block)
        if name == "pan_r" and list(block) != left_block:
            pan_sides_differ = True

check(seen["phaser4"][0] > 10000 and seen["phaser4"][2] > 4000,
      "the swept phaser renders sound")
check(moved, "the phaser moves the sawtooth")
check(seen["lfo_random"][0] > 3, "the random LFO draws levels")
check(seen["lfo_sine"][2] > 8000 and seen["lfo_sine"][1] < -8000,
      "the sine swings")
check(seen["lag_on"][0] > 1000, "a glide passes between notes")
check(seen["lag_pole"][0] > 1000, "the one-pole passes between notes")
check(seen["hold_clock"][0] > 10, "the clocked hold steps")
check(seen["hold_lag"][0] > 10 * seen["hold_clock"][0], "the lag slews")
check(seen["hold_written"][0] >= 10, "the written clock latches")
check(seen["hold_rate"][0] > 500, "the rate hold follows the sawtooth")
for name in ("tp_sk_lp", "tp_sk_hp", "tp_cmos_lp", "tp_cmos_bp", "tp_cmos_hp",
             "tp_open"):
    check(seen[name][0] > 5000 and seen[name][2] > 500,
          "the two-pole %s renders sound" % name)
if od_a is not None:
    for name in OVERDRIVES:
        check(seen[name][0] > 5000 and seen[name][2] > 500,
              "the overdrive %s renders sound" % name)
    check(seen["od_b"][2] > 20000, "the hot overdrive reaches the knee")
for name in ("pan_l", "pan_r"):
    check(seen[name][0] > 5000 and seen[name][2] > 4000,
          "the pan's %s side renders sound" % name)
check(pan_sides_differ, "the pan puts the inputs in different places")
for name in ("wf_a", "wf_b", "wf_c"):
    check(seen[name][0] > 5000 and seen[name][2] > 2000,
          "the wavefolder %s renders sound" % name)
for name in ("lpg_a", "lpg_b", "lpg_c"):
    check(seen[name][0] > 2000 and seen[name][2] > 1000,
          "the low-pass gate %s renders sound" % name)

for name in ("modal_a", "modal_b", "modal_c"):
    check(seen[name][0] > 2000 and seen[name][2] > 1000,
          "the modal resonator %s renders sound" % name)

if failures:
    for what in failures:
        print("FAIL " + what)
    sys.exit(1)
print("GRAPH NODES PASS")
