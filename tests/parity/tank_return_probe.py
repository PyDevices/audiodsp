"""Tank: a filter or the modulation taken to 0 and brought back.

    tank_return_probe.py audioverb

A corner of 0 takes `bandwidth_hz`, `low_cut_hz` or `damping_hz` out. Their
states used to stop where they were, so a corner brought back started from
that old value, and the modulation oscillator stopped at a rate of 0 and
started again from wherever it was (audiodsp#207). Now a low-pass's state
follows the signal while it is out, the high-pass's rests at zero, and the
oscillator waits at the phase a new node starts from.

Each case plays noise with the control in, takes it to 0, lets the tail die
on a source of silence (long enough that the node never rests, which would
clear the filters and hide the difference), brings the control back and
prints every block. With nothing playing, what follows must be exact silence,
and that is asserted as well as printed. A last case brings the control back
under live input, so the interpreters are held to the same bytes there too.
"""

import sys
from array import array

import audiocore

verb = __import__(sys.argv[1] if len(sys.argv) > 1 else "audioverb")

RATE = 8000
FRAMES = 256


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def noise(frames, channels, seed=7, level=12000):
    values = array("h")
    state = seed
    for _ in range(frames * channels):
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        values.append(((state >> 8) % (2 * level + 1)) - level)
    return values


def raw(values, channels):
    return audiocore.RawSample(values, sample_rate=RATE,
                               channel_count=channels)


def pull(node):
    return bytes(audiocore.get_buffer(node)[1])


FAILED = []


def check(tag, ok):
    print("check", tag, "ok" if ok else "FAILED")
    if not ok:
        FAILED.append(tag)


CASES = (
    ("bandwidth", {"bandwidth_hz": 1800.0}, {"bandwidth_hz": 0.0}),
    ("low-cut", {"low_cut_hz": 300.0}, {"low_cut_hz": 0.0}),
    ("damping", {"damping_hz": 900.0}, {"damping_hz": 0.0}),
    ("rate", {"mod_rate_hz": 1.7}, {"mod_rate_hz": 0.0}),
)
BASE = {"decay": 0.6, "diffusion": 0.7, "mix": 1.0, "mod_depth_ms": 2.0}


for channels in (2, 1):
    for name, on, off in CASES:
        tag = "%s-%dch" % (name, channels)
        options = dict(BASE)
        options.update(on)
        node = verb.Tank(sample_rate=RATE, channel_count=channels,
                         max_predelay_ms=20.0, **options)
        node.play(raw(noise(10 * FRAMES, channels), channels))
        playing = [pull(node) for _ in range(8)]
        node.set(**off)
        out = [pull(node) for _ in range(2)]
        node.play(raw(array("h", bytes(2 * 300 * FRAMES * channels)),
                      channels))
        died = 0
        quiet = 0
        while quiet < 4 and died < 280:
            quiet = quiet + 1 if not any(pull(node)) else 0
            died += 1
        node.set(**on)
        back = [pull(node) for _ in range(3)]
        print("tank-return", tag, [checksum(b) for b in playing + out],
              died, [checksum(b) for b in back])
        check(tag + " dies", quiet == 4)
        check(tag + " back in silence", not any(any(b) for b in back))
        # Off again, then back under live input.
        node.set(**off)
        node.play(raw(noise(6 * FRAMES, channels, seed=13), channels))
        live = [pull(node) for _ in range(2)]
        node.set(**on)
        live += [pull(node) for _ in range(4)]
        print("tank-return", tag, "live", [checksum(b) for b in live])
        node.deinit()

print("done tank-return")
if FAILED:
    raise SystemExit("failed: " + " ".join(FAILED))
