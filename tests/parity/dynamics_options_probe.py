"""Deterministic Dynamics PCM for the options the original never had.

    dynamics_options_probe.py audiodynamics

Like `feedback_delay_probe.py` and `dynamics_extras_probe.py`, this has no
oracle. `dynamics_probe.py` is held against micropython-vst3's
`vstaudio_dsp.c` compiled unmodified, so it may only use forms that original
accepts; everything the effects program added to `audiodynamics` -- the RMS
detector, the feedback and external-key detector topologies, the 4x true-peak
reconstruction, the key band's second pole and low corner, Key Listen, the
settable expander/gate depth, the gate's four-stage envelope, the
relative-threshold gain computer, the program-dependent attack, and the
transient shaper's now-settable time constants, second envelope pair and slow
peak-hold -- has no ancestor to be held to. What the golden pins is that every
interpreter renders it identically.

The first case, `off`, sets none of them, and its numbers are
`dynamics_probe.py`'s `compress` case line for line: same source, same
settings, same five blocks. That is the check that says the twenty-one
additions are additive rather than a rewrite.
"""

import sys
from array import array

import audiocore
from audiodsp_util import float32_bits

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiodynamics"
# A built-in module under MicroPython, which does not record those in
# sys.modules - take what __import__ hands back.
dynamics = __import__(MODULE)

SAMPLE_RATE = 48000


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def source(frames=1200, quiet=700, loud=12000, channels=2):
    """dynamics_probe.py's burst pattern, so the `off` case can be compared
    with that probe's `compress` case directly: 40 loud frames, then 60 quiet
    ones, and both halves of every detector move inside one 256-frame block."""
    values = array("h")
    for frame in range(frames):
        level = loud if (frame % 100) < 40 else quiet
        for channel in range(channels):
            shape = ((frame * (97 + channel * 18)) % 2001) - 1000
            values.append(shape * level // 1000)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def sibilant(frames=1200):
    """A low tone plus a high one at a fixed balance: what a de-esser's key
    band has to separate, and what the relative-threshold computer has to
    read the same way at any level."""
    values = array("h")
    for frame in range(frames):
        low = ((frame * 13) % 240) - 120
        high = ((frame * 211) % 40) - 20
        for channel in range(2):
            values.append((low * 90 + high * 300 + channel * 7))
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=2)


def keyed(frames=1200):
    """Silence, then a hard burst: an external key that opens something
    partway through the capture rather than at its start."""
    values = array("h")
    for frame in range(frames):
        value = 0 if frame < 500 else (26000 if frame % 2 else -26000)
        values.append(value)
        values.append(value)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=2)


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        # The gain reduction prints as its float32 bit pattern, not as
        # `"%.6f"`: the three targets hold the identical float32 and the
        # PCM beside it is byte-for-byte the same, but MicroPython's
        # single-precision formatter is not correctly rounded to seven
        # significant digits. audiodsp#80.
        print("dynopt", tag, index, len(data), sum(data), checksum(data),
              float32_bits(node.gain_reduction_db()))


#: dynamics_probe.py's `compress` case, verbatim.
BASE = {"threshold_db": -30.0, "ratio": 6.0, "knee_db": 9.0,
        "makeup_db": 4.0, "attack_ms": 5.0, "release_ms": 80.0}
GATE = {"threshold_db": -22.0, "attack_ms": 1.0, "release_ms": 150.0}
SHAPE = {"attack_gain_db": 9.0, "sustain_gain_db": -7.0}


def options(base, **extra):
    merged = {}
    merged.update(base)
    merged.update(extra)
    return merged


