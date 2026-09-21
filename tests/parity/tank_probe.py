"""Deterministic Tank PCM.

    tank_probe.py audioverb

Like multiply_probe.py and feedback_delay_probe.py, this has no oracle:
`audioverb` is audiodsp's own module, with no ancestor in CircuitPython or in
micropython-vst3's engine. What the golden pins is that every interpreter
renders it identically.

The tank is the most recursive thing in the suite. Six all-passes and four
delays feed each other in `float`, the two halves cross, and the whole network
runs for as long as the decay holds -- so a one-ulp disagreement between two
builds is not a one-ulp disagreement in the output, it is amplified for
thousands of frames. If three builds are going to fall out of step about
floating point, this is where it shows first.

The last fixture is not a checksum. `tail` prints the block at which the wet
output reaches *exact* zero, which is the whole reason every line write goes
through a magnitude-truncating quantiser rather than a rounding one: rounding
gives a network that sits at one LSB forever. A build whose quantiser started
rounding would still hash its other fixtures plausibly and would print `none`
here.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audioverb"
# A built-in module under MicroPython, which does not record those in
# sys.modules - take what __import__ hands back.
verb = __import__(MODULE)

SAMPLE_RATE = 8000
MAX_PREDELAY_MS = 60.0


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def source(frames=1600, level=14000, burst=120):
    """A burst and then a long tail, which is what a reverberator has to be
    driven with: the network has to be heard somewhere the input is not."""
    values = array("h")
    for frame in range(frames):
        for channel in range(2):
            if frame < burst:
                shape = ((frame * (97 + channel * 18)) % 2001) - 1000
                values.append(shape * level // 1000)
            else:
                values.append(0)
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=2)


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        print("tank", tag, index, len(data), sum(data), checksum(data))


def scale(reference, rate=SAMPLE_RATE):
    frames = reference * rate // 29761
    return frames if frames >= 4 else 4


# Two alternative networks, to exercise the Python-supplied topology the same
# way a character table will: a small bright room and a longer chamber. The
# tap sets are deliberately not the default's - a re-cut network re-cuts both.
ROOM_DELAYS = [scale(v) for v in
               (89, 71, 241, 173, 421, 2311, 1129, 1901,
                587, 2179, 1663, 1637)]
ROOM_TAPS = [0, 9, ROOM_DELAYS[9] // 3, 0.6,
             0, 10, ROOM_DELAYS[10] // 2, -0.6,
             0, 7, ROOM_DELAYS[7] // 5, -0.6,
             1, 5, ROOM_DELAYS[5] // 3, 0.6,
             1, 6, ROOM_DELAYS[6] // 2, -0.6,
             1, 11, ROOM_DELAYS[11] // 5, -0.6]
CHAMBER_DELAYS = [scale(v) for v in
                  (211, 158, 499, 397, 971, 6113, 2477, 5023,
                   1249, 5779, 3517, 4409)]
CHAMBER_TAPS = [0, 9, CHAMBER_DELAYS[9] // 7, 0.5,
                0, 9, CHAMBER_DELAYS[9] * 2 // 3, 0.5,
                0, 11, CHAMBER_DELAYS[11] // 2, -0.5,
                0, 5, CHAMBER_DELAYS[5] // 2, -0.5,
                1, 5, CHAMBER_DELAYS[5] // 7, 0.5,
                1, 5, CHAMBER_DELAYS[5] * 2 // 3, 0.5,
                1, 10, CHAMBER_DELAYS[10] // 2, -0.5,
                1, 9, CHAMBER_DELAYS[9] // 2, -0.5]


# A bare network first, then each thing in it turned on one at a time, so a
# failure says which part of the path moved.
CASES = (
    ("bare", {"mix": 2.0, "decay": 0.5}, {}),
    ("short", {"mix": 2.0, "decay": 0.2}, {}),
    ("long", {"mix": 2.0, "decay": 0.9}, {}),
    ("damped", {"mix": 2.0, "decay": 0.7, "damping_hz": 700.0}, {}),
    ("banded", {"mix": 2.0, "decay": 0.7, "bandwidth_hz": 2200.0,
                "low_cut_hz": 120.0}, {}),
    ("mod-off", {"mix": 2.0, "decay": 0.7, "mod_depth_ms": 0.4}, {}),
    ("mod-on", {"mix": 2.0, "decay": 0.7, "mod_depth_ms": 0.4,
                "mod_rate_hz": 1.3}, {}),
    ("disp-off", {"mix": 2.0, "decay": 0.7, "diffusion": 0.0}, {}),
    ("disp-on", {"mix": 2.0, "decay": 0.7, "diffusion": 0.9}, {}),
    ("predelay", {"mix": 2.0, "decay": 0.6, "predelay_ms": 23.0}, {}),
    ("driven", {"mix": 2.0, "decay": 0.7, "drive": 0.9}, {}),
    ("toned", {"mix": 2.0, "decay": 0.7, "tone_db": -9.0, "width": 0.4}, {}),
    ("blend", {"mix": 0.37, "decay": 0.6}, {}),
    ("room", {"mix": 2.0, "decay": 0.6, "mod_rate_hz": 0.9,
              "mod_depth_ms": 0.2},
     {"delays": ROOM_DELAYS, "taps": ROOM_TAPS}),
    ("chamber", {"mix": 2.0, "decay": 0.8, "damping_hz": 1600.0},
     {"delays": CHAMBER_DELAYS, "taps": CHAMBER_TAPS}),
    ("everything", {"mix": 0.64, "decay": 0.82, "diffusion": 0.7,
                    "damping_hz": 1400.0, "bandwidth_hz": 3100.0,
                    "low_cut_hz": 140.0, "predelay_ms": 11.0,
                    "mod_depth_ms": 0.35, "mod_rate_hz": 1.7, "drive": 0.5,
                    "width": 1.6, "tone_db": 6.0}, {}),
)

for name, options, shape in CASES:
    settings = {}
    settings.update(shape)
    settings.update(options)
    node = verb.Tank(sample_rate=SAMPLE_RATE,
                     max_predelay_ms=MAX_PREDELAY_MS, **settings)
    node.play(source())
    emit(name, node, 8)

# `mix=0` is a wire. The network still runs -- every line, every all-pass, the
# modulation -- and what comes out is what went in, sample for sample. Driven
# with unbroken material rather than the burst, so the fixture is six full
# blocks and not one block and five of silence.
node = verb.Tank(sample_rate=SAMPLE_RATE, max_predelay_ms=MAX_PREDELAY_MS,
                 mix=0.0, decay=0.8, mod_rate_hz=1.1, mod_depth_ms=0.3)
node.play(source(burst=1600))
reference = bytes(audiocore.get_buffer(source(burst=1600))[1])
wired = b""
for index in range(6):
    data = bytes(audiocore.get_buffer(node)[1])
    wired += data
    print("tank wire", index, len(data), sum(data), checksum(data))
print("tank wire-exact", wired == reference[:len(wired)])

# Set mid-stream: the lines keep their contents and only what the network does
# to them changes, which is a different fixture from building it that way.
node = verb.Tank(sample_rate=SAMPLE_RATE, max_predelay_ms=MAX_PREDELAY_MS,
                 mix=2.0, decay=0.75)
node.play(source())
emit("late-a", node, 3)
node.set(damping_hz=900.0, decay=0.4, width=0.2)
emit("late-b", node, 3)
node.clear()
emit("late-c", node, 3)

# Clamping: everything past its bound, and a mono network.
node = verb.Tank(sample_rate=SAMPLE_RATE, max_predelay_ms=MAX_PREDELAY_MS,
                 decay=4.0, diffusion=3.0, mix=7.0, width=9.0, drive=5.0,
                 tone_db=90.0, predelay_ms=5000.0, mod_depth_ms=900.0,
                 mod_rate_hz=400.0)
node.play(source())
emit("clamped", node, 4)

node = verb.Tank(sample_rate=SAMPLE_RATE, max_predelay_ms=MAX_PREDELAY_MS,
                 channel_count=1, mix=2.0, decay=0.7)
values = array("h")
for frame in range(1600):
    shape = ((frame * 61) % 2001) - 1000 if frame < 120 else 0
    values.append(shape * 14000 // 1000)
node.play(audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                              channel_count=1))
emit("mono", node, 4)

# No source: silence, never a short block, never finished.
node = verb.Tank(sample_rate=SAMPLE_RATE, max_predelay_ms=MAX_PREDELAY_MS)
emit("starved", node, 2)

# The tail reaches exact zero. Every line write goes through a magnitude
# truncation rather than a rounding, so a loop under unity gain strictly loses
# magnitude every pass; rounding leaves the network humming at one LSB for as
# long as it is pulled. The block index is the fixture.
node = verb.Tank(sample_rate=SAMPLE_RATE, max_predelay_ms=MAX_PREDELAY_MS,
                 mix=2.0, decay=0.4)
node.play(source(frames=40000))
silent = None
for index in range(150):
    data = bytes(audiocore.get_buffer(node)[1])
    if not any(data):
        silent = index
        break
print("tank tail", silent if silent is not None else "none")
