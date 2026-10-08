"""A host reset in the middle of a stream keeps the frames already taken.

    host_reset_probe.py

The rule (audiodsp#181): `audiocore.reset_buffer(node)` clears what the node
holds of its own -- its history, its tail, its filter memory -- and keeps the
source frames it has already taken from its source, which is what
CircuitPython's own effects do on a reset and what `clear()` always did. The
source was not reset, so a node that dropped them would skip that much of it.

Each node below is fed one long buffer, pulled for one block, reset, and
pulled for two more. Those two blocks must be exactly what a *fresh* node
renders from the source frames the first block had not used: a fresh node
because the reset cleared the node's own state, and from there because no
frame was lost or played twice. A node that dropped its held frames plays the
source from its top again (a `RawSample` that has handed out its one buffer
starts over), so it fails here by name.

`SampleHold` is the one node held to the other answer, on purpose. It ends
when its source ends and reports that end as its own, like CircuitPython's
`audiospeed.SpeedChanger`, so a reset is how a host that loops it starts it
again: it rewinds its source, and after a reset it must render what a fresh
node renders from the top.

Every line prints the PCM it compared as well, so `verify_dsp.py` holds the
three interpreters to the same bytes too.
"""

import sys
from array import array

import audiobiquad
import audioconvolve
import audiocore
import audiodynamics
import audioecho
import audioladder
import audiomath
import audiomodal
import audioroute
import audioshaper
import audioverb

RATE = 48000
CHANNELS = 2
PCM = {"sample_rate": RATE, "channel_count": CHANNELS}
FRAMES = 2048


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def ramp(start=0, count=FRAMES, a=97, b=131):
    """A decorrelated stereo signal whose every frame differs from its
    neighbours, so an offset of even one frame changes the bytes."""
    values = array("h")
    for frame in range(start, count):
        values.append((((frame * a) % 2001) - 1000) * 12)
        values.append((((frame * b) % 1777) - 888) * 13)
    return values


def raw(start=0, a=97, b=131):
    return audiocore.RawSample(ramp(start, a=a, b=b), sample_rate=RATE,
                               channel_count=CHANNELS)


_IMPULSE = array("h", [20000, 0, 0, -9000, 0, 4000, 0, 0,
                       -2000, 0, 1000, 0, 0, 0, -500, 0])
_CURVE = array("h", [-32768, -20000, 0, 20000, 32767])


def modal_bank():
    bank = audiomodal.Bank(modes=2, **PCM)
    bank.set_mode(0, 440.0, 0.1, 0.5)
    bank.set_mode(1, 1250.0, 0.05, 0.3)
    return bank


#: (name, builder, feed). `feed(node, start)` attaches every input the node
#: takes, each starting at frame `start`.
def _play(node, start):
    node.play(raw(start))


def _play_and_key(node, start):
    node.play(raw(start))
    node.key(raw(start, a=53, b=71))


def _play_and_modulate(node, start):
    node.play(raw(start))
    node.modulate(raw(start, a=29, b=37))


NODES = (
    ("audiobiquad.AllPass", lambda: audiobiquad.AllPass(
        frequency=1000.0, stages=2, **PCM), _play),
    ("audiobiquad.Biquad", lambda: audiobiquad.Biquad(
        mode=audiobiquad.LOW_PASS, frequency=1000.0, Q=0.7071, **PCM), _play),
    ("audioconvolve.Convolver", lambda: audioconvolve.Convolver(
        impulse=_IMPULSE, **PCM), _play),
    ("audiodynamics.Dynamics", lambda: audiodynamics.Dynamics(
        audiodynamics.DYN_COMPRESS, **PCM), _play_and_key),
    ("audioecho.FeedbackDelay", lambda: audioecho.FeedbackDelay(
        max_delay_ms=4, feedback=0.6, mix=0.5, **PCM), _play),
    ("audioladder.Ladder", lambda: audioladder.Ladder(
        cutoff_hz=900.0, **PCM), _play),
    ("audiomath.Multiply", lambda: audiomath.Multiply(**PCM),
     _play_and_modulate),
    ("audiomath.SubOctave", lambda: audiomath.SubOctave(**PCM), _play),
    ("audiomodal.Bank", modal_bank, _play),
    ("audioroute.MidSide", lambda: audioroute.MidSide(width=1.4, **PCM),
     _play),
    ("audioshaper.Waveshaper", lambda: audioshaper.Waveshaper(
        curve=_CURVE, **PCM), _play),
    ("audioverb.Tank", lambda: audioverb.Tank(**PCM), _play),
)


def pull(node, blocks):
    out = b""
    for _ in range(blocks):
        out += bytes(audiocore.get_buffer(node)[1])
    return out


failures = []
width = 2 * CHANNELS

for name, build, feed in NODES:
    node = build()
    feed(node, 0)
    first = pull(node, 1)
    taken = len(first) // width
    audiocore.reset_buffer(node)
    after = pull(node, 2)

    fresh = build()
    feed(fresh, taken)
    expected = pull(fresh, 2)[:len(after)]

    kept = after == expected
    print("reset", name, taken, len(after), checksum(after), kept)
    if not kept:
        failures.append(name)

# SampleHold rewinds its source: after a reset it plays from the top, as a
# fresh node does.
def sample_hold():
    return audioshaper.SampleHold(raw(), num=400, den=217)


node = sample_hold()
pull(node, 1)
audiocore.reset_buffer(node)
after = pull(node, 2)
expected = pull(sample_hold(), 2)[:len(after)]
rewound = after == expected
print("reset audioshaper.SampleHold rewinds", len(after), checksum(after),
      rewound)
if not rewound:
    failures.append("audioshaper.SampleHold")

if failures:
    print("FAIL", " ".join(failures))
    sys.exit(1)
print("host reset: every node kept its source frames")