# Each option on its own, so a failure names the branch that moved.
CASES = (
    # Nothing set: dynamics_probe.py's `compress` case, line for line.
    ("off", 0, options(BASE)),
    # The word form of the default, which takes the string path to the same
    # place the number does.
    ("peak-word", 0, options(BASE, detector="peak")),
    ("rms", 0, options(BASE, detector="rms")),
    ("rms-2ms", 0, options(BASE, detector="rms", rms_ms=2.0)),
    ("rms-number", 0, options(BASE, detector=1)),
    ("feedback", 0, options(BASE, feedback_detector=1)),
    ("program", 0, options(BASE, program_attack=1)),
    ("truepeak-2", 1, {"threshold_db": -26.0, "attack_ms": 0.5,
                       "release_ms": 40.0, "true_peak": 2}),
    ("truepeak-2-look", 1, {"threshold_db": -26.0, "attack_ms": 0.5,
                            "release_ms": 40.0, "true_peak": 2,
                            "lookahead_ms": 2.0}),
    ("keyband", 0, options(BASE, sidechain_hz=800.0, sidechain_lp_hz=4000.0)),
    ("keyband-2p", 0, options(BASE, sidechain_hz=800.0,
                              sidechain_lp_hz=4000.0, sidechain_poles=2)),
    ("hicut-2p", 0, options(BASE, sidechain_hz=2500.0, sidechain_poles=2)),
    ("keylisten", 0, options(BASE, sidechain_hz=2500.0, key_listen=1)),
    ("depth-expand", 2, {"threshold_db": -20.0, "ratio": 3.0,
                         "depth_db": -18.0}),
    # Below the original's own floor, which the clamp used to swallow.
    ("depth-deep", 2, {"threshold_db": -20.0, "ratio": 3.0,
                       "depth_db": -90.0}),
    ("depth-gate", 3, options(GATE, depth_db=-35.0)),
    ("hold", 3, options(GATE, hold_ms=5.0)),
    ("hold-hyst", 3, options(GATE, hold_ms=5.0, hysteresis_db=6.0,
                             depth_db=-45.0)),
    # A hold longer than the capture, so the machine never leaves HOLD.
    ("hold-long", 3, options(GATE, hold_ms=200.0)),
    ("transient-times", 4, options(SHAPE, transient_fast_attack_ms=0.4,
                                   transient_fast_release_ms=12.0,
                                   transient_slow_attack_ms=60.0,
                                   transient_slow_release_ms=900.0)),
    ("transient-hold", 4, options(SHAPE, slow_hold_ms=40.0)),
    ("transient-dual", 4, options(SHAPE, transient_dual=1)),
    ("transient-dual-times", 4, options(SHAPE, transient_dual=1,
                                        sustain_fast_attack_ms=2.0,
                                        sustain_fast_release_ms=90.0,
                                        sustain_slow_attack_ms=40.0,
                                        sustain_slow_release_ms=2000.0)),
    ("transient-dual-hold", 4, options(SHAPE, transient_dual=1,
                                       slow_hold_ms=40.0)),
    # Zero and negative values on the new options take their own branches:
    # a depth at or above 0 dB is unset, hold and slow-hold clamp to off,
    # a hysteresis carries no sign, and poles below 2 is one pole.
    ("clamped", 3, options(GATE, depth_db=0.0, hold_ms=-4.0,
                           hysteresis_db=-9.0, sidechain_poles=0,
                           slow_hold_ms=-1.0, rms_ms=-2.0)),
    # Several at once, because they share one loop.
    ("everything", 0, options(BASE, detector="rms", rms_ms=6.0,
                              feedback_detector=1, program_attack=1,
                              true_peak=2, lookahead_ms=1.5,
                              sidechain_hz=600.0, sidechain_lp_hz=6000.0,
                              sidechain_poles=2)),
)

for name, mode, opts in CASES:
    node = dynamics.Dynamics(mode, sample_rate=SAMPLE_RATE, **opts)
    node.play(source())
    emit(name, node, 5)

# The relative-threshold computer earns its own material: a fixed spectral
# balance, rendered at three levels, where the absolute-overshoot computer
# gives three different answers and this one gives one.
for tag, flag in (("relative-off", 0), ("relative-on", 1)):
    node = dynamics.Dynamics(0, sample_rate=SAMPLE_RATE, threshold_db=-30.0,
                             ratio=6.0, knee_db=3.0, attack_ms=0.5,
                             release_ms=60.0, sidechain_hz=5000.0,
                             relative_threshold=flag)
    node.play(sibilant())
    emit(tag, node, 4)

# An external key: the gain lands on the audio, the detector reads the burst.
node = dynamics.Dynamics(3, sample_rate=SAMPLE_RATE, threshold_db=-12.0,
                         attack_ms=1.0, release_ms=30.0, depth_db=-60.0)
node.play(source())
node.key(keyed())
emit("key", node, 5)

# The same node with the key taken away mid-stream: the detector goes back to
# reading the audio, and the gate closes again.
node.key(None)
emit("key-cleared", node, 3)

# A key shorter than the audio. It runs dry, and a starved node answers with
# silence rather than a short block or a detector that quietly reverts.
node = dynamics.Dynamics(3, sample_rate=SAMPLE_RATE, threshold_db=-12.0,
                         attack_ms=1.0, release_ms=30.0, depth_db=-60.0)
node.play(source())
node.key(keyed(frames=700))
emit("key-short", node, 5)

# Key Listen with an external key: the output is the key's band, not the audio
# at all.
node = dynamics.Dynamics(0, sample_rate=SAMPLE_RATE, sidechain_hz=1500.0,
                         key_listen=1)
node.play(source())
node.key(keyed())
emit("key-listen", node, 4)

# Mono, where the second channel is a copy rather than a channel: the
# feedback detector's stored output and the key both have to follow that.
node = dynamics.Dynamics(0, sample_rate=SAMPLE_RATE, channel_count=1,
                         threshold_db=-30.0, ratio=6.0, attack_ms=2.0,
                         release_ms=60.0, feedback_detector=1, detector="rms")
node.play(source(channels=1))
emit("mono", node, 4)

# Set mid-stream: the new options have to reach the running node the way the
# old ones do, and the detector keeps its memory across the change.
node = dynamics.Dynamics(3, sample_rate=SAMPLE_RATE, **GATE)
node.play(source())
emit("late-a", node, 2)
node.set(hold_ms=8.0, hysteresis_db=4.0, depth_db=-50.0)
emit("late-b", node, 3)
node.set(detector="rms", program_attack=1, true_peak=2)
emit("late-c", node, 3)

# reset_buffer drops the four-stage envelope and the peak-hold along with the
# detector, but keeps the key band's filter memory and the last reported gain
# reduction - the original's rule, extended to the state the additions keep.
node = dynamics.Dynamics(3, sample_rate=SAMPLE_RATE, threshold_db=-22.0,
                         attack_ms=1.0, release_ms=150.0, hold_ms=6.0,
                         depth_db=-40.0, sidechain_hz=1200.0)
node.play(source())
emit("reset-before", node, 3)
audiocore.reset_buffer(node)
print("dynopt reset-gr", float32_bits(node.gain_reduction_db()))
node.play(source())
emit("reset-after", node, 3)

# Never fed at all, with the gate machine armed: silence, and a node that
# never reports itself finished.
node = dynamics.Dynamics(3, sample_rate=SAMPLE_RATE, hold_ms=10.0,
                         depth_db=-30.0)
emit("unplayed", node, 2)

# Sample rate is what every millisecond option is read against, the new ones
# included.
for rate in (22050, 96000):
    node = dynamics.Dynamics(3, sample_rate=rate, threshold_db=-22.0,
                             attack_ms=1.0, release_ms=150.0, hold_ms=5.0,
                             hysteresis_db=3.0, depth_db=-40.0,
                             slow_hold_ms=20.0, rms_ms=4.0)
    node.play(source())
    emit("rate-%d" % rate, node, 3)

# `gain_smooth_ms` (audiodsp#61): a one-pole on the computed gain, between the
# gain computer and the multiply. Off is asserted here as well as on, because
# off is what every other fixture in this file and in dynamics_probe.py was
# captured under -- an "off" case that stopped matching the un-smoothed cases
# would say the default had moved.
#
# A short release on low-frequency material is the condition the option exists
# for: the per-sample gain follows the waveform, which is intermodulation
# rather than compression. Measured on the 50 Hz tone this fixture is not (the
# probe's own source is broadband on purpose), the ripple runs 6.82 % at a
# 10 ms release and falls to 1.04 % at 10 ms of smoothing and 0.41 % at 30 ms.
SMOOTH_BASE = {"threshold_db": -30.0, "ratio": 6.0, "knee_db": 0.0,
               "attack_ms": 1.0, "release_ms": 10.0}
for milliseconds in (0.0, 1.0, 10.0, 30.0):
    node = dynamics.Dynamics(0, sample_rate=SAMPLE_RATE,
                             **options(SMOOTH_BASE,
                                       gain_smooth_ms=milliseconds))
    node.play(source())
    emit("smooth-%g" % milliseconds, node, 3)

# Set mid-stream, and set back off again: the coefficient is recomputed and
# the smoother's own memory carries across, which is a different fixture from
# building the node with it.
node = dynamics.Dynamics(0, sample_rate=SAMPLE_RATE, **SMOOTH_BASE)
node.play(source())
emit("smooth-late-a", node, 2)
node.set(gain_smooth_ms=20.0)
emit("smooth-late-b", node, 2)
node.set(gain_smooth_ms=0.0)
emit("smooth-late-c", node, 2)

# Every mode goes through the same smoother, so each is captured with it on.
for name, mode, extra in (("limit", 1, {"threshold_db": -26.0}),
                          ("expand", 2, {"threshold_db": -20.0, "ratio": 3.0}),
                          ("gate", 3, GATE),
                          ("transient", 4, SHAPE)):
    node = dynamics.Dynamics(mode, sample_rate=SAMPLE_RATE,
                             gain_smooth_ms=8.0, **extra)
    node.play(source())
    emit("smooth-%s" % name, node, 3)

# The millisecond reading is against the sample rate, as every other one is.
for rate in (22050, 96000):
    node = dynamics.Dynamics(0, sample_rate=rate, threshold_db=-30.0,
                             ratio=6.0, release_ms=10.0, gain_smooth_ms=10.0)
    node.play(source())
    emit("smooth-rate-%d" % rate, node, 2)

# `feedback_gain_corrected` (audiodsp#62). Off, a feedback detector reads the
# reduced output, so the loop reduces its own detector input and settles at its
# own fixed point: against a 20 dB overshoot, 4:1 through 20:1 all land between
# 8.57 and 9.74 dB of reduction -- about 2:1 whatever the knob says. On, the
# gain computer's slope becomes (R - 1) rather than (1 - 1/R), which is what
# makes an output-referred detector settle on the input-referred curve: the
# same four ratios then read 14.999 / 17.499 / 18.332 / 18.999 against the
# algebra's 15.000 / 17.500 / 18.333 / 19.000.
#
# The raw case is captured beside it because it is what the three
# `feedback_detector=1` fixtures above were captured under, and because the
# pair is the evidence: the two must differ, or the option does nothing.
FB_BASE = {"threshold_db": -40.0, "knee_db": 0.0, "attack_ms": 5.0,
           "release_ms": 80.0}
for ratio in (4.0, 8.0, 20.0):
    for corrected in (0, 1):
        node = dynamics.Dynamics(0, sample_rate=SAMPLE_RATE,
                                 **options(FB_BASE, ratio=ratio,
                                           feedback_detector=1,
                                           feedback_gain_corrected=corrected))
        node.play(source())
        emit("fbcorr-%g-%d" % (ratio, corrected), node, 3)

# It is COMPRESS-only, deliberately: the limiter's law has no ratio to invert
# and the gate's slope is a fixed 8. Captured so that stays true rather than
# being assumed - these must match the same builds without the keyword.
for name, mode, extra in (("limit", 1, {"threshold_db": -26.0}),
                          ("gate", 3, GATE),
                          ("transient", 4, SHAPE)):
    node = dynamics.Dynamics(mode, sample_rate=SAMPLE_RATE,
                             feedback_detector=1, feedback_gain_corrected=1,
                             **extra)
    node.play(source())
    emit("fbcorr-%s" % name, node, 2)

# Set mid-stream: the slope changes under a loop that already has state in it.
node = dynamics.Dynamics(0, sample_rate=SAMPLE_RATE,
                         **options(FB_BASE, ratio=8.0, feedback_detector=1))
node.play(source())
emit("fbcorr-late-a", node, 2)
node.set(feedback_gain_corrected=1)
emit("fbcorr-late-b", node, 2)
node.set(feedback_gain_corrected=0)
emit("fbcorr-late-c", node, 2)

# On without the feedback detector must be inert: the flag is only read inside
# the feedback branch, and a fixture says so.
node = dynamics.Dynamics(0, sample_rate=SAMPLE_RATE,
                         **options(FB_BASE, ratio=8.0,
                                   feedback_gain_corrected=1))
node.play(source())
emit("fbcorr-no-feedback", node, 2)

print("done dynamics options")
